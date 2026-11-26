# Host test of the AIN task loop in CoreN2G src/SAME5x_C21/SAMC21/AnalogIn.cpp: the ADC and the SDADC must take turns. The file is
# compiled unchanged against the stand-ins in stubs/, adc_take_turns_test.cpp models the converters and the DMAC. Run with make in WSL.
#
#   make              build build/adc-take-turns-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-adc-take-turns-test
#   make check        build both and run them
#
# ANALOGIN selects the file under test, for example a copy from before the fix.

COREN2G ?= ../../../CoreN2G
ANALOGIN ?= $(COREN2G)/src/SAME5x_C21/SAMC21/AnalogIn.cpp
BUILD ?= build

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-missing-field-initializers
INCLUDES := -DRTOS -Istubs -I$(COREN2G)/src			# AnalogIn.cpp tests RTOS before it includes CoreIO.h; the firmware makefiles define it
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

SOURCES := adc_take_turns_test.cpp $(ANALOGIN)
HEADERS := $(wildcard stubs/*.h stubs/*/*.h)

.PHONY: all asan check clean
all: $(BUILD)/adc-take-turns-test
asan: $(BUILD)/asan-adc-take-turns-test

$(BUILD)/adc-take-turns-test: $(SOURCES) $(HEADERS) Makefile
	@mkdir -p $(BUILD)
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(INCLUDES) $(SOURCES) -o $@

$(BUILD)/asan-adc-take-turns-test: $(SOURCES) $(HEADERS) Makefile
	@mkdir -p $(BUILD)
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) $(INCLUDES) $(SOURCES) -o $@

check: all asan
	@status=0; for p in $(BUILD)/adc-take-turns-test $(BUILD)/asan-adc-take-turns-test; do echo "== $$p"; $$p || status=1; done; exit $$status

clean:
	rm -rf $(BUILD)
