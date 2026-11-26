# Host test of the lock that keeps Tasks::AllocPermanent from racing malloc: the real text of the malloc mutex functions and Tasks::AllocPermanent
# from src/Platform/Tasks.cpp and of _sbrk and CoreAllocPermanent from CoreN2G's src/syscalls.h, extracted at build time, with
# the interrupt mask, the mutex and the scheduler state played in alloc_permanent_lock_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/alloc-permanent-lock-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-alloc-permanent-lock-test
#   make tsan         the same with ThreadSanitizer, build/tsan-alloc-permanent-lock-test
#   make check        build all three and run them: plain and ASan the cases on one thread, TSan the case with two threads
#
# COREN2G is the CoreN2G checkout that Duet3Expansion builds with, by default the one next to it.

D3E ?= ../..
COREN2G ?= $(D3E)/../CoreN2G

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra -pthread
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
TSANFLAGS := -fsanitize=thread
# ThreadSanitizer of g++ 13 stops with "unexpected memory mapping" under the address space layout of newer kernels; setarch -R
# runs it without randomisation
SETARCH := setarch $(shell uname -m) -R
TEST := build/alloc-permanent-lock-test

.PHONY: all asan tsan check clean FORCE
all: $(TEST)
asan: build/asan-alloc-permanent-lock-test
tsan: build/tsan-alloc-permanent-lock-test

# Extracted on every build, so that a test against another COREN2G or D3E never uses an old copy
gen/Tasks.inc: FORCE extract.awk
	@mkdir -p gen
	awk -v syscalls=gen/syscalls.inc.tmp -f extract.awk $(abspath $(D3E)/src/Platform/Tasks.cpp) $(abspath $(COREN2G)/src/syscalls.h) > $@.tmp
	mv gen/syscalls.inc.tmp gen/syscalls.inc && mv $@.tmp $@

$(TEST): alloc_permanent_lock_test.cpp gen/Tasks.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen alloc_permanent_lock_test.cpp -o $@

build/asan-alloc-permanent-lock-test: alloc_permanent_lock_test.cpp gen/Tasks.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen alloc_permanent_lock_test.cpp -o $@

build/tsan-alloc-permanent-lock-test: alloc_permanent_lock_test.cpp gen/Tasks.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(TSANFLAGS) -Igen alloc_permanent_lock_test.cpp -o $@

check: all asan tsan
	$(TEST)
	build/asan-alloc-permanent-lock-test
	TSAN_OPTIONS="halt_on_error=1 exitcode=66" $(SETARCH) build/tsan-alloc-permanent-lock-test threads

clean:
	rm -rf build gen

FORCE:
