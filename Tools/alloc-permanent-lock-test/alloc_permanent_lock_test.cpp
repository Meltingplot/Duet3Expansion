// Host test of the lock that keeps Tasks::AllocPermanent (src/Platform/Tasks.cpp) from racing malloc, compiled from the real text of
// the malloc mutex functions and Tasks::AllocPermanent, and of _sbrk and CoreAllocPermanent from CoreN2G's src/syscalls.h, that the
// Makefile extracts.
//
// Our malloc (src/libc/nano-mallocr.c) takes the malloc mutex through GetMallocMutex and grows the heap with _sbrk, which raises heapTop
// up to heapLimit. CoreAllocPermanent lowers heapLimit down to heapTop, so the two must not interleave. Up to 3.7-dev 9ac5016c,
// Tasks::AllocPermanent called newlib's __malloc_lock, which does nothing in our builds (bx lr in the TOOL1LC, EXP1HCL and SZP ELF
// files), so a task that allocated permanent memory (MoveSegment, CanMessageBuffer) while another one was inside malloc could hand out
// memory that the other task's _sbrk hands out too. Upstream fixed this in CoreN2G c9b23d7 and Duet3Expansion 0f63349f: _sbrk and
// CoreAllocPermanent each do their check and update inside an AtomicCriticalSectionLocker, and AllocPermanent takes no mutex.
//
// The test plays the interrupt mask, the malloc mutex and the scheduler state. On one thread it checks that _sbrk and AllocPermanent
// change the heap pointers inside the interrupt lock, that AllocPermanent takes no malloc mutex, works while the scheduler is suspended
// or not started and leaves the interrupt mask as it was, and that GetMallocMutex takes the mutex while the scheduler is suspended (so
// FreeRTOS asserts, as upstream d85662412 and 0f63349f intend) but not before it has started. With "threads" it runs malloc's part (the
// mutex and _sbrk) and AllocPermanent on two threads, for ThreadSanitizer to check that they do not race.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#define RP2040	0
#define SAMC21	0										// the assertion of the SAME51 (EXP1HCL); the SAMC21 checks PRIMASK only
#define _ecv_array

static std::vector<std::string> failures;

static void AssertFailed(const char *file, int line) noexcept
{
	failures.push_back(std::string("configASSERT failed at ") + file + ":" + std::to_string(line));
}

#define configASSERT(_expr) do { if (!(_expr)) { AssertFailed(__FILE__, __LINE__); } } while (false)

// FreeRTOS include/task.h
constexpr int taskSCHEDULER_SUSPENDED = 0;
constexpr int taskSCHEDULER_NOT_STARTED = 1;
constexpr int taskSCHEDULER_RUNNING = 2;

static int schedulerState = taskSCHEDULER_RUNNING;
extern "C" int xTaskGetSchedulerState() noexcept { return schedulerState; }

// Per thread, because with "threads" each thread stands for a task with its own interrupt state
static thread_local uint32_t primask = 0, basepri = 0;
static uint32_t __get_PRIMASK() noexcept { return primask; }
static uint32_t __get_BASEPRI() noexcept { return basepri; }

// CoreN2G CoreIO.h:353-372 with Core.h:409-435: saves PRIMASK, disables interrupts and restores PRIMASK when it goes out of scope.
// Played: on one thread it counts and records the heap pointers when it is taken and released. With "threads" it is a global mutex,
// because on the single-core board nothing else runs while interrupts are disabled.
static bool threaded = false;
static std::mutex interruptsDisabled;
static unsigned int irqLocks = 0;
static const char *limitAtIrqLock = nullptr, *limitAtIrqUnlock = nullptr;
static char *topAtIrqLock = nullptr, *topAtIrqUnlock = nullptr;

class AtomicCriticalSectionLocker
{
public:
	AtomicCriticalSectionLocker() noexcept;
	~AtomicCriticalSectionLocker() noexcept;

private:
	uint32_t flags;
};

// The Mutex of RRFLibraries RTOSIface.h, played: see Take and Release below
class Mutex
{
public:
	void Take() noexcept;
	void Release() noexcept;
};

// Newlib's malloc lock. Our builds do not replace it, and newlib's calls __retarget_lock_acquire_recursive, which does nothing
struct _reent;
extern "C" void __malloc_lock(struct _reent *) noexcept { }
extern "C" void __malloc_unlock(struct _reent *) noexcept { }

// What syscalls.h takes from the linker script and the application
char _end;
const char *_ecv_array heapLimit = nullptr;
[[noreturn]] void OutOfMemoryHandler() noexcept
{
	printf("FAIL: OutOfMemoryHandler was called\n");
	exit(4);
}

namespace Tasks
{
	void *AllocPermanent(size_t sz, std::align_val_t align = (std::align_val_t)alignof(uint32_t)) noexcept;
}

// syscalls.h casts pointers to uint32_t, which is too narrow for a pointer on the 64-bit host
#define uint32_t uintptr_t
#include "syscalls.inc"
#undef uint32_t
#include "Tasks.inc"

AtomicCriticalSectionLocker::AtomicCriticalSectionLocker() noexcept : flags(primask)
{
	if (threaded)
	{
		interruptsDisabled.lock();
	}
	else
	{
		++irqLocks;
		limitAtIrqLock = heapLimit;
		topAtIrqLock = heapTop;
	}
	primask = 1;
}

AtomicCriticalSectionLocker::~AtomicCriticalSectionLocker() noexcept
{
	if (threaded)
	{
		primask = flags;
		interruptsDisabled.unlock();
		return;
	}
	limitAtIrqUnlock = heapLimit;
	topAtIrqUnlock = heapTop;
	primask = flags;
}

// The mutex. On one thread it counts and, like FreeRTOS, asserts when it is taken while the scheduler is suspended: Mutex::Take waits
// without a time limit (RTOSIface.h:113), and xQueueSemaphoreTake asserts that it is not asked to wait while the scheduler is suspended
// (FreeRTOS src/queue.c:1682). With "threads" it is a real recursive mutex.
static std::recursive_mutex realMutex;
static unsigned int takes = 0, releases = 0;

void Mutex::Take() noexcept
{
	if (threaded)
	{
		realMutex.lock();
		return;
	}
	++takes;
	if (schedulerState == taskSCHEDULER_SUSPENDED)
	{
		AssertFailed("FreeRTOS src/queue.c", 1682);
	}
}

void Mutex::Release() noexcept
{
	if (threaded)
	{
		realMutex.unlock();
		return;
	}
	++releases;
}

alignas(8) static char arena[1 << 20];
static unsigned int passed = 0, failed = 0;

static void Reset() noexcept
{
	heapTop = arena + 1024;
	heapLimit = arena + 3072;
	schedulerState = taskSCHEDULER_RUNNING;
	primask = basepri = 0;
	irqLocks = 0;
	limitAtIrqLock = limitAtIrqUnlock = nullptr;
	topAtIrqLock = topAtIrqUnlock = nullptr;
	takes = releases = 0;
	failures.clear();
}

static void Report(const char *name, std::string why) noexcept
{
	for (const std::string& f : failures)
	{
		why += (why.empty() ? "" : "; ") + f;
	}
	if (why.empty())
	{
		++passed;
		printf("PASS %s\n", name);
	}
	else
	{
		++failed;
		printf("FAIL %s: %s\n", name, why.c_str());
	}
}

