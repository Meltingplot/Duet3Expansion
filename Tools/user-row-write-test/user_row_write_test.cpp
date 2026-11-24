// Host test of _user_row_write_exec (CoreN2G src/SAME5x_C21/SAME5x/hpl/nvmctrl/hpl_nvmctrl.c and
// src/SAME5x_C21/SAMC21/hpl/nvmctrl/hpl_nvmctrl.c), which _user_area_write calls to rewrite the NVM user row: the page buffer must be
// filled with interrupts off, as _flash_program does since CoreN2G 4b7ab3f (SAME5x) and cdd9ba1 (SAMC21). The function is the real
// text, extracted by the Makefile; the NVMCTRL, the DSU and PRIMASK are modelled here. Compiled once with MODEL_SAME5X and once with
// MODEL_SAMC21. See README.md.

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(MODEL_SAME5X)
# define MODEL_NAME		"SAME5x"
constexpr size_t UserRowSize = 512;			// "32 Quad words for User row" (hpl_nvmctrl.c), NVMCTRL_PAGE_SIZE 512
#elif defined(MODEL_SAMC21)
# define MODEL_NAME		"SAMC21"
constexpr size_t UserRowSize = 256;			// NVMCTRL_ROW_SIZE (SAMC21_DFP 1.2.176 include/instance/nvmctrl.h:78)
#else
# error "Compile with MODEL_SAME5X or MODEL_SAMC21"
#endif

// ---------------------------------------------------------------------------------------------------------------------------------
// Constants of the code under test. SAME51_DFP 1.1.139 include/component/nvmctrl.h, SAMC21_DFP 1.2.176 include/component/nvmctrl.h,
// hal/utils/include/err_codes.h, and hpl_nvmctrl.c (_NVM_USER_ROW_BASE)

#define _U_(x)						((uint32_t)(x))
#define ERR_NONE					0
#define ERR_DENIED					-17
#define _NVM_USER_ROW_BASE			0x804000

#if defined(MODEL_SAME5X)
# define NVMCTRL_CTRLA_WMODE_Pos	4
# define NVMCTRL_CTRLA_WMODE_Msk	(_U_(0x3) << NVMCTRL_CTRLA_WMODE_Pos)
# define NVMCTRL_CTRLB_CMD_EP		_U_(0x0)
# define NVMCTRL_CTRLB_CMD_WQW		_U_(0x4)
# define NVMCTRL_CTRLB_CMD_PBC		_U_(0x15)
# define NVMCTRL_CTRLB_CMDEX_KEY	(_U_(0xA5) << 8)
#else
# define NVMCTRL_CTRLA_CMD_EAR		_U_(0x5)
# define NVMCTRL_CTRLA_CMD_WAP		_U_(0x6)
# define NVMCTRL_CTRLA_CMDEX_KEY	(_U_(0xA5) << 8)
# define NVMCTRL_INTFLAG_READY		(_U_(0x1) << 0)
# define NVMCTRL_STATUS_SB			(_U_(0x1) << 8)
# define NVMCTRL_STATUS_MASK		_U_(0x011F)
# define NVMCTRL_ROW_SIZE			256
# define NVMCTRL_PAGE_SIZE			64
#endif

typedef uint32_t hri_nvmctrl_addr_reg_t;

// ---------------------------------------------------------------------------------------------------------------------------------
// The model. Writes to NVMCTRL_USER go to the page buffer. An event is any call that the code under test makes into the model:
// a register access or a change of PRIMASK. Between two events the code runs plain instructions only, so if the page buffer changed
// between two events while PRIMASK was 0, an interrupt could have landed in the fill.

static uint8_t pageBuffer[UserRowSize];
static uint8_t pageBufferAtLastEvent[UserRowSize];
static uint8_t userRow[UserRowSize];
#define NVMCTRL_USER	((uintptr_t)&pageBuffer[0])

static uint32_t primask = 0;
static unsigned int fillsWithInterruptsOn = 0, fillCommandsWithInterruptsOn = 0, commandsWhileBusy = 0, writeCommands = 0;
static unsigned int busyPolls = 0;
static uint32_t addrReg = 0, ctrlaReg = 0, ctrlbReg = 0, statusReg = 0;
static bool securityBit = false;

static void Event() noexcept
{
	if (memcmp(pageBuffer, pageBufferAtLastEvent, UserRowSize) != 0)
	{
		if (primask == 0)
		{
			++fillsWithInterruptsOn;
		}
		memcpy(pageBufferAtLastEvent, pageBuffer, UserRowSize);
	}
}

static uint32_t __get_PRIMASK() noexcept { Event(); return primask; }
static void __disable_irq() noexcept { Event(); primask = 1; }
static void __set_PRIMASK(uint32_t v) noexcept { Event(); primask = v; }

static bool Ready() noexcept
{
	if (busyPolls != 0)
	{
		--busyPolls;
		return false;
	}
	return true;
}

static void Command(uint32_t reg) noexcept
{
	if (busyPolls != 0)
	{
		++commandsWhileBusy;
	}
	const uint32_t cmd = reg & 0x7F;
#if defined(MODEL_SAME5X)
	const bool isFillCommand = (cmd == NVMCTRL_CTRLB_CMD_PBC || cmd == NVMCTRL_CTRLB_CMD_WQW);
	if (cmd == NVMCTRL_CTRLB_CMD_EP)
	{
		memset(userRow, 0xFF, UserRowSize);
	}
	else if (cmd == NVMCTRL_CTRLB_CMD_PBC)
	{
		memset(pageBuffer, 0xFF, UserRowSize);
		memcpy(pageBufferAtLastEvent, pageBuffer, UserRowSize);	// the controller wrote it, not the code under test
	}
	else if (cmd == NVMCTRL_CTRLB_CMD_WQW)
	{
		const uint32_t offset = addrReg - _NVM_USER_ROW_BASE;		// the ADDR register holds a byte address on the SAME5x
		memcpy(userRow + offset, pageBuffer + offset, 16);
		++writeCommands;
	}
#else
	const bool isFillCommand = (cmd == NVMCTRL_CTRLA_CMD_WAP);
	if (cmd == NVMCTRL_CTRLA_CMD_EAR)
	{
		memset(userRow, 0xFF, UserRowSize);
	}
	else if (cmd == NVMCTRL_CTRLA_CMD_WAP)
	{
		const uint32_t offset = addrReg * 2 - _NVM_USER_ROW_BASE;	// the ADDR register holds a halfword address on the SAMC21
		const uint32_t pageStart = offset & ~(uint32_t)(NVMCTRL_PAGE_SIZE - 1);
		memcpy(userRow + pageStart, pageBuffer + pageStart, NVMCTRL_PAGE_SIZE);
		++writeCommands;
	}
#endif
	if (isFillCommand && primask == 0)
	{
		++fillCommandsWithInterruptsOn;
	}
	busyPolls = 3;
}

struct Nvmctrl { };
static Nvmctrl nvmctrlModel;
#define NVMCTRL		(&nvmctrlModel)

static void hri_nvmctrl_write_ADDR_reg(Nvmctrl *, uint32_t v) noexcept { Event(); addrReg = v; }

#if defined(MODEL_SAME5X)
struct Dsu { struct { struct { uint8_t PROT; } bit; } STATUSB; };
static Dsu dsuModel;
# define DSU		(&dsuModel)

