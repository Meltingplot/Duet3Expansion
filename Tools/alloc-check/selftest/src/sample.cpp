// Functions with a known answer for alloc-check.py, see ../../README.md. "make selftest" compiles and links this file with
// runtime.cpp, with the firmware's compiler, -O3 and -g, for the Cortex-M0+ (SAMC21) and the Cortex-M4 (SAME51), and compares
// the findings with ../expected.txt and ../expected-unmask-<core>.txt. Each function says whether the check must find it.

#include "runtime.h"

// Reset_Handler calls each function once, and -O3 would inline them into it: then their findings would have its name
#define TESTED	__attribute__((noinline))

// An inline locker, as TaskCriticalSectionLocker in RRFLibraries RTOSIface.h: the calls of vTaskSuspendAll and xTaskResumeAll
// end up in the function that uses it
class Locker
{
public:
	Locker() noexcept { vTaskSuspendAll(); }
	~Locker() noexcept { (void)xTaskResumeAll(); }
};

// Out-of-line functions that open and close a region for their caller
__attribute__((noinline)) void EnterRegion() noexcept { vTaskSuspendAll(); }
__attribute__((noinline)) void LeaveRegion() noexcept { (void)xTaskResumeAll(); }

// A function that allocates; a call to it inside a region is found
__attribute__((noinline)) void *Helper(unsigned int n) noexcept { return malloc(n); }

int *volatile sink;
void *volatile rawSink;
volatile int selector;
void (*volatile callback)() noexcept;

// Found: operator new between vTaskSuspendAll and xTaskResumeAll
TESTED void SuspendedNew() noexcept
{
	vTaskSuspendAll();
	sink = new int(1);
	(void)xTaskResumeAll();
}

// Found: malloc inside the inline locker
TESTED void LockerMalloc() noexcept
{
	Locker lock;
	rawSink = malloc(16);
}

// Found: free between the out-of-line functions that open and close the region
TESTED void OutOfLineFree(void *p) noexcept
{
	EnterRegion();
	free(p);
	LeaveRegion();
}

// Found: a function that allocates, called inside a FreeRTOS critical section (taskENTER_CRITICAL)
TESTED void CallsHelperInCritical() noexcept
{
	vPortEnterCritical();
	rawSink = Helper(8);
	vPortExitCritical();
}

// Found: operator new with interrupts disabled, as AtomicCriticalSectionLocker (CoreN2G CoreIO.h) does it
TESTED void MaskedNew() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	sink = new int(2);
	SetPrimask(flags);
}

#if HAS_BASEPRI
// Found on the Cortex-M4: malloc with BASEPRI raised, as BasePriorityBooster (CoreN2G CoreIO.h) does it
TESTED void BoostedMalloc() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0x50);
	rawSink = malloc(4);
	SetBasepri(old);
}
#endif

// Found: a call through a function pointer inside a region, whose target the check cannot see
TESTED void SuspendedCallback() noexcept
{
	Locker lock;
	callback();
}

// Found, keyed with the file name: a static function
static __attribute__((noinline)) void StaticSuspended() noexcept
{
	Locker lock;
	rawSink = Helper(32);
}

// Found: one case of a switch (a jump table) inside a region allocates
TESTED void SwitchInRegion() noexcept
{
	Locker lock;
	switch (selector)
	{
	case 0: sink = nullptr; break;
	case 1: rawSink = Helper(1); break;
	case 2: selector = 5; break;
	case 3: selector = 7; break;
	case 4: selector = 9; break;
	case 5: selector = 11; break;
	default: break;
	}
}

// Found: the allocation is reached only through a jump over more than 2 KB, which Thumb-1 code (Cortex-M0+) makes with bl
#define NOPS_4		"nop\n" "nop\n" "nop\n" "nop\n"
#define NOPS_64		NOPS_4 NOPS_4 NOPS_4 NOPS_4 NOPS_4 NOPS_4 NOPS_4 NOPS_4 NOPS_4 NOPS_4 NOPS_4 NOPS_4 NOPS_4 NOPS_4 NOPS_4 NOPS_4
#define NOPS_1024	NOPS_64 NOPS_64 NOPS_64 NOPS_64 NOPS_64 NOPS_64 NOPS_64 NOPS_64 NOPS_64 NOPS_64 NOPS_64 NOPS_64 NOPS_64 NOPS_64 NOPS_64 NOPS_64

TESTED void FarJumpInRegion() noexcept
{
	Locker lock;
	if (selector == 1)
	{
		__asm volatile (NOPS_1024 NOPS_1024 ::: "memory");
	}
	else
	{
		rawSink = malloc(3);
	}
}

// Found: an exception handler in the vector table that allocates
extern "C" void TestIrq_Handler() noexcept
{
	rawSink = Helper(4);
}

// Not found: the allocation is before the region and the release after it, the pattern of the fixes
TESTED void AllocateBeforeRegion() noexcept
{
	int *const p = new int(3);
	{
		Locker lock;
		sink = p;
	}
	delete p;
}

// Not found: the region is closed on every path before the allocation
TESTED void ConditionalRegion(bool b) noexcept
{
	if (b)
	{
		Locker lock;
		selector = 1;
	}
	rawSink = malloc(2);
}

