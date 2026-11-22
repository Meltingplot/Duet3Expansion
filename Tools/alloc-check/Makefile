# Check that Duet3Expansion reaches the heap from no new place inside a protected region, and that no new place lifts the mask
# of a region that masks interrupts, see README.md. Needs the Arm GNU toolchain and python3, e.g. in the firmware build
# container of DuetSoftwareFramework (firmware/build.sh shell).
#
#   make check        build the boards with -g (as Tools/pointer-check does), then run both checks
#   make scan         the heap check on the ELF files that are already there, without building: fail for each finding that
#                     allowlist.txt does not name (the release workflow runs it after Tools/pointer-check has built them)
#   make scan-unmask  the unmask check on the same ELF files: fail for each finding that unmask-allowlist.txt does not name
#   make update       rewrite both allowlists with the findings of the ELF files that are there (keeps the comments of
#                     existing entries)
#   make selftest     check alloc-check.py against selftest/src/sample.cpp and selftest/src/no-alloc.cpp
#
# CONFIGS selects the boards, default those that the release workflow builds (BOARDS in .github/workflows/mp-release.yml).

D3E ?= ../..
CONFIGS ?= TOOL1LC EXP1HCL SZP
CROSS_COMPILE ?= arm-none-eabi-
PYTHON ?= python3
ALLOWLIST := allowlist.txt
UNMASK_ALLOWLIST := unmask-allowlist.txt
CHECK := $(PYTHON) -I alloc-check.py --objdump $(CROSS_COMPILE)objdump --nm $(CROSS_COMPILE)nm --addr2line $(CROSS_COMPILE)addr2line

# Each board's ELF, e.g. TOOL1LC/Duet3Firmware_TOOL1LC.elf
ELFS = $(foreach c,$(CONFIGS),$(D3E)/$(c)/Duet3Firmware_$(c).elf)

# The cores of the boards that the release builds, with the -mcpu and FPU flags of their makefiles: SAMC21 (TOOL1LC, SZP) and
# SAME51 (EXP1HCL). The Cortex-M0+ has no BASEPRI, so its selftest expects no [basepri] finding of the heap check. The unmask
# check has a list per core, because the critical sections of the two ports mask differently (see selftest/src/runtime.cpp)
SELFTEST_CORES := m0plus m4
CORE_FLAGS_m0plus := -mcpu=cortex-m0plus
CORE_FLAGS_m4 := -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard
EXPECTED_m0plus := grep -v '\[basepri\]' selftest/expected.txt
EXPECTED_m4 := cat selftest/expected.txt
# -O3 as in the board makefiles
SELFTEST_FLAGS := -std=gnu++20 -mthumb -O3 -g -fno-exceptions -fno-rtti -nostdlib -nostartfiles -Wl,-e,Reset_Handler

.PHONY: build check scan scan-unmask update selftest $(SELFTEST_CORES:%=selftest-%) clean

# The same build as Tools/pointer-check: the check reads the source lines from the debug information
build:
	$(MAKE) -C ../pointer-check build D3E=$(abspath $(D3E)) CONFIGS="$(CONFIGS)"

# Both checks, also when the first one fails
check: build
	$(MAKE) -k scan scan-unmask

scan:
	$(CHECK) --allowlist $(ALLOWLIST) $(ELFS)

scan-unmask:
	$(CHECK) --unmask --allowlist $(UNMASK_ALLOWLIST) $(ELFS)

update:
	$(CHECK) --allowlist $(ALLOWLIST) --update $(ELFS)
	$(CHECK) --unmask --allowlist $(UNMASK_ALLOWLIST) --update $(ELFS)

build/selftest/%/sample.elf build/selftest/%/no-alloc.elf: selftest/src/sample.cpp selftest/src/no-alloc.cpp selftest/src/runtime.cpp selftest/src/runtime.h Makefile
	@mkdir -p build/selftest/$*
	cd selftest && $(CROSS_COMPILE)g++ $(CORE_FLAGS_$*) $(SELFTEST_FLAGS) src/sample.cpp src/runtime.cpp -lgcc -o $(abspath build/selftest/$*/sample.elf)
	cd selftest && $(CROSS_COMPILE)g++ $(CORE_FLAGS_$*) $(SELFTEST_FLAGS) src/no-alloc.cpp src/runtime.cpp -lgcc -o $(abspath build/selftest/$*/no-alloc.elf)

$(SELFTEST_CORES:%=selftest-%): selftest-%: build/selftest/%/sample.elf build/selftest/%/no-alloc.elf
	$(CHECK) --root selftest --keys $< > build/selftest/$*/keys.txt
	$(EXPECTED_$*) | diff -u - build/selftest/$*/keys.txt
	$(CHECK) --root selftest --allowlist selftest/expected.txt $<
	! $(CHECK) --root selftest $< > build/selftest/$*/without-allowlist.txt
	grep -q 'src/sample.cpp:35: error: heap reached inside a protected region: SuspendedNew \[suspended\] -> operator new' build/selftest/$*/without-allowlist.txt
	grep -q 'src/sample.cpp:86: error: call through a register inside a protected region: SuspendedCallback' build/selftest/$*/without-allowlist.txt
	$(CHECK) --root selftest --unmask --keys $< > build/selftest/$*/unmask-keys.txt
	diff -u selftest/expected-unmask-$*.txt build/selftest/$*/unmask-keys.txt
	$(CHECK) --root selftest --unmask --allowlist selftest/expected-unmask-$*.txt $<
	! $(CHECK) --root selftest --unmask $< > build/selftest/$*/unmask-without-allowlist.txt
	grep -q 'src/sample.cpp:[0-9]*: error: interrupts unmasked inside a protected region: NestedEnable \[primask\] -> NestedEnable' build/selftest/$*/unmask-without-allowlist.txt
	printf '# header\nGone [suspended] -> malloc  # an entry of a finding that no longer exists\n' > build/selftest/$*/update.txt
	$(CHECK) --root selftest --allowlist build/selftest/$*/update.txt --update build/selftest/$*/no-alloc.elf
	printf '# header\n' | diff -u - build/selftest/$*/update.txt
	printf '# header\nGone [primask] -> vPortExitCritical  # an entry of a finding that no longer exists\n' > build/selftest/$*/update.txt
	$(CHECK) --root selftest --unmask --allowlist build/selftest/$*/update.txt --update build/selftest/$*/no-alloc.elf
	printf '# header\n' | diff -u - build/selftest/$*/update.txt

selftest: $(SELFTEST_CORES:%=selftest-%)
	@echo "selftest passed"

clean:
	rm -rf build
