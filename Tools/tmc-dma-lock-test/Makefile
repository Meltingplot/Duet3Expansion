# Host test of DoTransaction (src/Movement/StepperDrivers/TMC22xx.cpp) as the TOOL1LC builds it: the real text of the function, extracted
# at build time, with TransferTimeout and DmacChanTmcRx from src/Config/TOOL1LC.h. tmc_dma_lock_test.cpp runs it against a model of the
# TMC UART transfer and of the DMA transfer mutex (CoreN2G DmacManager::GetTransferMutex). Run with make in WSL (g++ 13).
#
#   make              build build/tmc-dma-lock-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-tmc-dma-lock-test
#   make check        build both and run them
#
# TMC22XX selects the file under test, for example a copy from before the fix.

D3E ?= ../..
TMC22XX ?= $(D3E)/src/Movement/StepperDrivers/TMC22xx.cpp

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $<)" -v names="$(1)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

GEN := gen/TMC22xx.inc gen/Config.inc

.PHONY: all asan check clean FORCE
all: build/tmc-dma-lock-test
asan: build/asan-tmc-dma-lock-test

# FORCE, because TMC22XX may name another file than the last build
gen/TMC22xx.inc: $(TMC22XX) extract.awk Makefile FORCE
	$(call EXTRACT,DoTransaction)

# The two constants as the TOOL1LC defines them. Fails unless both are found
gen/Config.inc: $(D3E)/src/Config/TOOL1LC.h Makefile
	@mkdir -p gen
	grep -hE '^constexpr (uint32_t TransferTimeout|DmaChannel DmacChanTmcRx) = ' $< | sed -e 's|//.*||' | tr -d '\r' > $@.tmp
	test "$$(wc -l < $@.tmp)" -eq 2 && mv $@.tmp $@

build/tmc-dma-lock-test: tmc_dma_lock_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen tmc_dma_lock_test.cpp -o $@

build/asan-tmc-dma-lock-test: tmc_dma_lock_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen tmc_dma_lock_test.cpp -o $@

check: all asan
	build/tmc-dma-lock-test
	build/asan-tmc-dma-lock-test

clean:
	rm -rf build gen
