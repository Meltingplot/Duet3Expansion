/*
 * AtomicWrappers.cpp
 *
 * Fast RAM-resident replacement for the out-of-line atomic helper used by the step ISR, on boards that
 * select it via the linker -wrap option (currently only TOOL1LC).
 */

#include <RepRapFirmware.h>

#ifdef TOOL1LC

// armv6-m processors have no atomic read-modify-write instructions, so std::atomic<int32_t>::operator+=
// compiles to a call to __atomic_fetch_add_4. The default implementation is correct but lives in flash and is
// reached from the RAM-resident step ISR through a veneer, ~35-40 cycles in total for one addition; internally
// it just disables interrupts around a load-add-store. This wrap - activated by -Wl,-wrap,__atomic_fetch_add_4
// in the TOOL1LC linker flags, the same mechanism as the Qfplib float wraps - performs the identical
// interrupts-disabled load-add-store, but it is placed in RAM (zero wait states) and reached by a direct call,
// ~16 cycles. The only caller in the TOOL1LC image is DriveMovement's extrusion accumulator update at segment
// completion in the step ISR.
// The memory-order argument is ignored because on this single-core MCU disabling interrupts is a full barrier.
// Unsigned arithmetic gives the same two's complement result as the replaced routine for all operands, and like
// that routine we return the previous value.
extern "C" __attribute__((section(".time_critical"))) uint32_t __wrap___atomic_fetch_add_4(volatile void *ptr, uint32_t val, int /*memorder*/) noexcept
{
	volatile uint32_t *const p = static_cast<volatile uint32_t *>(ptr);
	const auto iflags = IrqSave();
	const uint32_t oldValue = *p;
	*p = oldValue + val;
	IrqRestore(iflags);
	return oldValue;
}

#endif
