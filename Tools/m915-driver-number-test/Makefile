# Host test of the driver numbers of an M915 request from the main board: the real text of ProcessM915
# (src/CommandProcessing/CommandProcessor.cpp) and of Move::SetOrResetEventOnStall and Move::GetEventOnStallText (src/Movement/Move.cpp),
# extracted at build time, with NumDrivers of the EXP1HCL (src/Config/EXP1HCL.h). Run with make in WSL (g++ 13).
#
#   make              build build/m915-driver-number-test
#   make asan         the same with AddressSanitizer and UBSan, which stop at the first report, build/asan-m915-driver-number-test
#   make check        build both and run them

D3E ?= ../..
RRFLIBS ?= $(D3E)/../RRFLibraries

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I$(RRFLIBS)/src -Igen
ASANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $<)" -v names="$(1)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

GEN := gen/CommandProcessor.inc gen/Move.inc gen/Sizes.inc

.PHONY: all asan check clean
all: build/m915-driver-number-test
asan: build/asan-m915-driver-number-test

gen/CommandProcessor.inc: $(D3E)/src/CommandProcessing/CommandProcessor.cpp extract.awk Makefile
	$(call EXTRACT,ProcessM915)

gen/Move.inc: $(D3E)/src/Movement/Move.cpp extract.awk Makefile
	$(call EXTRACT,Move::SetOrResetEventOnStall Move::GetEventOnStallText)

# NumDrivers as the EXP1HCL defines it. Fails unless it is found
gen/Sizes.inc: $(D3E)/src/Config/EXP1HCL.h Makefile
	@mkdir -p gen
	grep -hE '^constexpr size_t NumDrivers = ' $< | sed -e 's|//.*||' | tr -d '\r' > $@.tmp
	test "$$(wc -l < $@.tmp)" -eq 1 && mv $@.tmp $@

build/m915-driver-number-test: m915_driver_number_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $< -o $@

build/asan-m915-driver-number-test: m915_driver_number_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) $< -o $@

check: all asan
	build/m915-driver-number-test
	build/asan-m915-driver-number-test

clean:
	rm -rf build gen
