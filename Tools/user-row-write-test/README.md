# User row write host test (CoreN2G, SAME5x and SAMC21)

This test checks that `_user_row_write_exec` in CoreN2G keeps interrupts off while it fills the NVM page buffer and issues the write
command. `_user_area_write` calls `_user_row_write_exec` to rewrite the NVM user row. The test covers both HPL files:
`src/SAME5x_C21/SAME5x/hpl/nvmctrl/hpl_nvmctrl.c` (EXP3HC, EXP1HCL, M23CL and the other SAME5x boards of `Boards.cmake:24`) and
`src/SAME5x_C21/SAMC21/hpl/nvmctrl/hpl_nvmctrl.c` (EXP1XD, TOOL1LC, SAMMYC21, SZP, `Boards.cmake:38`).

## Why

Upstream disabled interrupts across the page buffer fill and the write command in `_flash_program`:
- SAME5x: CoreN2G 4b7ab3f (C. Hammacher, 2026-06-30). The comment says that "an interrupt landing in it (the USB stack stays live
  throughout an IAP update) corrupts a single word of the page".
- SAMC21: cdd9ba1 (2026-11-13), "as on the SAME5x".

`_user_row_write_exec` fills the same page buffer through the user row address `NVMCTRL_USER`:
- SAME5x: per quad word, a page buffer clear, 4 word writes and a WQW command.
- SAMC21: per page, 16 word writes and a WAP command.

It did this with interrupts on. The fix wraps each fill and its write command in the same `__get_PRIMASK` / `__disable_irq` /
`__set_PRIMASK` as `_flash_program`. The wait for READY after the command stays outside, as there.

The user row holds the fuses, for example the bootloader protection that the firmware sets up at start
(Duet3Expansion `src/Platform/Tasks.cpp:236-256`). Duet3Expansion also rewrites it at run time:
- the CAN address and timing, after a CAN message that sets them (`src/CAN/CanInterface.cpp:842`);
- the bootloader update flag (`src/Platform/Platform.cpp:553-558`).

Found by the flash census of 2026-11-23 (F6). Upstream code. The effect on a board, a corrupted word in the user row, has not been
seen. The mechanism, how an interrupt corrupts a word of the fill, is the one upstream's comment states; it is not in what this test
rests on.

## What the test compiles

The Makefile copies `_user_row_write_exec` out of both HPL files at build time, with `extract.awk` (from the RepRapFirmware host
tests, with `#line` directives). It compiles `user_row_write_test.cpp` once for each processor (`MODEL_SAME5X`, `MODEL_SAMC21`).

The model:
- `NVMCTRL_USER` points at a page buffer.
- The `hri_nvmctrl_*` accessors, the DSU security bit and PRIMASK are stand-ins with the constants of the DFP headers.
- The NVMCTRL stays busy for 3 READY polls after each command. Erase, page buffer clear, WQW and WAP act on a user row array.

Each call of the code into the model is an event. Between two events the code runs plain instructions only. So if the page buffer
changed between two events while PRIMASK was 0, an interrupt could have landed in the fill. The test also checks that the page buffer
clear and the write commands are issued with interrupts off, as in `_flash_program`.

There are three cases per processor:
- called with interrupts on;
- called with interrupts off, which must leave them off;
- security bit set, which must refuse the write.

Each case also checks the user row contents afterwards and that PRIMASK is back at its value on entry.

Without the fix, the case with interrupts on fails on both processors: 32 fills on the SAME5x, 4 on the SAMC21. The other cases pass.

Not covered:
- `_flash_program`, `_flash_erase_row` and the other HPL paths;
- what an interrupt in the fill does to the NVMCTRL;
- the time with interrupts off. On the SAME5x it includes the wait for the page buffer clear.

The Makefile reads CoreN2G from next to this repository, `../../../CoreN2G` (override with `COREN2G=`). The firmware builds do not
use this folder: make and CMake only read `src/`, and the `*-test` resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build the plain and the ASan/UBSan program for each processor, then run all four

To see the failure, run it against CoreN2G without the fix: `make clean check COREN2G=<checkout of the previous commit>`. The
generated files depend on the source files, not on `COREN2G=`, so `make clean` comes first.

Each run prints `PASS` or `FAIL` with the reason. The exit code is 1 if any run fails.