// Not found: interrupts are enabled again before the allocation
TESTED void AllocateAfterRestore() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	selector = 3;
	SetPrimask(flags);
	sink = new int(4);
}

// Not found: a switch outside a region
TESTED void SwitchOutsideRegion() noexcept
{
	switch (selector)
	{
	case 0: rawSink = Helper(2); break;
	case 1: selector = 4; break;
	case 2: selector = 6; break;
	case 3: selector = 8; break;
	case 4: selector = 10; break;
	default: break;
	}
	Locker lock;
	selector = 12;
}

// Not found: an exception handler that does not allocate
extern "C" void QuietIrq_Handler() noexcept
{
	selector = 0;
}

// Keep the static function in the program
TESTED void CallStatic() noexcept
{
	StaticSuspended();
}

// The cases of the unmask check (--unmask). runtime.cpp models the Cortex-M4F port of FreeRTOS on the Cortex-M4, whose critical
// sections use BASEPRI, and the Cortex-M0 port on the Cortex-M0+, whose critical sections disable the interrupts

#if HAS_BASEPRI
// Found on the Cortex-M4: a FreeRTOS call with BASEPRI raised, whose critical section ends with BASEPRI 0
TESTED void BoostedNotify() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0x50);
	(void)xTaskGenericNotify();
	SetBasepri(old);
}

// Found on the Cortex-M4: a task critical section with BASEPRI raised; xTaskResumeAll ends with a critical section
TESTED void BoostedLocker() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0x50);
	{
		Locker lock;
		selector = 13;
	}
	SetBasepri(old);
}

// Found on the Cortex-M4: a critical section of the function itself with BASEPRI raised, whose end writes BASEPRI 0
TESTED void BoostedCritical() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0x50);
	vPortEnterCritical();
	selector = 16;
	vPortExitCritical();
	SetBasepri(old);
}

// Found on the Cortex-M4: a call through a function pointer with BASEPRI raised (also a finding of the heap check)
TESTED void BoostedCallback() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0x50);
	callback();
	SetBasepri(old);
}

// Not found: a FromISR function with BASEPRI raised; it restores BASEPRI to the value that it found
TESTED void BoostedGiveFromIsr() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0x50);
	vTaskGenericNotifyGiveFromISR();
	SetBasepri(old);
}
#endif

// Found on the Cortex-M0+: a task notification with PRIMASK set, whose critical section ends with cpsie i. Not found on the
// Cortex-M4, whose critical sections leave PRIMASK alone
TESTED void MaskedNotify() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	(void)xTaskGenericNotify();
	SetPrimask(flags);
}

// Found on the Cortex-M0+: a critical section of the function itself with PRIMASK set, whose end runs cpsie i
TESTED void MaskedCritical() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	vPortEnterCritical();
	selector = 18;
	vPortExitCritical();
	SetPrimask(flags);
}

// Found: a call through a function pointer with PRIMASK set (also a finding of the heap check)
TESTED void MaskedCallback() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	callback();
	SetPrimask(flags);
}

// Found: a nested region whose inner end clears PRIMASK for the outer one too
TESTED void NestedEnable() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	DisableIrq();
	selector = 14;
	EnableIrq();
	selector = 15;
	SetPrimask(flags);
}

// Not found: a FromISR function with PRIMASK set; it restores the mask that it found
TESTED void MaskedGiveFromIsr() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	vTaskGenericNotifyGiveFromISR();
	SetPrimask(flags);
}

// Not found: the notification after the region
TESTED void NotifyAfterMask() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	const bool wake = (selector != 0);
	SetPrimask(flags);
	if (wake)
	{
		(void)xTaskGenericNotify();
	}
}

// Not found: a FreeRTOS call inside a critical section of the function: the nested end keeps the mask
TESTED void NotifyInCritical() noexcept
{
	vPortEnterCritical();
	(void)xTaskGenericNotify();
	vPortExitCritical();
}

// Not found: a callee that saves and restores PRIMASK, as IrqSave and IrqRestore do
TESTED void SaveRestoreHelper() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	selector = 17;
	SetPrimask(flags);
}

TESTED void MaskedSaveRestore() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	SaveRestoreHelper();
	SetPrimask(flags);
}

extern "C" void Reset_Handler() noexcept
{
	SuspendedNew(); LockerMalloc(); OutOfLineFree(nullptr); CallsHelperInCritical(); MaskedNew();
#if HAS_BASEPRI
	BoostedMalloc();
#endif
	SuspendedCallback(); CallStatic(); SwitchInRegion(); FarJumpInRegion(); AllocateBeforeRegion(); ConditionalRegion(true);
	AllocateAfterRestore(); SwitchOutsideRegion();
#if HAS_BASEPRI
	BoostedNotify(); BoostedLocker(); BoostedCritical(); BoostedCallback(); BoostedGiveFromIsr();
#endif
	MaskedNotify(); MaskedCritical(); MaskedCallback(); NestedEnable(); MaskedGiveFromIsr(); NotifyAfterMask(); NotifyInCritical();
	MaskedSaveRestore();
	for (;;) { }
}

// The vector table: the initial stack pointer, the reset handler, then the exception handlers
extern "C" __attribute__((used, section(".vectors"))) void (*const exception_table[])() noexcept =
{
	nullptr, Reset_Handler, TestIrq_Handler, QuietIrq_Handler
};
