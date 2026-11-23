// Host test of CanInterface::UpdateSyncLockState (src/CAN/CanInterface.cpp), which switches all heaters off when the board has lost
// time sync with the main board for longer than the connection timeout. The function is the real text, extracted by the Makefile;
// its file-static state, the clock, the time of the last sync message and Heat::SwitchOffAll are stand-ins. See README.md.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

// ---------------------------------------------------------------------------------------------------------------------------------
// The clock returns the time that it read, then runs the writer that the case has set up, if any, one tick later: the writer has
// preempted the caller right after the clock read.

static uint32_t millisNow;
static std::function<void()> writerAfterClockRead;

uint32_t millis() noexcept
{
	const uint32_t now = millisNow;
	++millisNow;
	if (writerAfterClockRead)
	{
		const std::function<void()> writer = writerAfterClockRead;
		writerAfterClockRead = nullptr;
		writer();
	}
	return now;
}

namespace StepTimer
{
	static volatile uint32_t whenLastSynced;			// src/Movement/StepTimer.h:130, written when a sync message is processed (StepTimer.cpp:256)
	uint32_t GetWhenLastSynced() noexcept { return whenLastSynced; }
}

namespace Heat
{
	static unsigned int switchOffAllCalls;
	void SwitchOffAll() noexcept { ++switchOffAllCalls; }
}

namespace CanInterface
{
	void UpdateSyncLockState(bool synced) noexcept;
}

// The file-static state of src/CAN/CanInterface.cpp:156-169
static bool mainBoardAcknowledgedAnnounce = false;
static uint32_t connectionTimeoutMillis = 1000;
static bool hadSyncLock = false;
static bool syncLockLost = false;
static bool timeoutActioned = false;
static bool announceIsReconnect = false;
static bool announceWasShutDown = false;
static unsigned int syncLossCount = 0;
static unsigned int timeoutShutdownCount = 0;

// ---------------------------------------------------------------------------------------------------------------------------------
// The real code

#include "CanInterface.inc"

// ---------------------------------------------------------------------------------------------------------------------------------

static unsigned int runs = 0, failures = 0;

static void Expect(bool ok, const std::string& name, const char *what) noexcept
{
	++runs;
	std::printf("%s %s: %s\n", (ok) ? "PASS" : "FAIL", name.c_str(), what);
	if (!ok)
	{
		++failures;
	}
}

enum class Arrival { recent, tooOld, newerDuringCheck };

static void RunCase(Arrival a) noexcept
{
	const std::string name = (a == Arrival::recent) ? "sync lost, last sync message 100 ms ago"
								: (a == Arrival::tooOld) ? "sync lost, last sync message 1500 ms ago"
									: "sync lost, last sync message 100 ms ago and a newer one during the check";
	millisNow = 5000000;
	hadSyncLock = true;
	syncLockLost = timeoutActioned = false;
	syncLossCount = timeoutShutdownCount = 0;
	Heat::switchOffAllCalls = 0;
	StepTimer::whenLastSynced = (a == Arrival::tooOld) ? millisNow - 1500 : millisNow - 100;
	if (a == Arrival::newerDuringCheck)
	{
		// StepTimer::ProcessTimeSyncMessage stores millis() when a sync message arrives (src/Movement/StepTimer.cpp:256), in the CanClock task
		writerAfterClockRead = []() { StepTimer::whenLastSynced = millis(); };
	}

	// The board is still resyncing, so Platform::Spin reports it as not synced (src/Platform/Platform.cpp:1130)
	CanInterface::UpdateSyncLockState(false);
	writerAfterClockRead = nullptr;

	const bool expectOff = (a == Arrival::tooOld);
	Expect(syncLockLost && syncLossCount == 1, name, "expected one sync loss recorded");
	Expect(Heat::switchOffAllCalls == ((expectOff) ? 1u : 0u) && timeoutActioned == expectOff && timeoutShutdownCount == ((expectOff) ? 1u : 0u), name,
			(expectOff) ? "expected the heaters switched off once" : "expected the heaters left on");
}

int main()
{
	RunCase(Arrival::recent);
	RunCase(Arrival::tooOld);
	RunCase(Arrival::newerDuringCheck);
	std::printf("%u runs, %u failed\n", runs, failures);
	return (failures == 0) ? 0 : 1;
}
