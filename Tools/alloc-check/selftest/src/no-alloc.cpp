// A program that allocates only outside its regions. "make selftest" runs "alloc-check.py --update" on it, which must write
// an allowlist with the header and no entries.

#include "runtime.h"

int *volatile sink;

void AllocateOutside() noexcept
{
	int *const p = new int(1);
	vTaskSuspendAll();
	sink = p;
	(void)xTaskResumeAll();
	delete p;
}

extern "C" void Reset_Handler() noexcept
{
	AllocateOutside();
	for (;;) { }
}

extern "C" __attribute__((used, section(".vectors"))) void (*const exception_table[])() noexcept = { nullptr, Reset_Handler };
