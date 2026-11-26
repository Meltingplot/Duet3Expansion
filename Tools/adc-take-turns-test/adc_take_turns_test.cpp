// Host test of the AIN task loop of CoreN2G src/SAME5x_C21/SAMC21/AnalogIn.cpp: the ADC and the SDADC must never convert at the same
// time. On the TOOL1LC the SDADC converts PA07/PA06 (Vref/Vssa, Duet3Expansion src/Platform/Platform.cpp SetupThermistorFilter(VrefPin,
// SdAdcVrefFilterIndex, true)), which ADC0 converts as well; while both converted at once ADC0 read Vssa ~1087 counts instead of ~104 and
// the PT1000 read 8.8 C instead of 24.5 C (lab printer 2026-11-26, forum thread 39227). The file under test is compiled unchanged against
// the stand-ins in stubs/; the ADC, the SDADC and the DMAC are modelled here.
// Since TL3 the test also models the TMC task of the TOOL1LC, which competes for the DMA transfer mutex (DmacManager::GetTransferMutex):
// a TMC UART transfer must never run while an ADC DMA channel is enabled (SAM C20/C21 erratum 1.10.4, DS80000740), and the AIN task
// starts at most one sweep per tick. See README.md.

#include <AnalogIn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

// ---------------------------------------------------------------------------------------------------------------------------------
// The model

struct ModelConverter
{
	const char *name;
	unsigned int id;
	uint32_t durationUs;						// time from SWTRIG to the last result of the sequence
	bool hung;									// the conversion never finishes
	bool converting;
	uint64_t endUs;
	DmaChannel dmaChan;
	unsigned int starts, completions;
	uint64_t lastCompletionUs, maxGapUs;
};

static ModelConverter adc0Conv = { "ADC0", 0, 450, false, false, 0, 0xFF, 0, 0, 0, 0 };
static ModelConverter sdadcConv = { "SDADC", 1, 430, false, false, 0, 0xFF, 0, 0, 0, 0 };
static ModelConverter adc1Conv = { "ADC1", 2, 0, true, false, 0, 0xFF, 0, 0, 0, 0 };		// not used by AnalogIn.cpp, only compared against
static ModelConverter * const converters[] = { &adc0Conv, &sdadcConv };

Adc modelAdc0 = { &adc0Conv, { 0 }, { 0 }, { { &adc0Conv } }, 0 };
Adc modelAdc1 = { &adc1Conv, { 0 }, { 0 }, { { &adc1Conv } }, 0 };
Sdadc modelSdadc = { &sdadcConv, { 0 }, { 0 }, { { &sdadcConv } }, 0 };
Tsens modelTsens = {};
uint32_t modelFuseWord = 0;

struct ModelDmaChannel
{
	bool enabled, interruptEnabled;
	volatile uint16_t *dst;
	uint32_t count;
	DmaCallbackFunction fn;
	CallbackParameter param;
};
static ModelDmaChannel dma[8];

static uint64_t nowUs = 1000000;
static bool notified = false;
static int taskHandleDummy;
static unsigned int loopIterations = 0, overlaps = 0, restartsWhileConverting = 0;
static unsigned int startsInSameTick = 0, startsWithoutMutex = 0, callbacksWithMutex = 0;
static uint32_t lastStartTick = 0;
static bool anyStart = false;

// The DMA transfer mutex and the TMC task of the TOOL1LC that competes for it, as Duet3Expansion src/Movement/StepperDrivers/TMC22xx.cpp
// DoTransaction does it: take the mutex, run a UART transfer on two DMA channels, release the mutex, then delay(2) before the next one.
// TmcTransferUs is about a register read at DriversBaudRate 200000 (src/Config/TOOL1LC.h): 4 bytes out, 8 bytes back, 10 bits each.
// A TMC task that is waiting for the mutex gets it when the AIN task releases it; the AIN task blocks in delay(1) right after that.
enum class Holder : uint8_t { none, ain, tmc };
static Holder holder = Holder::none;

