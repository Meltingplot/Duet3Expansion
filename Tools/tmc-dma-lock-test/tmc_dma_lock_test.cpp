// Host test of DoTransaction (src/Movement/StepperDrivers/TMC22xx.cpp) as the TOOL1LC builds it. The function must hold the DMA transfer
// mutex (CoreN2G DmacManager::GetTransferMutex) from before it enables the TMC DMA channels until they are off again, and must not hold it
// in delay(2) or while it processes the reply. SAM C20/C21 erratum 1.10.4 (DS80000740): with the triggers of several DMA channels active
// at the same time the DMAC may corrupt the write-back descriptors, and on the TOOL1LC it overwrote the one of the ADC channel with the
// state of the TMC transmit channel (finding TL3). The AIN task holds the same mutex while an ADC channel is enabled, which
// Tools/adc-take-turns-test checks. The Makefile extracts the real text of DoTransaction; this file models what it calls. See README.md.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

// As src/Config/TOOL1LC.h sets them for the build, and the processor
#define SAMC21					1
#define RP2040					0
#define TMC22xx_USES_SERCOM		1
#define TMC22xx_HAS_MUX			0
#define TMC22xx_SINGLE_DRIVER	1
#define TMC22xx_USE_SLAVEADDR	0

typedef uint8_t DmaChannel;
#include "Config.inc"											// TransferTimeout and DmacChanTmcRx of the TOOL1LC

// CoreN2G src/DmacManager.h
enum class DmaCallbackReason : uint8_t { none = 0, error = 1, complete = 2, completeAndError = 3 };

// CoreN2G src/CoreNotifyIndices.h and Duet3Expansion src/AppNotifyIndices.h: only the index DoTransaction waits on
namespace NotifyIndices { constexpr uint32_t Tmc = 2; }

// ---------------------------------------------------------------------------------------------------------------------------------
// The model

enum class Outcome { complete, timeout, error, completeAndError };

static Outcome outcome;
static bool mutexHeld = false;
static unsigned int takes = 0, releases = 0;
static bool dmaActive = false;									// the TMC DMA channels are enabled
static unsigned int startsWithoutMutex = 0, waitsWithoutMutex = 0, abortsWithoutMutex = 0, releasesWithDmaActive = 0;
static unsigned int delaysWithMutex = 0, handlerWithMutex = 0;
static unsigned int handlerCalls = 0, timeoutCalls = 0, dmaErrorCalls = 0, delays = 0;
static uint32_t lastTimeout = 0;

// RRFLibraries src/RTOSIface/RTOSIface.h
class Mutex
{
public:
	static constexpr uint32_t TimeoutUnlimited = 0xFFFFFFFFu;
	bool Take(uint32_t = TimeoutUnlimited) noexcept { mutexHeld = true; ++takes; return true; }
	bool Release() noexcept
	{
		if (dmaActive)
		{
			++releasesWithDmaActive;
		}
		mutexHeld = false;
		++releases;
		return true;
	}
};

// As RRFLibraries src/RTOSIface/RTOSIface.cpp: Release is non-counting, the destructor releases what is still held
class MutexLocker
{
public:
	explicit MutexLocker(Mutex& m, uint32_t timeout = Mutex::TimeoutUnlimited) noexcept : handle(&m), acquired(m.Take(timeout)) { }
	~MutexLocker() { Release(); }
	void Release() noexcept { if (acquired) { handle->Release(); acquired = false; } }
	MutexLocker(const MutexLocker&) = delete;
	MutexLocker& operator=(const MutexLocker&) = delete;
private:
	Mutex *handle;
	bool acquired;
};

static Mutex transferMutex;

namespace DmacManager
{
	Mutex& GetTransferMutex() noexcept { return transferMutex; }
	void DisableCompletedInterrupt(DmaChannel) noexcept { }
}

static DmaCallbackReason dmaFinishedReason;						// TMC22xx.cpp, written by TransferCompleteCallback

namespace TaskBase
{
	void ClearCurrentTaskNotifyCount(uint32_t) noexcept { }

	// The TMC task waits for TransferCompleteCallback. It runs when the receive channel has finished or has had an error.
	bool TakeIndexed(uint32_t index, uint32_t timeout) noexcept
	{
		lastTimeout = timeout;
		if (!mutexHeld)
		{
			++waitsWithoutMutex;
		}
		switch (outcome)
		{
		case Outcome::complete:
			dmaActive = false;									// both channels have transferred their blocks, so the DMAC has disabled them
			dmaFinishedReason = DmaCallbackReason::complete;
			return true;
		case Outcome::error:
			dmaFinishedReason = DmaCallbackReason::error;		// the channels stay enabled until AbortTransfer
			return true;
		case Outcome::completeAndError:
			dmaFinishedReason = DmaCallbackReason::completeAndError;
			return true;
		case Outcome::timeout:
		default:
			return false;
		}
	}
}

