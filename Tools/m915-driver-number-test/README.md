# M915 driver number host test

This test checks the driver numbers of an M915 request that the main board sends to an expansion board over CAN.

## Why

`ProcessM915` (`src/CommandProcessing/CommandProcessor.cpp`) takes the 16-bit driver bitmap `d` of the request as it is:
- The smart driver functions check the driver number themselves (`SmartDrivers::SetStallThreshold`, `AppendStallConfig` and the others
  in `src/Movement/StepperDrivers/TMC51xx.cpp` and `TMC22xx.cpp`).
- `R` stores every bit in `Move::eventOnStallDrivers` (`Move::SetOrResetEventOnStall`, `src/Movement/Move.cpp`).
- The report, an M915 without parameters, calls `Move::GetEventOnStallText` for every bit. On a closed-loop board it reads
  `dms[driver]` for a driver whose bit is set (`Move.cpp:1668`). `dms` has `NumDrivers` entries (`src/Movement/Move.h`), and the
  EXP1HCL, M23CL and TOOLINDX have one driver and `SUPPORT_CLOSED_LOOP` 1 (`src/Config/EXP1HCL.h`, `M23CL.h`, `TOOLINDX.h`).

So `M915 P<address>.5 R1` and then `M915 P<address>.5` read beyond `dms[]` on these boards. The main board passes remote driver numbers
on without checking them against the board; it checks only its own (`Move::ConfigureStallDetection`, RRF `src/Movement/Move.cpp`: an
error "Invalid local drive number" before anything is changed).

The fix does the same on the expansion board: a request that names a driver the board does not have is refused as a whole with
"No such driver <address>.<driver>", the message that 162803de (C. Hammacher, 2026-11-13) gave set-driver-states and stop-drivers.
Behaviour change: before, the drivers that exist got the new settings and the others were ignored, except for the event on stall bit.

Upstream code. Found by the census of CAN receive (error class 3, 2026-11-24). Not seen on a board.

## What the test compiles

The real text of `ProcessM915`, `Move::SetOrResetEventOnStall` and `Move::GetEventOnStallText`, which the Makefile extracts with
`extract.awk` (with `#line` directives that point at the real lines), and the real `NumDrivers` of the EXP1HCL.
`m915_driver_number_test.cpp` plays the rest:
- `CanMessageGenericParser` returns the parameters that a case sets;
- the `SmartDrivers` functions record the drivers they are called for;
- the `Move` stand-in has `eventOnStallDrivers` and a heap array `dms` of exactly `NumDrivers` entries, so that AddressSanitizer sees a
  read beyond it.

RRFLibraries `General/Bitmap.h` is the real one.

Four cases: R1 and the report for driver 0; R1 and the report for driver 5; a request with every parameter for drivers 0 and 15; R1 and
the report for drivers 0 to 15. Without the fix 3 fail, and the AddressSanitizer build stops with a heap-buffer-overflow in
`GetEventOnStallText` (`Move.cpp:1668`, called from `CommandProcessor.cpp:455`). With the fix all 4 pass.

Not covered: the main board side and what the smart drivers do with a setting.

## Build and run (WSL, g++ 13)

    make check        # build the plain and the AddressSanitizer + UBSan program, then run both

To see the failure, run it against the source without the fix: `make clean check D3E=<checkout of the previous commit>`. The Makefile
reads RRFLibraries from next to this repository, `../../../RRFLibraries` (override with `RRFLIBS=`). The `*-test` resource filter in
`.project` hides this folder from Eclipse.
