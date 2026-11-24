// Host test of the driver numbers of an M915 request from the main board: the real text of ProcessM915
// (src/CommandProcessing/CommandProcessor.cpp) and of Move::SetOrResetEventOnStall and Move::GetEventOnStallText (src/Movement/Move.cpp),
// which the Makefile extracts, with the one driver of the EXP1HCL (src/Config/EXP1HCL.h), a closed-loop board. See README.md.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <General/Bitmap.h>

#include "Sizes.inc"											// NumDrivers of the EXP1HCL

#define HAS_SMART_DRIVERS		1								// as in src/Config/EXP1HCL.h
#define HAS_STALL_DETECT		1
#define SUPPORT_CLOSED_LOOP		1
#define _ecv_array

typedef Bitmap<uint16_t> LocalDriversBitmap;					// as in CANlib src/Duet3Common.h

enum class GCodeResult : uint8_t { ok, error };

// Just what the extracted functions use of StringRef (RRFLibraries General/StringRef.h)
class StringRef
{
public:
	explicit StringRef(std::string& s) noexcept : str(s) { }
	void copy(const char *s) const noexcept { str = s; }
	void catf(const char *fmt, ...) const noexcept __attribute__((format(printf, 2, 3)))
	{
		va_list args;
		va_start(args, fmt);
		Append(fmt, args);
		va_end(args);
	}
	void lcatf(const char *fmt, ...) const noexcept __attribute__((format(printf, 2, 3)))
	{
		if (!str.empty()) { str += '\n'; }
		va_list args;
		va_start(args, fmt);
		Append(fmt, args);
		va_end(args);
	}

private:
	void Append(const char *fmt, va_list args) const noexcept
	{
		char buf[100];
		vsnprintf(buf, sizeof(buf), fmt, args);
		str += buf;
	}

	std::string& str;
};

namespace CanInterface
{
	inline unsigned int GetCanAddress() noexcept { return 121; }
}

// The M915 request: CanMessageGenericParser plays the parameters that the test sets in 'params'
struct CanMessageGeneric { };
struct ParamDescriptor { };
static const ParamDescriptor M915Params[1] = { };
static std::map<char, int> params;

class CanMessageGenericParser
{
public:
	CanMessageGenericParser(const CanMessageGeneric&, const ParamDescriptor *) noexcept { }
	template<class T> bool GetUintParam(char c, T& v) const noexcept { return Get(c, v); }
	template<class T> bool GetIntParam(char c, T& v) const noexcept { return Get(c, v); }

private:
	template<class T> bool Get(char c, T& v) const noexcept
	{
		const auto it = params.find(c);
		if (it == params.end()) { return false; }
		v = (T)it->second;
		return true;
	}
};

// The smart driver functions record the drivers they are called for; the real ones check the number (TMC51xx.cpp)
enum class SmartDriverRegister : uint8_t { coolStep };
static std::vector<unsigned int> smartDriverCalls;

namespace SmartDrivers
{
	void SetStallThreshold(size_t driver, int) noexcept { smartDriverCalls.push_back(driver); }
	void SetStallFilter(size_t driver, unsigned int) noexcept { smartDriverCalls.push_back(driver); }
	void SetStallMinimumStepsPerSecond(size_t driver, unsigned int) noexcept { smartDriverCalls.push_back(driver); }
	void SetRegister(size_t driver, SmartDriverRegister, uint32_t) noexcept { smartDriverCalls.push_back(driver); }
	void AppendStallConfig(size_t driver, const StringRef&) noexcept { smartDriverCalls.push_back(driver); }
}

// The members of Move (src/Movement/Move.h) that the extracted functions use. dms is a heap array of exactly NumDrivers entries, so
// that AddressSanitizer sees a read beyond it
struct ClosedLoop
{
	bool IsClosedLoopEnabled() const noexcept { return enabled; }
	bool enabled = false;
};

struct DriveMovement
{
	ClosedLoop closedLoopControl;
};

