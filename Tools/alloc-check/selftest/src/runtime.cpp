// Stand-ins for the functions that alloc-check.py knows by name, so that the selftest programs link without a C library. See
// runtime.h.

#include "runtime.h"
#include <new>

static volatile uint32_t suspended, critical;
static char heap[256];
static size_t heapUsed;

extern "C"
{
	__attribute__((noinline)) void vTaskSuspendAll() noexcept { suspended = suspended + 1; }
	__attribute__((noinline)) long xTaskResumeAll() noexcept { suspended = suspended - 1; return 0; }
	__attribute__((noinline)) void vPortEnterCritical() noexcept { critical = critical + 1; }
	__attribute__((noinline)) void vPortExitCritical() noexcept { critical = critical - 1; }

	__attribute__((noinline)) void *malloc(size_t n) noexcept
	{
		void *const p = (heapUsed + n <= sizeof(heap)) ? &heap[heapUsed] : nullptr;
		heapUsed += n;
		return p;
	}

	__attribute__((noinline)) void free(void *p) noexcept { if (p == nullptr) { heapUsed = 0; } }
}

void *operator new(size_t n) { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
