# Host test of the sync loss timeout of the CAN interface: the real text of CanInterface::UpdateSyncLockState from
# src/CAN/CanInterface.cpp, extracted at build time. sync_timeout_test.cpp runs it with a sync message that arrives right after the
# clock read. Run with make in WSL (g++ 13).
#
#   make              build build/sync-timeout-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-sync-timeout-test
#   make check        build both and run them

D3E ?= ../..

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $<)" -v names="$(1)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

GEN := gen/CanInterface.inc

.PHONY: all asan check clean
all: build/sync-timeout-test
asan: build/asan-sync-timeout-test

gen/CanInterface.inc: $(D3E)/src/CAN/CanInterface.cpp extract.awk Makefile
	$(call EXTRACT,CanInterface::UpdateSyncLockState)

build/sync-timeout-test: sync_timeout_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen sync_timeout_test.cpp -o $@

build/asan-sync-timeout-test: sync_timeout_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen sync_timeout_test.cpp -o $@

check: all asan
	build/sync-timeout-test
	build/asan-sync-timeout-test

clean:
	rm -rf build gen