class Move
{
public:
	Move() noexcept : dms(new DriveMovement[NumDrivers]) { }

	void SetOrResetEventOnStall(LocalDriversBitmap drivers, bool enable) noexcept;
	const char *_ecv_array GetEventOnStallText(unsigned int driver) noexcept;

	LocalDriversBitmap eventOnStallDrivers;
	std::unique_ptr<DriveMovement[]> dms;
};

static Move *moveInstance;

#include "Move.inc"
#include "CommandProcessor.inc"

static unsigned int passed = 0, failed = 0;

static void Report(const char *name, const std::string& why) noexcept
{
	if (why.empty())
	{
		++passed;
		printf("PASS %s\n", name);
	}
	else
	{
		++failed;
		printf("FAIL %s:%s\n", name, why.c_str());
	}
}

// An M915 request with the given parameters; the result and the reply
static GCodeResult M915(std::map<char, int> p, std::string& reply) noexcept
{
	params = p;
	smartDriverCalls.clear();
	reply.clear();
	StringRef r(reply);
	return ProcessM915(CanMessageGeneric(), r);
}

int main()
{
	printf("NumDrivers = %zu (EXP1HCL)\n", NumDrivers);

	{
		Move move;
		moveInstance = &move;
		std::string reply, why;
		if (M915({ { 'd', 0x0001 }, { 'R', 1 } }, reply) != GCodeResult::ok) { why += " R1 for driver 0 failed: " + reply; }
		if (!move.eventOnStallDrivers.IsBitSet(0)) { why += " driver 0 has no event on stall"; }
		if (M915({ { 'd', 0x0001 } }, reply) != GCodeResult::ok) { why += " the report for driver 0 failed"; }
		if (reply != "Driver 121.0: , event on stall: yes") { why += " report \"" + reply + "\""; }
		Report("M915 for driver 0 sets and reports the event on stall", why);
	}

	{
		Move move;
		moveInstance = &move;
		std::string reply, why;
		if (M915({ { 'd', 0x0020 }, { 'R', 1 } }, reply) != GCodeResult::error) { why += " R1 for driver 5 was not refused"; }
		if (reply != "No such driver 121.5") { why += " reply \"" + reply + "\""; }
		if (move.eventOnStallDrivers.IsBitSet(5)) { why += " driver 5 got an event on stall"; }
		if (M915({ { 'd', 0x0020 } }, reply) != GCodeResult::error) { why += " the report for driver 5 was not refused"; }
		Report("M915 for driver 5, which the board does not have, is refused and changes nothing", why);
	}

	{
		Move move;
		moveInstance = &move;
		std::string reply, why;
		if (M915({ { 'd', 0x8001 }, { 'S', 5 }, { 'F', 1 }, { 'H', 200 }, { 'T', 7 }, { 'R', 1 } }, reply) != GCodeResult::error)
		{
			why += " the request for drivers 0 and 15 was not refused";
		}
		if (reply != "No such driver 121.15") { why += " reply \"" + reply + "\""; }
		if (!smartDriverCalls.empty()) { why += " " + std::to_string(smartDriverCalls.size()) + " smart driver calls"; }
		if (!move.eventOnStallDrivers.IsEmpty()) { why += " a driver got an event on stall"; }
		Report("M915 for drivers 0 and 15 is refused as a whole, as the main board refuses its own", why);
	}

	{
		// The order of the finding: R1 for a driver that the board does not have, then the report for it
		Move move;
		moveInstance = &move;
		std::string reply, why;
		(void)M915({ { 'd', 0xFFFF }, { 'R', 1 } }, reply);
		(void)M915({ { 'd', 0xFFFF } }, reply);
		for (unsigned int d = NumDrivers; d < 16; ++d)
		{
			if (move.eventOnStallDrivers.IsBitSet(d)) { why += " driver " + std::to_string(d) + " has an event on stall"; }
		}
		Report("M915 R1 for drivers 0 to 15 and then the report for them read nothing beyond the drivers", why);
	}

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
