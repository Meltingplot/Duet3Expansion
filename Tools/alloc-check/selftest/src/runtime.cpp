// Stand-ins for the functions that alloc-check.py knows by name, so that the selftest programs link without a C library. See
// runtime.h.

#include "runtime.h"
#include <new>

static volatile uint32_t suspended, critical, notified;
static char heap[256];
static size_t heapUsed;

extern "C"
{
	__attribute__((noinline)) void vTaskSuspendAll() noexcept { suspended = suspended + 1; }

#if HAS_BASEPRI
	// As the Cortex-M4F port of FreeRTOS: vPortEnterCritical raises BASEPRI without touching PRIMASK (portmacro.h:213-225), and
	// vPortExitCritical writes BASEPRI 0 when the nesting count drops to 0 (port.c:492-500)
	__attribute__((noinline)) void vPortEnterCritical() noexcept
	{
		SetBasepri(0x50);
		critical = critical + 1;
	}

	__attribute__((noinline)) void vPortExitCritical() noexcept
	{
		critical = critical - 1;
		if (critical == 0)
		{
			SetBasepri(0);
		}
	}

	// A FromISR function raises BASEPRI and restores the value that it found (ulPortRaiseBASEPRI and vPortSetBASEPRI,
	// portmacro.h:229-255)
	__attribute__((noinline)) void vTaskGenericNotifyGiveFromISR() noexcept
	{
		const uint32_t saved = GetBasepri();
		SetBasepri(0x50);
		notified = notified + 1;
		SetBasepri(saved);
	}
#else
	// As the Cortex-M0 port of FreeRTOS: vPortEnterCritical disables the interrupts (port.c:790-799), and vPortExitCritical
	// enables them when the nesting count drops to 0 (port.c:803-812)
	__attribute__((noinline)) void vPortEnterCritical() noexcept
	{
		DisableIrq();
		critical = critical + 1;
	}

	__attribute__((noinline)) void vPortExitCritical() noexcept
	{
		critical = critical - 1;
		if (critical == 0)
		{
			EnableIrq();
		}
	}

	// A FromISR function saves PRIMASK, disables the interrupts and restores the value that it found (ulSetInterruptMask and
	// vClearInterruptMask, portasm.c:243-268)
	__attribute__((noinline)) void vTaskGenericNotifyGiveFromISR() noexcept
	{
		const uint32_t saved = GetPrimask();
		DisableIrq();
		notified = notified + 1;
		SetPrimask(saved);
	}
#endif

	// xTaskResumeAll works inside a critical section (tasks.c:4038-4162), as task notifications do
	__attribute__((noinline)) long xTaskResumeAll() noexcept
	{
		vPortEnterCritical();
		suspended = suspended - 1;
		vPortExitCritical();
		return 0;
	}

	__attribute__((noinline)) long xTaskGenericNotify() noexcept
	{
		vPortEnterCritical();
		notified = notified + 1;
		vPortExitCritical();
		return 1;
	}

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