void delay(uint32_t) noexcept
{
	++delays;
	if (mutexHeld)
	{
		++delaysWithMutex;
	}
}

// The members of TmcDriverState that DoTransaction calls. TransferTimedOut and DmaError call AbortTransfer, which disables both channels.
class TmcDriverState
{
public:
	void StartTransfer() noexcept
	{
		if (!mutexHeld)
		{
			++startsWithoutMutex;
		}
		dmaActive = true;
	}
	void TransferTimedOut() noexcept { ++timeoutCalls; Abort(); }
	void DmaError() noexcept { ++dmaErrorCalls; Abort(); }
	void UartTmcHandler() noexcept
	{
		++handlerCalls;
		if (mutexHeld)
		{
			++handlerWithMutex;
		}
	}

private:
	void Abort() noexcept
	{
		if (!mutexHeld)
		{
			++abortsWithoutMutex;
		}
		dmaActive = false;
	}
};

static TmcDriverState driverStates[1];

// The function under test
#include "TMC22xx.inc"

// ---------------------------------------------------------------------------------------------------------------------------------
// Scenarios

struct Scenario
{
	const char *name;
	Outcome outcome;
	bool expectedResult;
	unsigned int expectedHandler, expectedTimeout, expectedDmaError;
	unsigned int expectedDelays;				// delay(2) follows a completed transfer and a DMA error, not a timeout, which has waited already
};

static const Scenario scenarios[] =
{
	{ "complete", Outcome::complete, true, 1, 0, 0, 1 },
	{ "timeout", Outcome::timeout, false, 0, 1, 0, 0 },
	{ "dma-error", Outcome::error, false, 0, 0, 1, 1 },
	{ "complete-and-error", Outcome::completeAndError, false, 0, 0, 1, 1 },
};

int main()
{
	unsigned int failedScenarios = 0;
	for (const Scenario& s : scenarios)
	{
		outcome = s.outcome;
		mutexHeld = dmaActive = false;
		takes = releases = startsWithoutMutex = waitsWithoutMutex = abortsWithoutMutex = releasesWithDmaActive = 0;
		delaysWithMutex = handlerWithMutex = handlerCalls = timeoutCalls = dmaErrorCalls = delays = 0;
		dmaFinishedReason = DmaCallbackReason::none;

		const bool result = DoTransaction(0);

		unsigned int failures = 0;
		auto check = [&failures](bool ok, const std::string& what)
		{
			std::printf("    %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
			if (!ok) { ++failures; }
		};
		std::printf("== %s\n", s.name);
		check(result == s.expectedResult, std::string("DoTransaction returns ") + (s.expectedResult ? "true" : "false"));
		check(handlerCalls == s.expectedHandler && timeoutCalls == s.expectedTimeout && dmaErrorCalls == s.expectedDmaError,
				"UartTmcHandler " + std::to_string(handlerCalls) + ", TransferTimedOut " + std::to_string(timeoutCalls) + ", DmaError "
				+ std::to_string(dmaErrorCalls) + " times, as before the fix");
		check(lastTimeout == TransferTimeout, "it waits TransferTimeout ticks (" + std::to_string(lastTimeout) + ")");
		check(delays == s.expectedDelays, std::to_string(s.expectedDelays) + " delay(2) after the transaction, as before the fix (" + std::to_string(delays) + ")");
		check(startsWithoutMutex == 0, "the DMA channels are enabled only while the mutex is held");
		check(waitsWithoutMutex == 0, "the mutex is held while the transfer runs");
		check(abortsWithoutMutex == 0, "the channels are disabled before the mutex is released");
		check(releasesWithDmaActive == 0, "the mutex is never released while a channel is enabled");
		check(delaysWithMutex == 0, "the mutex is not held in delay(2)");
		check(handlerWithMutex == 0, "the mutex is not held while the reply is processed");
		check(takes == 1 && releases == 1 && !mutexHeld, "taken once and released once (" + std::to_string(takes) + " / " + std::to_string(releases) + ")");
		check(!dmaActive, "the channels are off when DoTransaction returns");
		if (failures != 0)
		{
			++failedScenarios;
			std::printf("    scenario %s FAILED\n", s.name);
		}
	}
	const unsigned int run = sizeof(scenarios) / sizeof(scenarios[0]);
	std::printf("%u of %u scenarios passed\n", run - failedScenarios, run);
	return (failedScenarios == 0) ? 0 : 1;
}
