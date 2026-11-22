# Raw pointer check

This check fails when Duet3Expansion declares a new raw pointer that can be reseated, unless `allowlist.txt` names it.

`pointer-check.py` is a copy of `Tools/pointer-check/pointer-check.py` of the Meltingplot RepRapFirmware fork
(Meltingplot/RepRapFirmware#52). The copy differs only in the folders that `VENDORED` names and in the name of the project in
its texts, so that the keys and the messages read the same in both repositories. A fix to the script belongs in both copies.

## Why

Other tasks read many pointers without a lock. When one task deletes an object while a pointer that another task reads still
names it, the reader follows a pointer to freed memory. The RepRapFirmware fork fixed two such cases:
- `StringHandle::Delete` released the slot before it cleared the handle (Meltingplot/RepRapFirmware#50).
- `Move::SetKinematics` deleted the old kinematics before it pointed `kinematics` at the new one
  (Meltingplot/RepRapFirmware#51).

Duet3Expansion runs several FreeRTOS tasks as well, for example MAIN (`src/Platform/Tasks.cpp`), HEAT (`src/Heating/Heat.cpp`)
and CanRecv (`src/CAN/CanInterface.cpp`).

Neither order is visible in the declaration `T *p`. The aim is that every pointer that another task can read states how it is
shared, through a pointer class instead of a raw pointer. This check finds the raw pointers. The pointer classes do not exist
yet, so for now every existing pointer is in `allowlist.txt`. Its entries have no census yet: the comment of each is its type.

## What counts

The check counts every class member, static member and variable outside a function whose type is a pointer or an array of
pointers, and whose pointer is not const:
- `T *p`, `const T *p`, `T *volatile p`, function pointers and typedefs of pointers (`TaskHandle`) count.
- `T *const p` and references do not count.
- Declarations inside functions do not count: locals, static locals, local classes and the members of lambda closures.
- Only code under `src/` counts. The libraries next to Duet3Expansion (CANlib, CoreN2G, FreeRTOS, RRFLibraries,
  Qfplib-M0-full) do not.
- Code under `src/` that comes from elsewhere does not count: `src/libc`, which holds newlib's allocator (`nano-mallocr.c`)
  and the replacement for newlib's `errno.c`. `VENDORED` in `pointer-check.py` lists these folders. Without it the check would
  report the allocator's `__malloc_free_list`, `__malloc_sbrk_start` and `malloc_chunk::next`; `errno.c` declares no pointer.

`selftest/src/sample.cpp` has one example of each case. `make selftest` compiles it with `-O3`, as the board makefiles do, for
each core of the boards: Cortex-M0+ (SAMC21), Cortex-M4 (SAME51) and Cortex-M33 (STM32H5). `selftest/src/no-pointers.cpp` has
none: `make selftest` runs `--update` on it, which must write an allowlist with the header and no entries.

## How it works

The check reads the declarations from the DWARF debug information that the compiler writes with `-g`. It uses
`arm-none-eabi-readelf` of the Arm GNU toolchain and python3 and needs no other compiler. It therefore sees what the firmware
build compiled for each board, including the files that only the firmware compiler accepts.

`make check` first rebuilds each board with `DEBUG_FLAGS=-g`. `-B` rebuilds every object, so no object is left over from a
build without `-g`: at 3a8e565f each of the 86 objects of TOOL1LC, 86 of EXP1HCL and 91 of SZP had debug information
afterwards. The top-level Makefile builds the libraries in sub-makes, which get `-B` and `DEBUG_FLAGS` too. `DEBUG=1` would not
do, because it also sets `-Og -DDEBUG`. `-g` adds the debug sections and leaves the code alone. For TOOL1LC, EXP1HCL and SZP at
3a8e565f, the `.bin` with and without `-g` differed only in:
- the build time (`TimeSuffix` in `src/Version.cpp`);
- the CRC at the end;
- the order of the veneers, the stubs through which code in flash calls functions in RAM and back, and the calls to them.

Apart from the time, every function disassembled the same in both builds once its calls were named by symbol.

The build time can move more than its own bytes. The linker merges a string literal into the end of a longer string, and when
the time ends in `3` it can hold the `"3"` of `src/Heating/Sensors/RtdSensor31865.cpp:121`. EXP1HCL built at 20:48:03 was 8
bytes shorter than at 20:47:42, without `-g` as with it. When two builds differ in length, compare their build times first.

What the check does not see:
- A class template that is never instantiated, and a type that no compiled code uses: the compiler writes no debug information
  for them. They are not in the firmware either. The check sees them as soon as code uses them.
- A source file of which the linker keeps no section, e.g. a file that the board configuration leaves empty with `#if`: the
  linker (`--gc-sections`) drops its debug information with its code. At 3a8e565f this applied to 32 of the 86 objects of
  TOOL1LC, 13 of the 86 of EXP1HCL and 49 of the 91 of SZP. Their code is not in the firmware either.
- Boards that it does not build. The default is those of the release workflow (`TOOL1LC`, `EXP1HCL`, `SZP`); the other boards
  of the top-level Makefile are not included.

## Use

The Makefile needs the Arm GNU toolchain (`CROSS_COMPILE`, default `arm-none-eabi-`) and python3. The firmware build
container of DuetSoftwareFramework has both: `firmware/build.sh shell`, then `make -C Duet3Expansion/Tools/pointer-check check`.

    make check                      # build the boards with -g, then check them
    make check CONFIGS=TOOL1LC      # only this board; entries of the others show up as notes
    make scan                       # check the ELF files that are already there, without building
    make update                     # build, then rewrite allowlist.txt from the firmware
    make selftest                   # check pointer-check.py against selftest/src/sample.cpp and no-pointers.cpp

Each new pointer gives one line, in the format that compilers use:

    src/Heating/Heat.cpp:46: error: new raw pointer that can be reseated: Heating/Heat.cpp: heaterTask (Task *). ...

The exit code is 1 if there is a new pointer.

## A new pointer

Use a pointer class that states how the pointer is shared. Until Duet3Expansion has such classes, add the key from the error
message to `allowlist.txt` with the reason why the pointer is safe, e.g. that only one task reads and writes it. A static member
is reported at its definition in the `.cpp` file, not at the declaration in the class. When a pointer goes away, the check
prints a note, and `make update` removes it from the list.

The Meltingplot release workflow (`.github/workflows/mp-release.yml`) runs `make check` for the boards that it releases
(`BOARDS`) before it uploads the binaries. A new pointer stops the release.

The firmware builds do not use this folder: make and CMake only read `src/`, and a resource filter in `.project` hides it from
Eclipse.
