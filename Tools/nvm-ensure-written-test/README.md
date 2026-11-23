# NonVolatileMemory::EnsureWritten host test (SAMC21)

This test checks that `NonVolatileMemory::EnsureWritten` (`src/Hardware/NonVolatileMemory.cpp`) returns on the SAMC21 (TOOL1LC, SZP)
only after the write of the last page has finished.

`SoftwareReset` saves the software reset data with `EnsureWritten` and resets the processor straight after
(`src/Hardware/ExceptionHandlers.cpp:30-35`).
- On the SAMC21, `EnsureWritten` writes its 512 bytes (`NonVolatileMemory.h`, `static_assert(sizeof(NVM) == 512)`) to the RWW EEPROM
  area with `Flash::RwwWrite`.
- `Flash::RwwWrite` is CoreN2G `hpl_RwwWrite`, which writes them as 8 pages of 64 bytes, 2 rows of 4 pages
  (`src/SAME5x_C21/SAMC21/hpl/nvmctrl/hpl_nvmctrl.c`, `src/atmel/SAMC21_DFP/1.2.176/samc21/include/instance/nvmctrl.h:73-78`).
- `_flash_program` waits for NVMCTRL READY before it issues each command and returns once it has issued it. So the reset is
  requested while the last page is still being programmed.
- The SAMC21 datasheet says what a reset does to a page write in progress. That is not what this test rests on.
- The paths that already work wait after the write before they return or reset:
  - the SAME5x branch of the same function waits for the SmartEEPROM after the copy (`NonVolatileMemory.cpp:83`);
  - the user row writes that are followed by a reset wait 10 ms first: the fuse setup (`src/Platform/Tasks.cpp:252-256`) and the
    bootloader update flag (`src/Platform/Platform.cpp:553-558`), "If we reset immediately then the user area write doesn't complete".

The fix waits for READY after the write, as the SAME5x branch does. The other callers of `EnsureWritten` (thermistor and inductive
heater calibration, the closed-loop encoders) now also return once the page is written.

Found by the flash census of 2026-11-23 (F4). The SAMC21 branch is upstream code (D. Crocker, c5aa17d2d 2020-09-12, a5f391bb1
2021-09-13). The effect on a board (missing or garbled reset data in M122 after a tool board crash) has not been seen.

## What the test compiles

The Makefile copies `NonVolatileMemory::EnsureWritten` out of `src/Hardware/NonVolatileMemory.cpp` at build time. It uses `extract.awk`
from the RepRapFirmware host tests, with `#line` directives, and compiles the function with `SAMC21` set.

`nvm_ensure_written_test.cpp` gives it stand-ins:
- its class;
- `Flash::RwwErase` and `Flash::RwwWrite`, which wait for READY before each command, as the HPL does;
- an NVMCTRL that stays busy for 5 reads of INTFLAG after each command.

There are three cases:
- nothing to write;
- a write: a free reset data slot, where `AllocateResetDataSlot` sets the page dirty without an erase;
- an erase and a write: all slots used.

Each case checks that the state is clean, the number of commands (0, 8, 10), and that the NVMCTRL is ready when `EnsureWritten`
returns. Without the fix, the two cases with a write fail.

Not covered:
- the SAME5x, SAME70 and other branches of `EnsureWritten`;
- what the NVMCTRL does on a reset during a page write.

The firmware builds do not use this folder: make and CMake only read `src/`, and the `*-test` resource filter in `.project` hides it
from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/nvm-ensure-written-test and build/asan-nvm-ensure-written-test (AddressSanitizer + UBSan), then run both

To see the failure, run it against the source without the fix: `make clean check D3E=<checkout of the previous commit>`. The generated
file depends on the source file, not on `D3E=`, so `make clean` comes first.

Each run prints `PASS` or `FAIL` with the reason. The exit code is 1 if any run fails.
