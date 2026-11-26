# ADC take-turns host test (CoreN2G, SAMC21)

This test checks that the AIN task loop in CoreN2G `src/SAME5x_C21/SAMC21/AnalogIn.cpp` never lets ADC0 and the SDADC convert at the
same time. The file is used on the SAMC21 boards (EXP1XD, TOOL1LC, SAMMYC21, SZP, `Boards.cmake:38`). Of the release boards, TOOL1LC
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
- `TakeIndexed` and `delay` advance the simulated time. Each call is one loop iteration; the scenario ends after a fixed number.

The checks:
- ADC0 and the SDADC never convert at the same time, and no converter is started again while it converts;
- each converter that has channels converts regularly and waits at most 12 ms for its turn;
- every reading comes from its own slot, no result is delivered twice and there is one reading per conversion;
- no timeouts, no errors and no `delay()` while there are channels to convert. In the two scenarios with a hung converter, its
  conversions must time out instead.

The scenarios:
- `turns`: the TOOL1LC channels, ADC0 temp0, board type, Vssa, Vref and Vin, SDADC pair 0;
- `adc0-only`: no SDADC channel, as on the SZP with temp0 on ADC0;
- `sdadc-enabled-later`: the SDADC channel is enabled while ADC0 is running;
- `sdadc-hung` and `adc0-hung`: one converter never finishes, and the other one must still get its turns.

Without the fix, 4 of the 5 scenarios fail on the overlap check: `turns` with 4000 overlapping starts, `sdadc-enabled-later` 3900,
`sdadc-hung` 2179 and `adc0-hung` 2000. `adc0-only` passes, as it has only one converter.

Not covered:
- the analog effect itself. That rests on the lab measurement above.
- the real conversion times and the reading rate on the board;
- the DMAC and the ADC registers beyond start, reset and completion.

`make check` builds the test normally and with AddressSanitizer and UBSan, and runs both. `ANALOGIN=` selects another copy of
`AnalogIn.cpp`, for example the one from before the fix, and `BUILD=` another output folder. The Makefile reads CoreN2G from next to
this repository, `../../../CoreN2G` (override with `COREN2G=`). The firmware builds do not use this folder: make and CMake only read
`src/`, and the `*-test` resource filter in `.project` hides it from Eclipse.
