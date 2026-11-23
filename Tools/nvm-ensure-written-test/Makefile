# Host test of NonVolatileMemory::EnsureWritten on the SAMC21: the real text of the function from src/Hardware/NonVolatileMemory.cpp,
# extracted at build time. nvm_ensure_written_test.cpp runs it against a model of the NVMCTRL that stays busy for a while after each
# command. Run with make in WSL (g++ 13).
#
#   make              build build/nvm-ensure-written-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-nvm-ensure-written-test
#   make check        build both and run them

D3E ?= ../..

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $<)" -v names="$(1)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

GEN := gen/NonVolatileMemory.inc

.PHONY: all asan check clean
all: build/nvm-ensure-written-test
asan: build/asan-nvm-ensure-written-test

gen/NonVolatileMemory.inc: $(D3E)/src/Hardware/NonVolatileMemory.cpp extract.awk Makefile
	$(call EXTRACT,NonVolatileMemory::EnsureWritten)

build/nvm-ensure-written-test: nvm_ensure_written_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen nvm_ensure_written_test.cpp -o $@

build/asan-nvm-ensure-written-test: nvm_ensure_written_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen nvm_ensure_written_test.cpp -o $@

check: all asan
	build/nvm-ensure-written-test
	build/asan-nvm-ensure-written-test

clean:
	rm -rf build gen