static uint32_t hri_nvmctrl_read_CTRLA_reg(Nvmctrl *) noexcept { Event(); return ctrlaReg; }
static void hri_nvmctrl_write_CTRLA_reg(Nvmctrl *, uint32_t v) noexcept { Event(); ctrlaReg = v; }
static void hri_nvmctrl_clear_CTRLA_WMODE_bf(Nvmctrl *, uint32_t mask) noexcept { Event(); ctrlaReg &= ~mask; }
static bool hri_nvmctrl_get_STATUS_READY_bit(Nvmctrl *) noexcept { Event(); return Ready(); }
static void hri_nvmctrl_write_CTRLB_reg(Nvmctrl *, uint32_t v) noexcept { Event(); Command(v); }
#else
static uint32_t hri_nvmctrl_read_CTRLB_reg(Nvmctrl *) noexcept { Event(); return ctrlbReg; }
static void hri_nvmctrl_write_CTRLB_reg(Nvmctrl *, uint32_t v) noexcept { Event(); ctrlbReg = v; }
static void hri_nvmctrl_set_CTRLB_MANW_bit(Nvmctrl *) noexcept { Event(); ctrlbReg |= 1u << 7; }
static bool hri_nvmctrl_get_STATUS_reg(Nvmctrl *, uint32_t mask) noexcept { Event(); return ((securityBit ? NVMCTRL_STATUS_SB : 0) & mask) != 0; }
static void hri_nvmctrl_clear_STATUS_reg(Nvmctrl *, uint32_t mask) noexcept { Event(); statusReg &= ~mask; }
static uint32_t hri_nvmctrl_get_INTFLAG_reg(Nvmctrl *, uint32_t mask) noexcept { Event(); return (Ready()) ? (NVMCTRL_INTFLAG_READY & mask) : 0; }
static void hri_nvmctrl_write_CTRLA_reg(Nvmctrl *, uint32_t v) noexcept { Event(); Command(v); }
#endif

// ---------------------------------------------------------------------------------------------------------------------------------
// The real code

#if defined(MODEL_SAME5X)
# include "same5x.inc"
#else
# include "samc21.inc"
#endif

// ---------------------------------------------------------------------------------------------------------------------------------

static unsigned int runs = 0, failures = 0;

static void RunCase(const char *name, uint32_t primaskOnEntry, bool secured) noexcept
{
	uint32_t row[UserRowSize / 4];
	for (size_t i = 0; i < UserRowSize / 4; ++i)
	{
		row[i] = 0x5A000000u + (uint32_t)i * 0x01010101u;
	}
	memset(userRow, 0x33, UserRowSize);
	uint8_t userRowBefore[UserRowSize];
	memcpy(userRowBefore, userRow, UserRowSize);
	memset(pageBuffer, 0xFF, UserRowSize);
	memcpy(pageBufferAtLastEvent, pageBuffer, UserRowSize);
	primask = primaskOnEntry;
	fillsWithInterruptsOn = fillCommandsWithInterruptsOn = commandsWhileBusy = writeCommands = busyPolls = 0;
	ctrlaReg = 0x30;
	ctrlbReg = 0x00;
	securityBit = secured;
#if defined(MODEL_SAME5X)
	dsuModel.STATUSB.bit.PROT = (secured) ? 1 : 0;
#endif

	const int32_t rc = _user_row_write_exec(row);
	Event();

	std::string why;
	if (secured)
	{
		if (rc != ERR_DENIED) { why += " expected ERR_DENIED;"; }
		if (memcmp(userRow, userRowBefore, UserRowSize) != 0) { why += " the user row changed;"; }
	}
	else
	{
		if (rc != ERR_NONE) { why += " expected ERR_NONE;"; }
		if (memcmp(userRow, row, UserRowSize) != 0) { why += " the user row does not hold the new data;"; }
		if (fillsWithInterruptsOn != 0) { why += " page buffer written with interrupts on (" + std::to_string(fillsWithInterruptsOn) + " times);"; }
		if (fillCommandsWithInterruptsOn != 0) { why += " page buffer or write command issued with interrupts on (" + std::to_string(fillCommandsWithInterruptsOn) + " times);"; }
		if (commandsWhileBusy != 0) { why += " command issued while the NVMCTRL was busy;"; }
	}
	if (primask != primaskOnEntry) { why += " PRIMASK not restored;"; }

	++runs;
	const bool ok = why.empty();
	std::printf("%s " MODEL_NAME " %s: %u write commands%s\n", (ok) ? "PASS" : "FAIL", name, writeCommands, why.c_str());
	if (!ok)
	{
		++failures;
	}
}

int main()
{
	RunCase("called with interrupts on", 0, false);
	RunCase("called with interrupts off", 1, false);
	RunCase("security bit set", 0, true);
	std::printf("%u runs, %u failed\n", runs, failures);
	return (failures == 0) ? 0 : 1;
}
