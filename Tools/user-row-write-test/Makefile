# Host test of _user_row_write_exec in CoreN2G, the function behind _user_area_write that rewrites the NVM user row on the SAME5x and the
# SAMC21. The real text of the function is extracted at build time from both HPL files. user_row_write_test.cpp runs it against a model
# of the NVMCTRL and checks that the page buffer is filled with interrupts off. Run with make in WSL (g++ 13).
#
#   make              build build/user-row-write-test-same5x and build/user-row-write-test-samc21
#   make asan         the same with AddressSanitizer and UBSan, build/asan-user-row-write-test-*
#   make check        build all four and run them

COREN2G ?= ../../../CoreN2G

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

SAME5X_HPL := $(COREN2G)/src/SAME5x_C21/SAME5x/hpl/nvmctrl/hpl_nvmctrl.c
SAMC21_HPL := $(COREN2G)/src/SAME5x_C21/SAMC21/hpl/nvmctrl/hpl_nvmctrl.c

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $<)" -v names="$(1)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

PROGRAMS := build/user-row-write-test-same5x build/user-row-write-test-samc21
ASANPROGRAMS := build/asan-user-row-write-test-same5x build/asan-user-row-write-test-samc21

.PHONY: all asan check clean
all: $(PROGRAMS)
asan: $(ASANPROGRAMS)

gen/same5x.inc: $(SAME5X_HPL) extract.awk Makefile
	$(call EXTRACT,_user_row_write_exec)

gen/samc21.inc: $(SAMC21_HPL) extract.awk Makefile
	$(call EXTRACT,_user_row_write_exec)

build/user-row-write-test-%: user_row_write_test.cpp gen/%.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -DMODEL_$(shell echo $* | tr a-z A-Z) -Igen user_row_write_test.cpp -o $@

build/asan-user-row-write-test-%: user_row_write_test.cpp gen/%.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -DMODEL_$(shell echo $* | tr a-z A-Z) -Igen user_row_write_test.cpp -o $@

check: all asan
	@status=0; for p in $(PROGRAMS) $(ASANPROGRAMS); do echo "== $$p"; $$p || status=1; done; exit $$status

clean:
	rm -rf build gen
