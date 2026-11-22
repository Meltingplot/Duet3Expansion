// Host test of the lock that Tasks::AllocPermanent (src/Platform/Tasks.cpp) takes, compiled from the real text of the malloc mutex
// functions and Tasks::AllocPermanent, and of _sbrk and CoreAllocPermanent from CoreN2G's src/syscalls.h, that the Makefile extracts.
//
// Our malloc (src/libc/nano-mallocr.c) takes the malloc mutex through GetMallocMutex. _sbrk raises heapTop up to heapLimit, and
// CoreAllocPermanent lowers heapLimit down to heapTop, so the two must not run at the same time; syscalls.h says to take the malloc
// mutex before calling CoreAllocPermanent. Up to 3.7-dev 9ac5016c, Tasks::AllocPermanent called newlib's __malloc_lock instead, which
// is the lock of newlib's own malloc and the RP2040 builds. Our builds do not replace it, and newlib's calls
// __retarget_lock_acquire_recursive, which is empty (bx lr in the TOOL1LC, EXP1HCL and SZP ELF files). So AllocPermanent took no lock,
// and a task that allocated permanent memory (MoveSegment, CanMessageBuffer) while another one was inside malloc could hand out memory
// that the other task's _sbrk hands out too.
//
// The test plays the malloc mutex, the scheduler state and the interrupt mask. On one thread it checks that AllocPermanent changes the
// heap limit while it holds the malloc mutex, that it waits for a task that is inside malloc, that it takes nothing while the scheduler
// is suspended or not started (like malloc), and that it asserts with interrupts masked (like malloc, since e847c690). With "threads"
// it runs malloc's part (the mutex and _sbrk) and AllocPermanent on two threads, for ThreadSanitizer to check that they do not race.

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

static uint32_t primask = 0, basepri = 0;
static uint32_t __get_PRIMASK() noexcept { return primask; }
static uint32_t __get_BASEPRI() noexcept { return basepri; }

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

// The mutex. On one thread it counts, records the heap limit when it is taken and released, and lets the task that is inside malloc
// finish first, as the real mutex does when that task holds it. With "threads" it is a real recursive mutex.
static bool threaded = false;
static std::recursive_mutex realMutex;
static unsigned int takes = 0, releases = 0;
static const char *limitAtTake = nullptr, *limitAtRelease = nullptr;
static bool otherTaskInsideMalloc = false;			// another task holds the malloc mutex inside malloc and is about to call _sbrk
static size_t otherTaskRequest = 0;
static char *otherTaskChunk = nullptr;

// The other task's malloc goes on: it calls _sbrk and then releases the mutex
static void FinishOtherTask() noexcept
{
	otherTaskInsideMalloc = false;
	otherTaskChunk = static_cast<char *>(_sbrk((ptrdiff_t)otherTaskRequest));
}

void Mutex::Take() noexcept
{
	if (threaded)
	{
		realMutex.lock();
		return;
	}
	if (otherTaskInsideMalloc)
	{
		FinishOtherTask();								// the caller blocks until the other task releases the mutex
	}
	++takes;
	limitAtTake = heapLimit;
}

void Mutex::Release() noexcept
{
	if (threaded)
	{
		realMutex.unlock();
		return;
	}
	++releases;
	limitAtRelease = heapLimit;
}

alignas(8) static char arena[1 << 20];
static unsigned int passed = 0, failed = 0;

static void Reset() noexcept
{
	heapTop = arena + 1024;
	heapLimit = arena + 3072;
	schedulerState = taskSCHEDULER_RUNNING;
	primask = basepri = 0;
	takes = releases = 0;
	limitAtTake = limitAtRelease = nullptr;
	otherTaskInsideMalloc = false;
	otherTaskChunk = nullptr;
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

static bool Overlap(const char *a, size_t na, const char *b, size_t nb) noexcept
{
	return a < b + nb && b < a + na;
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
		if (takes != 1 || releases != 1) { why = "the malloc mutex was taken " + std::to_string(takes) + " times and released " + std::to_string(releases) + " times"; }
		else if (limitAtTake != oldLimit || limitAtRelease != p || heapLimit != p) { why = "the heap limit did not change while the mutex was held"; }
		Report("AllocPermanent lowers the heap limit while it holds the malloc mutex", why);
	}

	{
		Reset();
		otherTaskInsideMalloc = true;
		otherTaskRequest = 512;
		char *const p = static_cast<char *>(Tasks::AllocPermanent(256));
		std::string why;
		if (otherTaskInsideMalloc)
		{
			why = "AllocPermanent did not wait for the task inside malloc";
			FinishOtherTask();
		}
		if (otherTaskChunk == nullptr || Overlap(otherTaskChunk, otherTaskRequest, p, 256)) { why += " the two allocations overlap"; }
		Report("AllocPermanent waits for a task that is inside malloc", why);
	}

	{
		Reset();
		schedulerState = taskSCHEDULER_SUSPENDED;
		(void)Tasks::AllocPermanent(16);
		schedulerState = taskSCHEDULER_NOT_STARTED;
		(void)Tasks::AllocPermanent(16);
		Report("AllocPermanent takes nothing while the scheduler is suspended or not started, as malloc", (takes == 0 && releases == 0) ? "" : "the mutex was taken");
	}

	{
		Reset();
		primask = 1;
		(void)Tasks::AllocPermanent(16);
		const bool assertedPrimask = failures.size() == 1;
		failures.clear();
		primask = 0;
		basepri = 0x20;
		(void)Tasks::AllocPermanent(16);
		const bool assertedBasepri = failures.size() == 1;
		failures.clear();
		Report("AllocPermanent asserts with PRIMASK or BASEPRI set and the scheduler running, as malloc", (assertedPrimask && assertedBasepri) ? "" : "it did not assert");
	}

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
