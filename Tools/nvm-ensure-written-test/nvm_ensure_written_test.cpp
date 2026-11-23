// Host test of NonVolatileMemory::EnsureWritten (src/Hardware/NonVolatileMemory.cpp) on the SAMC21 (TOOL1LC, SZP): when it returns, the
// write of the last page must have finished, because the exception handlers reset the processor right after it
// (src/Hardware/ExceptionHandlers.cpp). The function is the real text, extracted by the Makefile; the class around it, Flash::RwwErase and
// Flash::RwwWrite and the NVMCTRL are stand-ins. See README.md.

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>

#define SAMC21	1

// ---------------------------------------------------------------------------------------------------------------------------------
// Model of the NVMCTRL: a command keeps it busy for some reads of INTFLAG, then INTFLAG.READY is set again (bit 0, CoreN2G
// src/atmel/SAMC21_DFP/1.2.176/samc21/include_mcc/component/nvmctrl.h). The HPL waits for READY before each command and returns as soon as
// it has issued the command (CoreN2G src/SAME5x_C21/SAMC21/hpl/nvmctrl/hpl_nvmctrl.c, _flash_program and hpl_RwwWrite).

constexpr uint32_t NVMCTRL_INTFLAG_READY = 1u << 0;
constexpr unsigned int PollsPerCommand = 5;

static unsigned int busyPolls = 0;			// reads of INTFLAG until the command in progress has finished
static unsigned int commands = 0;

struct IntflagRegister
{
	operator uint32_t() const noexcept
	{
		if (busyPolls != 0)
		{
			--busyPolls;
			return 0;
		}
		return NVMCTRL_INTFLAG_READY;
	}
};

struct NvmctrlModel
{
	struct { IntflagRegister reg; } INTFLAG;
};

static NvmctrlModel nvmctrl;
#define NVMCTRL		(&nvmctrl)

static void WaitReadyThenStart() noexcept
{
	while ((NVMCTRL->INTFLAG.reg & NVMCTRL_INTFLAG_READY) == 0) { }
	busyPolls = PollsPerCommand;
	++commands;
}

namespace Flash
{
	// 512 bytes are 2 rows of 4 pages of 64 bytes (CoreN2G src/atmel/SAMC21_DFP/1.2.176/samc21/include/instance/nvmctrl.h:73-78):
	// 2 row erases, or 8 page writes (hpl_nvmctrl.c, hpl_RwwErase and hpl_RwwWrite)
	constexpr uint32_t RowSize = 256, PageSize = 64;

	bool RwwErase(uint32_t addr, uint32_t length) noexcept
	{
		for (uint32_t done = 0; done < length; done += RowSize)
		{
			WaitReadyThenStart();
		}
		return true;
	}

	bool RwwWrite(uint32_t addr, uint32_t length, const uint8_t *data) noexcept
	{
		for (uint32_t done = 0; done < length; done += PageSize)
		{
			WaitReadyThenStart();
		}
		return true;
	}
}

// src/Hardware/NonVolatileMemory.cpp:15, with FLASH_ADDR 0 on the SAMC21
constexpr uint32_t RWW_ADDR = 0x00400000;

// Stand-in for the class of src/Hardware/NonVolatileMemory.h with the members that EnsureWritten uses
enum class NvmPage : uint8_t { common = 0, closedLoop };

class NonVolatileMemory
{
public:
	void EnsureWritten() noexcept;

	enum class NvmState : uint8_t { notRead, clean, writeNeeded, eraseAndWriteNeeded };
	union NVM { uint8_t bytes[512]; uint32_t words[128]; };
	alignas(4) NVM buffer;
	NvmState state = NvmState::clean;
	NvmPage page = NvmPage::common;
};

// ---------------------------------------------------------------------------------------------------------------------------------
// The real code

#include "NonVolatileMemory.inc"

// ---------------------------------------------------------------------------------------------------------------------------------

static unsigned int runs = 0, failures = 0;

static void RunCase(NonVolatileMemory::NvmState initial, const char *name, unsigned int expectedCommands) noexcept
{
	NonVolatileMemory mem;
	mem.state = initial;
	busyPolls = 0;
	commands = 0;
	mem.EnsureWritten();
	const bool ok = mem.state == NonVolatileMemory::NvmState::clean && commands == expectedCommands && busyPolls == 0;
	++runs;
	std::printf("%s %s: %u commands, %s when EnsureWritten returned\n", (ok) ? "PASS" : "FAIL", name, commands,
				(busyPolls == 0) ? "NVMCTRL ready" : "a page write still in progress");
	if (!ok)
	{
		++failures;
	}
}

int main()
{
	RunCase(NonVolatileMemory::NvmState::clean, "nothing to write", 0);
	RunCase(NonVolatileMemory::NvmState::writeNeeded, "write needed (a free reset data slot)", 8);
	RunCase(NonVolatileMemory::NvmState::eraseAndWriteNeeded, "erase and write needed (all reset data slots used)", 10);
	std::printf("%u runs, %u failed\n", runs, failures);
	return (failures == 0) ? 0 : 1;
}
