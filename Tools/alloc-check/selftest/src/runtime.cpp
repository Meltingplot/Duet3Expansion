// Stand-ins for the functions that alloc-check.py knows by name, so that the selftest programs link without a C library. See
// runtime.h. Without RESTORING_PORT they behave as the ports of upstream FreeRTOS V11.3.0, with RESTORING_PORT as those of
// Meltingplot/FreeRTOS from PR #1 on, whose critical sections restore the mask that they found.

#include "runtime.h"
#include <new>

static volatile uint32_t suspended, critical, notified, available, waiting;
#ifdef RESTORING_PORT
static volatile uint32_t maskBeforeCritical;
#endif
static char heap[256];
static size_t heapUsed;

extern "C"
{
	__attribute__((noinline)) void vTaskSuspendAll() noexcept { suspended = suspended + 1; }

#if HAS_BASEPRI && defined(RESTORING_PORT)
	// As the Cortex-M4F port of Meltingplot/FreeRTOS: vPortEnterCritical raises BASEPRI with basepri_max (ulPortRaiseBASEPRI,
	// portmacro.h), the outermost one saves the BASEPRI that it found, and vPortExitCritical writes it back when the nesting count
	// drops to 0 (port.c, vPortEnterCritical and vPortExitCritical)
	__attribute__((noinline)) void vPortEnterCritical() noexcept
	{
		const uint32_t saved = GetBasepri();
		SetBasepriMax(0x50);
		if (critical == 0)
		{
			maskBeforeCritical = saved;
		}
		critical = critical + 1;
	}

	__attribute__((noinline)) void vPortExitCritical() noexcept
	{
		critical = critical - 1;
		if (critical == 0)
		{
			SetBasepri(maskBeforeCritical);
		}
	}

	// pdFALSE while the task holds a critical section other than the kernelNesting ones of the kernel function that asks, or a
	// mask of its own (port.c, xPortTaskCanBlock)
	__attribute__((noinline)) long xPortTaskCanBlock(unsigned long kernelNesting) noexcept
	{
		if (critical != kernelNesting)
		{
			return 0;
		}
		const uint32_t basepri = (kernelNesting != 0) ? maskBeforeCritical : GetBasepri();
		return (basepri == 0 && GetPrimask() == 0) ? 1 : 0;
	}

	__attribute__((noinline)) void vTaskGenericNotifyGiveFromISR() noexcept
	{
		const uint32_t saved = GetBasepri();
		SetBasepriMax(0x50);
		notified = notified + 1;
		SetBasepri(saved);
	}
#elif HAS_BASEPRI
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
# ifdef RESTORING_PORT
	// As the Cortex-M0 port of Meltingplot/FreeRTOS: the outermost vPortEnterCritical saves the PRIMASK that it found before it
	// disables the interrupts, and vPortExitCritical writes it back when the nesting count drops to 0 (port.c,
	// vPortEnterCritical and vPortExitCritical)
	__attribute__((noinline)) void vPortEnterCritical() noexcept
	{
		const uint32_t saved = GetPrimask();
		DisableIrq();
		if (critical == 0)
		{
			maskBeforeCritical = saved;
		}
		critical = critical + 1;
	}

	__attribute__((noinline)) void vPortExitCritical() noexcept
	{
		critical = critical - 1;
		if (critical == 0)
		{
			SetPrimask(maskBeforeCritical);
		}
	}

	// pdFALSE while the task holds a critical section other than the kernelNesting ones of the kernel function that asks, or a
	// mask of its own (port.c, xPortTaskCanBlock)
	__attribute__((noinline)) long xPortTaskCanBlock(unsigned long kernelNesting) noexcept
	{
		if (critical != kernelNesting)
		{
			return 0;
		}
		return (((kernelNesting != 0) ? maskBeforeCritical : GetPrimask()) == 0) ? 1 : 0;
	}
# else
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
# endif

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

	// Where a task blocks (tasks.c:5310). With RESTORING_PORT its trace hook asserts xPortTaskCanBlock(0) (FreeRTOSConfig.h of
	// Meltingplot/FreeRTOS, traceENTER_vTaskPlaceOnEventList)
	__attribute__((noinline)) void vTaskPlaceOnEventList() noexcept
	{
#ifdef RESTORING_PORT
		if (xPortTaskCanBlock(0) == 0)
		{
			for (;;) { }
		}
#endif
		waiting = waiting + 1;
	}

	// A semaphore take: a critical section that takes the semaphore if it is available or returns if the timeout is 0, then
	// a block with the scheduler suspended (queue.c:1659-1830, the block at :1806)
	__attribute__((noinline)) long xQueueSemaphoreTake(unsigned long ticksToWait) noexcept
	{
		vPortEnterCritical();
		const bool taken = (available != 0);
		if (taken)
		{
			available = available - 1;
		}
		vPortExitCritical();
		if (!taken && ticksToWait != 0)
		{
			vTaskSuspendAll();
			vTaskPlaceOnEventList();
			(void)xTaskResumeAll();
		}
		return taken;
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
