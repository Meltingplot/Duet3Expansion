// Host test of the AIN task loop of CoreN2G src/SAME5x_C21/SAMC21/AnalogIn.cpp: the ADC and the SDADC must never convert at the same
// time. On the TOOL1LC the SDADC converts PA07/PA06 (Vref/Vssa, Duet3Expansion src/Platform/Platform.cpp SetupThermistorFilter(VrefPin,
// SdAdcVrefFilterIndex, true)), which ADC0 converts as well; while both converted at once ADC0 read Vssa ~1087 counts instead of ~104 and
// the PT1000 read 8.8 C instead of 24.5 C (lab printer 2026-11-26, forum thread 39227). The file under test is compiled unchanged against
// the stand-ins in stubs/; the ADC, the SDADC and the DMAC are modelled here. See README.md.

#include <AnalogIn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
static unsigned int loopIterations = 0, delays = 0, overlaps = 0, restartsWhileConverting = 0;

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

static void AdvanceTo(uint64_t t) noexcept
{
	for (ModelConverter *conv; (conv = NextToFinish(t)) != nullptr; )
	{
		nowUs = conv->endUs;
		Complete(conv);
	}
	nowUs = t;
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
	void EnableChannel(DmaChannel channel, DmaPriority) noexcept { dma[channel].enabled = true; }
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
};

static const Scenario scenarios[] =
{
	{ "turns", "TOOL1LC channels: ADC0 temp0, board type, Vssa, Vref, Vin; SDADC Vref/Vssa",
		{ 3, 5, 6, 7, 8 }, { 0 }, false, false, 0, 4000 },
	{ "adc0-only", "no SDADC channel (SZP with temp0 on ADC0): ADC0 converts back to back",
		{ 3, 7 }, { }, false, false, 0, 2000 },
	{ "sdadc-enabled-later", "the SDADC channel is enabled while ADC0 is running",
		{ 6, 7, 8 }, { 0 }, false, false, 100, 4000 },
	{ "sdadc-hung", "the SDADC never finishes: ADC0 still gets its turns",
		{ 6, 7, 8 }, { 0 }, false, true, 0, 2000 },
	{ "adc0-hung", "ADC0 never finishes: the SDADC still gets its turns",
		{ 6, 7, 8 }, { 0 }, true, false, 0, 2000 },
};

static const Scenario *scenario = nullptr;

static void ChannelCallback(CallbackParameter p, int32_t reading) noexcept
{
	const unsigned int conv = p.u32 >> 8, chan = p.u32 & 0xFF;
	ChannelStats& s = stats[conv][chan];
	++s.calls;
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

	std::printf("    %u loop iterations, %.1f ms simulated, ADC0 %u starts %u completions max gap %.2f ms, SDADC %u starts %u completions max gap %.2f ms\n"
				"    AnalogIn: %u started, %u completed, %u timed out, %u errors\n",
				loopIterations, (double)(nowUs - 1000000) / 1000.0,
				adc0Conv.starts, adc0Conv.completions, (double)adc0Conv.maxGapUs / 1000.0,
				sdadcConv.starts, sdadcConv.completions, (double)sdadcConv.maxGapUs / 1000.0,
				(unsigned int)started, (unsigned int)completed, (unsigned int)timeouts, (unsigned int)errors);

	check(overlaps == 0, "ADC0 and SDADC never convert at the same time (overlapping starts: " + std::to_string(overlaps) + ")");
	check(restartsWhileConverting == 0, "no converter is started again while it is converting (" + std::to_string(restartsWhileConverting) + ")");

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
		check(mc.completions >= loopIterations / 8, std::string(mc.name) + " converts regularly (" + std::to_string(mc.completions) + " completions)");
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
		check(delays == 0, "the task never falls back to delay() while it has channels to convert (" + std::to_string(delays) + ")");
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
	const uint64_t deadline = nowUs + (uint64_t)timeout * 1000;
	while (!notified)
	{
		ModelConverter * const conv = NextToFinish(deadline);
		if (conv == nullptr)
		{
			nowUs = deadline;
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
	if (loopIterations > 1)
	{
		++delays;
	}
	AdvanceTo(nowUs + (uint64_t)ms * 1000);
}

[[noreturn]] static void RunScenario(const Scenario& s) noexcept
{
	scenario = &s;
	adc0Conv.hung = s.adc0Hung;
	sdadcConv.hung = s.sdadcHung;
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