static int Threads() noexcept
{
	Reset();
	heapLimit = arena + sizeof(arena);
	threaded = true;
	constexpr unsigned int Rounds = 20000;
	std::thread other([]() noexcept
		{
			for (unsigned int i = 0; i < Rounds; ++i)
			{
				GetMallocMutex();						// what malloc does around its _sbrk calls
				(void)_sbrk(8);
				ReleaseMallocMutex();
			}
		});
	for (unsigned int i = 0; i < Rounds; ++i)
	{
		(void)Tasks::AllocPermanent(8);
	}
	other.join();
	threaded = false;
	Report("malloc and AllocPermanent on two threads", (heapTop <= heapLimit) ? "" : "the heap top is above the heap limit");
	return (failed == 0) ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "threads") == 0)
	{
		return Threads();
	}

	{
		Reset();
		const char *const oldLimit = heapLimit;
		void *const p = Tasks::AllocPermanent(100);
		std::string why;
		if (irqLocks != 1) { why = "the interrupt lock was taken " + std::to_string(irqLocks) + " times"; }
		else if (limitAtIrqLock != oldLimit || limitAtIrqUnlock != p || heapLimit != p) { why = "the heap limit did not change inside the interrupt lock"; }
		if (takes != 0 || releases != 0) { why += (why.empty() ? "" : "; ") + std::string("the malloc mutex was taken"); }
		Report("AllocPermanent lowers the heap limit inside the interrupt lock and takes no malloc mutex", why);
	}

	{
		Reset();
		char *const oldTop = heapTop;
		void *const p = _sbrk(64);
		std::string why;
		if (irqLocks != 1) { why = "the interrupt lock was taken " + std::to_string(irqLocks) + " times"; }
		else if (p != oldTop || topAtIrqLock != oldTop || topAtIrqUnlock != oldTop + 64 || heapTop != oldTop + 64) { why = "the heap top did not change inside the interrupt lock"; }
		Report("_sbrk raises the heap top inside the interrupt lock", why);
	}

	{
		Reset();
		schedulerState = taskSCHEDULER_SUSPENDED;
		const char *const oldLimit = heapLimit;
		const void *const p1 = Tasks::AllocPermanent(16);
		schedulerState = taskSCHEDULER_NOT_STARTED;
		const void *const p2 = Tasks::AllocPermanent(16);
		std::string why;
		if (takes != 0 || releases != 0) { why = "the malloc mutex was taken"; }
		else if (p1 >= oldLimit || p2 >= p1) { why = "no memory was allocated"; }
		Report("AllocPermanent works while the scheduler is suspended or not started", why);
	}

	{
		Reset();
		primask = 1;
		(void)Tasks::AllocPermanent(16);
		const bool keptMasked = (primask == 1);
		primask = 0;
		(void)Tasks::AllocPermanent(16);
		const bool keptUnmasked = (primask == 0);
		Report("AllocPermanent leaves the interrupt mask as it found it", (keptMasked && keptUnmasked) ? "" : "the interrupt mask changed");
	}

	{
		Reset();
		schedulerState = taskSCHEDULER_SUSPENDED;
		GetMallocMutex();
		ReleaseMallocMutex();
		const bool tookSuspended = (takes == 1 && releases == 1 && failures.size() == 1);
		failures.clear();
		schedulerState = taskSCHEDULER_NOT_STARTED;
		GetMallocMutex();
		ReleaseMallocMutex();
		const bool skippedNotStarted = (takes == 1 && releases == 1);
		Report("GetMallocMutex takes the mutex while the scheduler is suspended, so FreeRTOS asserts, and not before it has started",
				(tookSuspended && skippedNotStarted) ? "" : "taken " + std::to_string(takes) + " times, released " + std::to_string(releases) + " times");
	}

	{
		Reset();
		primask = 1;
		GetMallocMutex();
		ReleaseMallocMutex();
		const bool assertedPrimask = failures.size() == 1;
		failures.clear();
		primask = 0;
		basepri = 0x20;
		GetMallocMutex();
		ReleaseMallocMutex();
		const bool assertedBasepri = failures.size() == 1;
		failures.clear();
		Report("GetMallocMutex asserts with PRIMASK or BASEPRI set and the scheduler running", (assertedPrimask && assertedBasepri) ? "" : "it did not assert");
	}

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