struct TmcModel
{
	bool enabled;
	bool waiting;								// wants the mutex
	bool transferring;							// holds the mutex, its DMA channels are enabled
	uint64_t eventUs;							// when it next wants the mutex, or when its transfer ends
	uint64_t waitStartUs, maxWaitUs;
	unsigned int transfers, overlaps;			// overlaps: transfers that started or ran while an ADC DMA channel was enabled
};
static TmcModel tmc = {};
static uint64_t ainMaxMutexWaitUs = 0;
constexpr uint64_t TmcTransferUs = 650;
constexpr uint64_t TmcPauseUs = 2000;
constexpr uint64_t NoEvent = UINT64_MAX;

// A converter that has been triggered converts until it finishes or is reset. Starting one while the other converts is the fault.
void ModelSoftwareTrigger(ModelConverter *conv) noexcept
{
	++conv->starts;
	if (conv->converting)
	{
		++restartsWhileConverting;
	}
	for (ModelConverter *other : converters)
	{
		if (other != conv && other->converting)
		{
			++overlaps;
		}
	}
	const uint32_t tick = millis();
	if (anyStart && tick == lastStartTick)
	{
		++startsInSameTick;
	}
	anyStart = true;
	lastStartTick = tick;
	conv->converting = true;
	conv->endUs = nowUs + conv->durationUs;
}

void ModelSoftwareReset(ModelConverter *conv) noexcept
{
	conv->converting = false;
}

// The last result of a sequence: the DMAC writes all results and raises the completion interrupt
static void Complete(ModelConverter *conv) noexcept
{
	conv->converting = false;
	++conv->completions;
	if (conv->lastCompletionUs != 0 && nowUs - conv->lastCompletionUs > conv->maxGapUs)
	{
		conv->maxGapUs = nowUs - conv->lastCompletionUs;
	}
	conv->lastCompletionUs = nowUs;
	if (conv->dmaChan >= 8)
	{
		return;
	}
	ModelDmaChannel& ch = dma[conv->dmaChan];
	if (!ch.enabled)
	{
		return;
	}
	for (uint32_t i = 0; i < ch.count; ++i)
	{
		ch.dst[i] = (uint16_t)((conv->id << 12) | (i << 8) | (conv->completions & 0xFF));	// converter, position in the sequence, sequence number
	}
	ch.enabled = false;
	if (ch.interruptEnabled && ch.fn != nullptr)
	{
		ch.fn(ch.param, DmaCallbackReason::complete);			// as the DMAC ISR does
	}
}

static ModelConverter *NextToFinish(uint64_t limit) noexcept
{
	ModelConverter *next = nullptr;
	for (ModelConverter *conv : converters)
	{
		if (conv->converting && !conv->hung && conv->endUs <= limit && (next == nullptr || conv->endUs < next->endUs))
		{
			next = conv;
		}
	}
	return next;
}

static bool AdcDmaEnabled() noexcept
{
	for (const ModelConverter *conv : converters)
	{
		if (conv->dmaChan < 8 && dma[conv->dmaChan].enabled)
		{
			return true;
		}
	}
	return false;
}

static void TmcStartTransfer() noexcept
{
	holder = Holder::tmc;
	tmc.waiting = false;
	tmc.transferring = true;
	++tmc.transfers;
	if (nowUs - tmc.waitStartUs > tmc.maxWaitUs)
	{
		tmc.maxWaitUs = nowUs - tmc.waitStartUs;
	}
	if (AdcDmaEnabled())
	{
		++tmc.overlaps;
	}
	tmc.eventUs = nowUs + TmcTransferUs;
}

static void TmcEvent() noexcept
{
	nowUs = tmc.eventUs;
	if (tmc.transferring)
	{
		// The transfer has finished: DoTransaction releases the mutex and calls delay(2)
		tmc.transferring = false;
		if (holder == Holder::tmc)
		{
			holder = Holder::none;
		}
		tmc.eventUs = nowUs + TmcPauseUs;
	}
	else
	{
		// DoTransaction wants the mutex. Without the fix nobody else takes it, so the transfer starts at once.
		tmc.waitStartUs = nowUs;
		if (holder == Holder::none)
		{
			TmcStartTransfer();
		}
		else
		{
			tmc.waiting = true;
			tmc.eventUs = NoEvent;
		}
	}
}

static void AdvanceTo(uint64_t t) noexcept
{
	for (;;)
	{
		ModelConverter * const conv = NextToFinish(t);
		const uint64_t tmcEventUs = tmc.enabled ? tmc.eventUs : NoEvent;
		if (conv != nullptr && conv->endUs <= tmcEventUs)
		{
			nowUs = conv->endUs;
			Complete(conv);
		}
		else if (tmcEventUs <= t)
		{
			TmcEvent();
		}
		else
		{
			break;
		}
	}
	nowUs = t;
}

static uint64_t NextTickUs(uint32_t ticks) noexcept
{
	return ((nowUs / 1000) + ticks) * 1000;					// FreeRTOS counts a delay or timeout from the current tick
}

// The AIN task's side of the mutex. A TMC transfer that holds it runs to its end first.
Mutex transferMutex;
Mutex& DmacManager::GetTransferMutex() noexcept { return transferMutex; }

bool Mutex::Take(uint32_t) noexcept
{
	const uint64_t start = nowUs;
	while (holder == Holder::tmc)
	{
		AdvanceTo(tmc.eventUs);
	}
	if (nowUs - start > ainMaxMutexWaitUs)
	{
		ainMaxMutexWaitUs = nowUs - start;
	}
	holder = Holder::ain;
	return true;
}

bool Mutex::Release() noexcept
{
	holder = Holder::none;
	if (tmc.enabled && tmc.waiting)
	{
		TmcStartTransfer();
	}
	return true;
}

uint32_t millis() noexcept { return (uint32_t)(nowUs / 1000); }

