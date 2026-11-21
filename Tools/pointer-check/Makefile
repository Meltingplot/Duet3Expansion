# Check that Duet3Expansion declares no new reseatable raw pointer, see README.md. Needs the Arm GNU toolchain and python3, e.g.
# in the firmware build container of DuetSoftwareFramework (firmware/build.sh shell).
#
#   make check        build the boards with -g, then fail for each raw pointer that allowlist.txt does not name
#   make scan         the same check on the ELF files that are already there, without building
#   make update       build, then rewrite allowlist.txt with the pointers found (keeps the comments of existing entries)
#   make selftest     check pointer-check.py against selftest/src/sample.cpp and selftest/src/no-pointers.cpp
#
# CONFIGS selects the boards, default those that the release workflow builds (BOARDS in .github/workflows/mp-release.yml).

D3E ?= ../..
CONFIGS ?= TOOL1LC EXP1HCL SZP
CROSS_COMPILE ?= arm-none-eabi-
PYTHON ?= python3
ALLOWLIST := allowlist.txt
CHECK := $(PYTHON) -I pointer-check.py --readelf $(CROSS_COMPILE)readelf

# Each board's ELF, e.g. TOOL1LC/Duet3Firmware_TOOL1LC.elf
ELFS = $(foreach c,$(CONFIGS),$(D3E)/$(c)/Duet3Firmware_$(c).elf)

# The cores of the boards, with the -mcpu and FPU flags of their makefiles: SAMC21 (TOOL1LC, SZP, EXP1XD, SAMMYC21), SAME51
# (EXP1HCL and the other SAME51 boards) and STM32H5 (NodeTrix)
SELFTEST_CORES := m0plus m4 m33
CORE_FLAGS_m0plus := -mcpu=cortex-m0plus
CORE_FLAGS_m4 := -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CORE_FLAGS_m33 := -mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard

.PHONY: build check scan update selftest $(SELFTEST_CORES:%=selftest-%) selftest-update clean

# -B rebuilds every object, so none is left from a build without -g. DEBUG_FLAGS=-g on the command line replaces the
# Makefile's DEBUG_FLAGS; DEBUG=1 would also add -Og -DDEBUG. -g changes only the debug sections, not the code
build:
	for c in $(CONFIGS); do $(MAKE) -C $(D3E) -B DEBUG_FLAGS=-g $$c || exit 1; done

check: build
	$(CHECK) --allowlist $(ALLOWLIST) $(ELFS)

scan:
	$(CHECK) --allowlist $(ALLOWLIST) $(ELFS)

update: build
	$(CHECK) --allowlist $(ALLOWLIST) --update $(ELFS)

# -O3 as in the board makefiles
build/selftest/sample-%.o: selftest/src/sample.cpp Makefile
	@mkdir -p build/selftest
	cd selftest && $(CROSS_COMPILE)g++ -std=c++20 $(CORE_FLAGS_$*) -mthumb -O3 -g -c src/sample.cpp -o $(abspath $@)

$(SELFTEST_CORES:%=selftest-%): selftest-%: build/selftest/sample-%.o
	$(CHECK) --root selftest --keys $< > build/selftest/keys-$*.txt
	diff -u selftest/expected.txt build/selftest/keys-$*.txt
	$(CHECK) --root selftest --allowlist selftest/expected.txt $<
	! $(CHECK) --root selftest $< > build/selftest/without-allowlist-$*.txt
	grep -q 'error: new raw pointer that can be reseated: Sample::plain (int \*)' build/selftest/without-allowlist-$*.txt

# --update on a file without pointers must write the header and no entries; the core does not matter here
build/selftest/no-pointers.o: selftest/src/no-pointers.cpp Makefile
	@mkdir -p build/selftest
	cd selftest && $(CROSS_COMPILE)g++ -std=c++20 $(CORE_FLAGS_m0plus) -mthumb -O3 -g -c src/no-pointers.cpp -o $(abspath $@)

selftest-update: build/selftest/no-pointers.o
	printf '# header\nSample::gone  # an entry of a pointer that no longer exists\n' > build/selftest/update.txt
	$(CHECK) --root selftest --allowlist build/selftest/update.txt --update $<
	printf '# header\n' | diff -u - build/selftest/update.txt

selftest: $(SELFTEST_CORES:%=selftest-%) selftest-update
	@echo "selftest passed"

clean:
	rm -rf build
