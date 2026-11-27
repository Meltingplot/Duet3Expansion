# TMC DMA lock host test (TOOL1LC)

This test checks that `DoTransaction` in `src/Movement/StepperDrivers/TMC22xx.cpp`, as the TOOL1LC builds it, holds the DMA transfer
mutex of CoreN2G (`DmacManager::GetTransferMutex()`, SAMC21 only) while its UART transfer runs, and only then. It is the TMC side of the
fix for finding TL3. `Tools/adc-take-turns-test` checks the AIN side and the two tasks together.

## Why

Lab printer, 2026-11-26: since the TL1 fix (Meltingplot/CoreN2G#9) the TOOL1LC had about 3.5 ADC conversion timeouts per minute.
Diagnostic builds showed the write-back descriptor of the ADC DMA channel overwritten with the state of channel 0, the TMC UART transmit
channel (`DmacChanTmcTx` in `src/Config/TOOL1LC.h`). SAM C20/C21 erratum 1.10.4 (DS80000740) describes this: when several channels have
their triggers active at the same time, the DMAC may corrupt the write-back descriptors. The board is of silicon revision F, which the
erratum lists. Details are in DSF `design/open-findings.md`, TL3.

With the fix the AIN task holds the mutex while an ADC sweep converts, and `DoTransaction` holds it from before `StartTransfer` enables
the two TMC channels until they are off again:
- after a completed transfer the DMAC has disabled both channels by itself, so `DoTransaction` releases the mutex before
  `UartTmcHandler` and `delay(2)`;
- after a DMA error `DmaError` disables them (`AbortTransfer`), then the mutex is released before `delay(2)`;
- after a timeout `TransferTimedOut` disables them, then the mutex is released and the task yields. There is no `delay(2)` on that path,
  before or after the fix.

On the TOOL1LC, the only SAMC21 board with a TMC22xx driver, the AIN and TMC tasks have the same priority (`AinPriority` and `TmcOpenLoop`
2, `src/Platform/TaskPriorities.h`). Releasing a FreeRTOS mutex makes a waiting task ready, but switches to it only if it has a higher
priority (`xTaskRemoveFromEventList` in FreeRTOS `tasks.c`). So `DoTransaction` must yield or block after it has released the mutex, else
the TMC task loop (`DriversState::ready` calls it again at once) takes the mutex again before the AIN task can run. After a completed
transfer or a DMA error `delay(2)` blocks. After a timeout nothing did: while the driver did not answer, the TMC task held the mutex for
`TransferTimeout` (10 ticks) at a time, and the AIN task got it only if a tick fell between the release and the next take. Its sweeps,
and with them the temperature readings, would nearly stop. Not seen on the board: the lab runs had no TMC timeouts.

## What the test compiles

The Makefile extracts the real text of `DoTransaction` with `extract.awk` (the same script as `Tools/m915-driver-number-test`), and
`TransferTimeout` and `DmacChanTmcRx` from `src/Config/TOOL1LC.h`. `tmc_dma_lock_test.cpp` sets the configuration macros as the TOOL1LC
build has them (`TMC22xx_USES_SERCOM`, `TMC22xx_SINGLE_DRIVER`, no mux, no slave addresses, `SAMC21`) and models what the function calls:
- `StartTransfer` enables the TMC DMA channels;
- `TaskBase::TakeIndexed` returns as `TransferCompleteCallback` would make it: complete (the channels are off), DMA error, complete with
  error, or timeout (the channels are still on);
- `TransferTimedOut` and `DmaError` disable the channels, as `AbortTransfer` does;
- `Mutex` and `MutexLocker` record when the mutex is taken and released. `MutexLocker::Release` is non-counting, as in RRFLibraries
  `src/RTOSIface/RTOSIface.cpp`.

The checks, for each of the four outcomes:
- the return value, the calls of `UartTmcHandler`, `TransferTimedOut` and `DmaError`, the timeout and the number of `delay(2)` are as
  before the fix;
- the channels are enabled only while the mutex is held, the mutex is held while the transfer runs, and it is never released while a
  channel is enabled;
- the mutex is not held in `delay(2)` or while `UartTmcHandler` processes the reply;
- after the mutex is released the task yields or blocks before `DoTransaction` returns, and it never yields while it holds the mutex;
- it is taken once and released once, and the channels are off when `DoTransaction` returns.

Without the fix (Duet3Expansion 974e1645a) all 4 scenarios fail: the channels run without the mutex and it is never taken. Without the
yield after a timeout (Duet3Expansion cb86a7039) the `timeout` scenario fails.

Not covered:
- a wake-up without a DMA callback. In the TOOL1LC build only `TransferCompleteCallback` gives `NotifyIndices::Tmc` (the UART handler
  next to it is for the builds without SERCOM), and `DoTransaction` clears the count before it starts the transfer;
- the TMC transmit and receive channels, which run at the same time as each other;
- other boards. Of the SAMC21 boards (`Boards.cmake`: EXP1XD, TOOL1LC, SAMMYC21, SZP; and ATECM, ATEIO) only the TOOL1LC has a TMC22xx
  driver. The TMC51xx boards EXP1HCL and M23CL are SAME5x boards, which this SAM C20/C21 erratum does not concern.

`make check` builds the test normally and with AddressSanitizer and UBSan, and runs both. `TMC22XX=` selects another copy of
`TMC22xx.cpp`, for example the one from before the fix. The firmware builds do not use this folder: make and CMake only read `src/`, and
the `*-test` resource filter in `.project` hides it from Eclipse.