namespace DmacManager
{
	void SetBtctrl(DmaChannel, uint32_t) noexcept { }
	void SetSourceAddress(DmaChannel, const volatile void *const) noexcept { }
	void SetDestinationAddress(DmaChannel channel, volatile void *const dst) noexcept { dma[channel].dst = static_cast<volatile uint16_t *>(dst); }
	void SetDataLength(DmaChannel channel, uint32_t amount) noexcept { dma[channel].count = amount; }
	void SetTriggerSource(DmaChannel channel, DmaTrigSource source) noexcept
	{
		((source == DmaTrigSource::sdadc_resrdy) ? sdadcConv : adc0Conv).dmaChan = channel;
	}
	void EnableChannel(DmaChannel channel, DmaPriority) noexcept
	{
		dma[channel].enabled = true;
		if (holder != Holder::ain)
		{
			++startsWithoutMutex;
		}
		if (tmc.transferring)
		{
			++tmc.overlaps;
		}
	}
	bool DisableChannel(DmaChannel channel) noexcept			// on the SAMC21 this also disables the channel interrupts
	{
		dma[channel].enabled = false;
		dma[channel].interruptEnabled = false;
		return true;
	}
	void SetInterruptCallback(DmaChannel channel, DmaCallbackFunction fn, CallbackParameter param) noexcept { dma[channel].fn = fn; dma[channel].param = param; }
	void EnableCompletedInterrupt(DmaChannel channel) noexcept { dma[channel].interruptEnabled = true; }
	void DisableCompletedInterrupt(DmaChannel channel) noexcept { dma[channel].interruptEnabled = false; }
	uint32_t GetAndClearChannelStatus(DmaChannel) noexcept { return 0; }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Scenarios

struct ChannelStats
{
	unsigned int expectedPosition;
	unsigned int calls, wrongSlot, duplicates;
	int lastSequence;
};
static ChannelStats stats[2][12];

struct Scenario
{
	const char *name;
	const char *description;
	std::vector<unsigned int> adc0Channels, sdadcChannels;
	bool adc0Hung, sdadcHung;
	unsigned int enableSdadcAtIteration;		// 0 = enable the SDADC channels before the task starts
	unsigned int iterations;
	bool tmcTask;								// the TMC task of the TOOL1LC competes for the DMA transfer mutex
};

static const Scenario scenarios[] =
{
	{ "turns", "TOOL1LC channels: ADC0 temp0, board type, Vssa, Vref, Vin; SDADC Vref/Vssa; TMC task",
		{ 3, 5, 6, 7, 8 }, { 0 }, false, false, 0, 4000, true },
	{ "adc0-only", "no SDADC channel and no TMC task (SZP with temp0 on ADC0): ADC0 converts once per tick",
		{ 3, 7 }, { }, false, false, 0, 2000, false },
	{ "sdadc-enabled-later", "the SDADC channel is enabled while ADC0 is running; TMC task",
		{ 6, 7, 8 }, { 0 }, false, false, 100, 4000, true },
	{ "sdadc-hung", "the SDADC never finishes: ADC0 and the TMC task still get their turns",
		{ 6, 7, 8 }, { 0 }, false, true, 0, 2000, true },
	{ "adc0-hung", "ADC0 never finishes: the SDADC and the TMC task still get their turns",
		{ 6, 7, 8 }, { 0 }, true, false, 0, 2000, true },
};

static const Scenario *scenario = nullptr;

static void ChannelCallback(CallbackParameter p, int32_t reading) noexcept
{
	const unsigned int conv = p.u32 >> 8, chan = p.u32 & 0xFF;
	ChannelStats& s = stats[conv][chan];
	++s.calls;
	if (holder == Holder::ain)
	{
		++callbacksWithMutex;
	}
	const unsigned int id = ((uint32_t)reading >> 12) & 0x0F, position = ((uint32_t)reading >> 8) & 0x0F, sequence = (uint32_t)reading & 0xFF;
	if (id != conv || position != s.expectedPosition)
	{
		++s.wrongSlot;
	}
	if ((int)sequence == s.lastSequence)
	{
		++s.duplicates;
	}
	s.lastSequence = (int)sequence;
}

static void EnableChannels(unsigned int conv, const std::vector<unsigned int>& channels) noexcept
{
	unsigned int position = 0;
	for (unsigned int chan = 0; chan < 12; ++chan)
	{
		for (unsigned int c : channels)
		{
			if (c == chan)
			{
				stats[conv][chan].expectedPosition = position++;
				stats[conv][chan].lastSequence = -1;
			}
		}
	}
	for (unsigned int chan : channels)
	{
		const AdcInput input = (AdcInput)(((conv == 0) ? (unsigned int)AdcInput::adc0_0 : (unsigned int)AdcInput::sdadc_0) + chan);
		if (!AnalogIn::EnableChannel(input, ChannelCallback, CallbackParameter((uint32_t)((conv << 8) | chan)), 0))
		{
			std::printf("    cannot enable channel %u of %s\n", chan, converters[conv]->name);
			std::_Exit(2);
		}
	}
}

[[noreturn]] static void Finish() noexcept
{
	uint32_t started, completed, timeouts, errors;
	AnalogIn::GetDebugInfo(started, completed, timeouts, errors);
	unsigned int failures = 0;
	auto check = [&failures](bool ok, const std::string& what)
	{
		std::printf("    %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok) { ++failures; }
	};

	const uint64_t simulatedMs = (nowUs - 1000000) / 1000;
	std::printf("    %u loop iterations, %u ms simulated, ADC0 %u starts %u completions max gap %.2f ms, SDADC %u starts %u completions max gap %.2f ms\n"
				"    AnalogIn: %u started, %u completed, %u timed out, %u errors\n",
				loopIterations, (unsigned int)simulatedMs,
				adc0Conv.starts, adc0Conv.completions, (double)adc0Conv.maxGapUs / 1000.0,
				sdadcConv.starts, sdadcConv.completions, (double)sdadcConv.maxGapUs / 1000.0,
				(unsigned int)started, (unsigned int)completed, (unsigned int)timeouts, (unsigned int)errors);
	if (scenario->tmcTask)
	{
		std::printf("    TMC task: %u transfers, longest wait for the mutex %u us; AIN task: longest wait for the mutex %u us\n",
					tmc.transfers, (unsigned int)tmc.maxWaitUs, (unsigned int)ainMaxMutexWaitUs);
	}

	check(overlaps == 0, "ADC0 and SDADC never convert at the same time (overlapping starts: " + std::to_string(overlaps) + ")");
	check(restartsWhileConverting == 0, "no converter is started again while it is converting (" + std::to_string(restartsWhileConverting) + ")");
	check(startsInSameTick == 0, "at most one conversion starts per tick (" + std::to_string(startsInSameTick) + " more)");
	check(startsWithoutMutex == 0, "an ADC DMA channel is enabled only while the AIN task holds the DMA transfer mutex ("
			+ std::to_string(startsWithoutMutex) + " without)");
	check(callbacksWithMutex == 0, "the callbacks run after the AIN task has released the mutex (" + std::to_string(callbacksWithMutex) + " with)");
	if (scenario->tmcTask)
	{
		const bool hangs = scenario->adc0Hung || scenario->sdadcHung;
		check(tmc.overlaps == 0, "no TMC transfer runs while an ADC DMA channel is enabled (" + std::to_string(tmc.overlaps) + ")");
		// The TMC task waits for one sweep at most, or for the conversion timeout (AdcConversionTimeout, 5 ticks) and one tick if a converter hangs
		const uint64_t tmcWaitLimitUs = hangs ? 6000 : std::max(adc0Conv.durationUs, sdadcConv.durationUs);
		check(tmc.maxWaitUs <= tmcWaitLimitUs, "the TMC task waits at most " + std::to_string(tmcWaitLimitUs) + " us for the mutex ("
				+ std::to_string(tmc.maxWaitUs) + " us)");
		check(ainMaxMutexWaitUs <= TmcTransferUs, "the AIN task waits at most one TMC transfer for the mutex (" + std::to_string(ainMaxMutexWaitUs) + " us)");
		// One transfer and delay(2) take about 2.65 ms, a sweep may come in between. A hung converter holds the mutex for its timeout each turn.
		const uint64_t minTransfers = simulatedMs / (hangs ? 8 : 4);
		check(tmc.transfers >= minTransfers, "the TMC task transfers regularly (" + std::to_string(tmc.transfers) + " transfers, at least "
				+ std::to_string(minTransfers) + ")");
	}

	const bool adc0Used = !scenario->adc0Channels.empty(), sdadcUsed = !scenario->sdadcChannels.empty();
	for (unsigned int conv = 0; conv < 2; ++conv)
	{
		ModelConverter& mc = *converters[conv];
		const bool used = (conv == 0) ? adc0Used : sdadcUsed;
		const bool hung = (conv == 0) ? scenario->adc0Hung : scenario->sdadcHung;
		const std::vector<unsigned int>& channels = (conv == 0) ? scenario->adc0Channels : scenario->sdadcChannels;
		if (!used)
		{
			check(mc.starts == 0, std::string(mc.name) + " without channels is never started");
			continue;
		}
		if (hung)
		{
			check(timeouts > 0, std::string(mc.name) + " hung: its conversions time out");
			continue;
		}
		// Both converters used: they take turns, one sweep per tick at most. One converter: up to one sweep per tick. If the other converter
		// hangs, it holds the turn for the timeout (5 ticks) and one more tick each time.
		const bool otherHung = (conv == 0) ? scenario->sdadcHung : scenario->adc0Hung;
		const uint64_t minCompletions = otherHung ? simulatedMs / 8 : simulatedMs / (((adc0Used && sdadcUsed) ? 2 : 1) * 2);
		check(mc.completions >= minCompletions, std::string(mc.name) + " converts regularly (" + std::to_string(mc.completions) + " completions, at least "
				+ std::to_string(minCompletions) + ")");
		// A converter must not wait long for its turn: the other one takes at most its conversion time, or the timeout if it hangs
		check(mc.maxGapUs <= 12000, std::string(mc.name) + " waits at most 12 ms between results (" + std::to_string(mc.maxGapUs) + " us)");
		for (unsigned int chan : channels)
		{
			const ChannelStats& s = stats[conv][chan];
			const std::string label = std::string(mc.name) + " channel " + std::to_string(chan);
			check(s.wrongSlot == 0, label + ": every reading comes from its own slot of the sequence (" + std::to_string(s.wrongSlot) + " wrong)");
			check(s.duplicates == 0, label + ": no result is delivered twice (" + std::to_string(s.duplicates) + " duplicates)");
			check(s.calls + 1 >= mc.completions && s.calls <= mc.completions, label + ": one reading per completed conversion ("
					+ std::to_string(s.calls) + " readings, " + std::to_string(mc.completions) + " conversions)");
		}
	}
	if (!scenario->adc0Hung && !scenario->sdadcHung)
	{
		check(timeouts == 0 && errors == 0, "no timeouts and no errors");
	}
	std::fflush(stdout);
	std::_Exit(failures == 0 ? 0 : 1);
}

static void LoopStep() noexcept
{
	++loopIterations;
	if (scenario->enableSdadcAtIteration != 0 && loopIterations == scenario->enableSdadcAtIteration)
	{
		EnableChannels(1, scenario->sdadcChannels);		// as another task does it when a sensor is configured
	}
	if (loopIterations >= scenario->iterations)
	{
		Finish();
	}
}

TaskHandle TaskBase::GetCallerTaskHandle() noexcept { return &taskHandleDummy; }
void TaskBase::GiveFromISR(TaskHandle, uint32_t) noexcept { notified = true; }

bool TaskBase::TakeIndexed(uint32_t, uint32_t timeout) noexcept
{
	LoopStep();
	const uint64_t deadline = NextTickUs(timeout);
	while (!notified)
	{
		ModelConverter * const conv = NextToFinish(deadline);
		if (conv == nullptr)
		{
			AdvanceTo(deadline);
			return false;
		}
		AdvanceTo(conv->endUs);
	}
	notified = false;
	return true;
}

void delay(uint32_t ms) noexcept
{
	LoopStep();
	AdvanceTo(NextTickUs(ms));
}

[[noreturn]] static void RunScenario(const Scenario& s) noexcept
{
	scenario = &s;
	adc0Conv.hung = s.adc0Hung;
	sdadcConv.hung = s.sdadcHung;
	tmc.enabled = s.tmcTask;
	tmc.eventUs = nowUs + 300;								// the first TMC transfer comes in the middle of a tick
	AnalogIn::Init(2, 2, false);							// TOOL1LC: DmacChanAdc0Rx 2 (SDADC gets 3), DmacPrioAdcRx 2
	EnableChannels(0, s.adc0Channels);
	if (s.enableSdadcAtIteration == 0)
	{
		EnableChannels(1, s.sdadcChannels);
	}
	AnalogIn::TaskLoop(nullptr);
}

int main(int argc, char **argv)
{
	unsigned int failed = 0, run = 0;
	for (const Scenario& s : scenarios)
	{
		if (argc > 1 && std::strcmp(argv[1], s.name) != 0)
		{
			continue;
		}
		++run;
		std::printf("== %s: %s\n", s.name, s.description);
		std::fflush(stdout);
		const pid_t pid = fork();
		if (pid == 0)
		{
			RunScenario(s);
		}
		int status = 0;
		waitpid(pid, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		{
			++failed;
			std::printf("    scenario %s FAILED\n", s.name);
		}
	}
	std::printf("%u of %u scenarios passed\n", run - failed, run);
	return (run != 0 && failed == 0) ? 0 : 1;
}
