# ADC take-turns host test (CoreN2G, SAMC21)

This test checks that the AIN task loop in CoreN2G `src/SAME5x_C21/SAMC21/AnalogIn.cpp` never lets ADC0 and the SDADC convert at the
same time (finding TL1), and that no TMC UART transfer runs while an ADC DMA channel is enabled (finding TL3, section "DMA transfer
mutex" below). The file is used on the SAMC21 boards (EXP1XD, TOOL1LC, SAMMYC21, SZP, `Boards.cmake:38`). Of the release boards, TOOL1LC
and SZP enable the SDADC (`SUPPORT_SDADC 1` in `src/Config/TOOL1LC.h` and `src/Config/SZP.h`).

## Why

On the TOOL1LC the SDADC always converts its input pair 0, which is PA07 (INP0, Vref) and PA06 (INN0, Vssa):
- `src/Platform/Platform.cpp` calls `SetupThermistorFilter(VrefPin, SdAdcVrefFilterIndex, true)`;
- the pin table in `src/Config/TOOL1LC.h` (PA06, PA07) names the pins.

ADC0 converts the same two pins as Vssa and Vref. Before the fix the task loop started both converters whenever they were free, so
they converted at the same time. Both run from the 48 MHz clock, so the timing of the software start decides how the two line up. That
depends on the code layout of the build. In some builds ADC0 read Vssa too high and Vref too low, and the thermistor reading was
several degrees low (forum thread 39227 for 3.6.3; again on 3.7.0+1.mp.6-dev.1).

Lab printer, 2026-11-26, TOOL1LC on 3.7.0+1.mp.6-dev.1, heaters off. The SDADC restarts were stopped in the running image by clearing
`numChannelsEnabled` of the SDADC object with `M122 B20 P1007`:

| | ADC0 Vssa | ADC0 Vref | PT1000 on temp0 |
|---|---|---|---|
| both converters running | 1087 | 41700 | 8.8 C |
| SDADC stopped | 104 | 42147 | 24.5 C |
| SDADC running again | 1087 | 41700 | 8.8 C |

The values are counts, filter sum / 8. There were two runs with the same result. AnalogIn reported 0 timeouts and 0 errors, so the DMA
transfers were complete. The fault was in the readings themselves.

The fix makes the converters take turns: the loop starts a conversion only when no converter is converting, and goes round the
converters that have channels. A conversion that takes longer than `AdcConversionTimeout` is stopped (DMA channel off, `ReInit`) and
the next converter gets its turn. Before the fix a timed-out conversion was restarted on the same converter.

## What the test compiles

`AnalogIn.cpp` is compiled unchanged. The headers it includes are replaced by the stand-ins in `stubs/`: `CoreIO.h`, `DmacManager.h`,
`Cache.h`, `CoreNotifyIndices.h`, `RTOSIface/RTOSIface.h`, `hri_adc_c21.h` and `hri_sdadc_c21.h`, all declared in `stubs/model.h`. The
real `AnalogIn.h` is used. The register values in `stubs/model.h` are placeholders; the model reacts only to SWRST and to a write to
SWTRIG.

The model in `adc_take_turns_test.cpp`:
- A write to SWTRIG starts a conversion. It converts for a fixed time (ADC0 450 us, SDADC 430 us) until it finishes or a software reset
  stops it.
- When it finishes, the DMA channel bound to the converter by `SetTriggerSource` writes one result per channel and calls the completion
  callback, as the DMAC ISR does. Each result encodes the converter, its slot in the sequence and the sequence number.
- `TakeIndexed` and `delay` advance the simulated time. As in FreeRTOS they count from the current tick: `delay(1)` returns at the
  next tick and `TakeIndexed(index, 5)` times out at the fifth tick. Each call is one loop iteration; the scenario ends after a fixed
  number.
- The TMC task of the TOOL1LC (scenarios with a TMC task) competes for the DMA transfer mutex, see below.

The checks:
- ADC0 and the SDADC never convert at the same time, and no converter is started again while it converts;
- each converter that has channels converts regularly and waits at most 12 ms for its turn;
- every reading comes from its own slot, no result is delivered twice and there is one reading per conversion;
- no timeouts and no errors. In the two scenarios with a hung converter, its conversions must time out instead.
- the TL3 checks below.

The scenarios:
- `turns`: the TOOL1LC channels, ADC0 temp0, board type, Vssa, Vref and Vin, SDADC pair 0, with the TMC task;
- `adc0-only`: no SDADC channel and no TMC task, as on the SZP with temp0 on ADC0;
- `sdadc-enabled-later`: the SDADC channel is enabled while ADC0 is running, with the TMC task;
- `sdadc-hung` and `adc0-hung`: one converter never finishes, and the other one and the TMC task must still get their turns.

Without the TL1 fix (CoreN2G before #9), 4 of the 5 scenarios fail on the overlap check: `turns` with 4000 overlapping starts,
`sdadc-enabled-later` 3900, `sdadc-hung` 2179 and `adc0-hung` 2000. `adc0-only` has only one converter. All 5 fail the TL3 checks too.

## DMA transfer mutex (TL3)

Lab printer, 2026-11-26: since the TL1 fix the TOOL1LC had about 3.5 ADC conversion timeouts per minute. Diagnostic builds showed the
write-back descriptor of the ADC DMA channel overwritten with the state of channel 0, the TMC UART transmit channel. That is the
behaviour that SAM C20/C21 erratum 1.10.4 (DS80000740) describes: when several channels have their triggers active at the same time,
the DMAC may corrupt the write-back descriptors. The board is of silicon revision F, which the erratum lists. Details are in DSF
`design/open-findings.md`, TL3.

The fix gives the DMAC a mutex, `DmacManager::GetTransferMutex()` (SAMC21 only). The AIN task holds it from the start of a sweep until the
sweep has finished or timed out, and `DoTransaction` in `src/Movement/StepperDrivers/TMC22xx.cpp` holds it while its transfer runs
(`Tools/tmc-dma-lock-test` checks that side). The AIN task now converts one sweep per pass and starts each pass with `delay(1)`, so it
starts at most one sweep per tick. The callbacks are rate-limited to one per tick anyway (`ticksPerCall` 1 in `src/Platform/Platform.cpp`).
The `delay(1)` is also where the TMC task gets the mutex.

The TMC task model follows `DoTransaction` on the TOOL1LC: it takes the mutex, runs a transfer of 650 us (about a register read at
`DriversBaudRate` 200000, `src/Config/TOOL1LC.h`: 4 bytes out and 8 back, 10 bits each), releases the mutex and waits 2 ms
(`delay(2)`). A TMC task that waits for the mutex gets it when the AIN task releases it.

The TL3 checks:
- at most one conversion starts per tick;
- an ADC DMA channel is enabled only while the AIN task holds the mutex, and the callbacks run after it has released it;
- no TMC transfer starts or runs while an ADC DMA channel is enabled;
- the TMC task waits at most one sweep for the mutex (450 us in the model), or 6 ms (the conversion timeout of 5 ticks and one tick)
  when a converter hangs. The AIN task waits at most one TMC transfer;
- both converters and the TMC task still get their turns: without a hung converter at least one sweep per converter every 4 ms and one
  TMC transfer every 4 ms, with a hung converter one of each every 8 ms.

In the model the sweeps run at 1000 per second (`turns`: 2000 sweeps in 2001 ms) and the TMC task makes a transfer every 3 ms (668 in
2001 ms), against every 2.65 ms without the mutex.

Without the TL3 fix (CoreN2G 821c4ea, which has the TL1 fix), all 5 scenarios fail: `turns` with 2240 extra starts in a tick, 4000
channel starts without the mutex and 1632 TMC transfers during an ADC transfer; `sdadc-enabled-later` 2239, 4000 and 1623;
`sdadc-hung` 999, 2000 and 2357; `adc0-hung` 1000, 2000 and 2375; `adc0-only` 1100 and 2000.

Not covered:
- the analog effect itself. That rests on the lab measurement above.
- the real conversion times and the reading rate on the board;
- the DMAC and the ADC registers beyond start, reset and completion;
- the write-back corruption itself. The test checks only that the channels of the two tasks never run at the same time; whether that
  removes the timeouts on the board is for the lab check;
- other DMA users that can run at the same time as an ADC sweep, such as the LED channel of the TOOL1LC or the I2C channel of the SZP,
  and the TMC transmit and receive channels, which run at the same time as each other.

`make check` builds the test normally and with AddressSanitizer and UBSan, and runs both. `ANALOGIN=` selects another copy of
`AnalogIn.cpp`, for example the one from before the fix, and `BUILD=` another output folder. The Makefile reads CoreN2G from next to
this repository, `../../../CoreN2G` (override with `COREN2G=`). The firmware builds do not use this folder: make and CMake only read
`src/`, and the `*-test` resource filter in `.project` hides it from Eclipse.
