# Heap use inside protected regions check

This check fails when Duet3Expansion can reach the heap from a new place inside a protected region, unless `allowlist.txt` names
it. A protected region is code that runs
- with the scheduler suspended (`vTaskSuspendAll` to `xTaskResumeAll`),
- inside a FreeRTOS critical section (`vPortEnterCritical` to `vPortExitCritical`),
- with BASEPRI raised (`msr BASEPRI_MAX`, only on the SAME51; the Cortex-M0+ of the SAMC21 has no BASEPRI),
- with interrupts disabled (`cpsid i`, `msr PRIMASK`),
- or in an exception handler.

`alloc-check.py` is a copy of `Tools/alloc-check/alloc-check.py` of the Meltingplot RepRapFirmware fork (branch
`feature/alloc-check`). The copy differs only in the libraries that `LIBRARIES` names and in the name of the project in its
texts, so that the keys and the messages read the same in both repositories. A fix to the script belongs in both copies.

## Why

Duet3Expansion's heap is newlib's nano-malloc in `src/libc/nano-mallocr.c`, whose lock is `MALLOC_LOCK`, that is
`GetMallocMutex` (`nano-mallocr.c:138-142`). `GetMallocMutex` (`src/Platform/Tasks.cpp:139-154`) takes the malloc mutex only
while the scheduler is running. With the scheduler suspended, or before it starts, it takes nothing. This gives two failures:
- **The mutex is skipped.** Code that allocates or frees with the scheduler suspended runs `malloc` without the mutex. If a task
  of the same or a lower priority was preempted inside `malloc`, both work on the free list or the heap top at once. The task
  priorities are in `src/Platform/TaskPriorities.h:14-34`: main 1, heat 2, Move and CAN receiver 3, CAN sender 5.
- **The guard is lifted.** If the scheduler is running, taking the mutex can switch tasks, and a switch restores the PRIMASK (and
  on the SAME51 the BASEPRI) of the next task. Upstream added the assertion at `Tasks.cpp:146-151` for this (e847c690).

In an exception handler the heap is never safe: the interrupted task may be inside `malloc`, and an ISR cannot take a mutex.

`Tasks::AllocPermanent` (`Tasks.cpp:1141-1147`) takes memory from the top of the heap with CoreN2G's `CoreAllocPermanent`, which
needs the same mutex (CoreN2G `src/syscalls.h:66-78`). Up to 9ac5016c it called newlib's `__malloc_lock`, which takes nothing in
these builds (see `census.md`, P1). The branch `fix/alloc-permanent-lock` makes it take the malloc mutex.

### History of the class

- RRFLibraries 2422ef6 (D. Crocker, 2024-10-08, "Fixed call to ::operator new inside a critical section"): the freelist
  `AllocateItem` called `::operator new` inside its own `TaskCriticalSectionLocker`. On the same day Duet3Expansion 0a1c2b65
  ("Shortened an excessively long critical section") cut down a region in `ClosedLoop.cpp`.
- Duet3Expansion e539a216 (D. Crocker, 2023-01-18, "Fixes for RP2040 memory allocation") renamed `GetMallocMutex` to
  `__malloc_lock` and made `Tasks::AllocPermanent` call `__malloc_lock`. 6d52ab31 (2023-03-31, "Updated to gcc version 12.2")
  brought back `GetMallocMutex` for all builds but the RP2040 one and left `Tasks::AllocPermanent` calling `__malloc_lock`.
- Duet3Expansion 3b5c6506 (2026-08-09, "Fix FreeRTOS assertion when the tick ISR detects a stuck spin loop"): the tick ISR
  switched the heaters off through a `ReadWriteLock`; it now calls `Heat::SwitchOffAllLocalFromISR`.
- Duet3Expansion e847c690 (C. Hammacher, 2026-08-14, "Fixed step interrupt guard being lifted by memory allocation"):
  `MoveSegment::PrimeFreeList` before the region of `Move::AddLinearSegments`, and the assertion in `GetMallocMutex`.
- This fork (2026-11-22): K23, `ReadWriteLock` allocating its lock records with the scheduler suspended (RRFLibraries branch
  `fix/alloc-outside-critical-section`), and P1 above.

## What counts

The regions, the targets, the key format and the limits are those of the RepRapFirmware copy; its README has the details. In
short:

**Regions.** `vTaskSuspendAll`/`xTaskResumeAll` (`TaskCriticalSectionLocker` and the other inline lockers of RRFLibraries
`RTOSIface.h` leave these calls), `vPortEnterCritical`/`vPortExitCritical` (on the Cortex-M0+ these disable interrupts with
`cpsid i`, FreeRTOS `ARM_CM0/portmacro.h:308-311`), `msr BASEPRI_MAX`/`msr BASEPRI` (`ChangeBasePriority`, `RestoreBasePriority`
and `BasePriorityBooster` in CoreN2G `src/CoreIO.h:374-414`, not on the SAMC21), `cpsid i` and `msr PRIMASK` (`IrqDisable`,
`IrqSave`, `IrqRestore`, `AtomicCriticalSectionLocker`), and every entry of `exception_table` after the stack pointer and the reset
handler.

The check follows the control flow of each function, with the jump tables of the two cores: `tbb` and `tbh` on the Cortex-M4,
and on the Cortex-M0+ the tables of addresses that `mov pc` jumps through and the jumps with `bl` inside a function (Thumb-1 has
no long branch). The number of table entries comes from the bounds check before the jump.

**Targets.** The functions of `nano-mallocr.c`, `_sbrk` and `CoreAllocPermanent`, `GetMallocMutex`/`ReleaseMallocMutex`, newlib's
`__malloc_lock`/`__malloc_unlock`, and the allocation functions `operator new` and `delete`, `Tasks::AllocPermanent` and
`MessageBufferAlloc` (through which CANlib's `CanMessageBuffer` allocates; in EXP1HCL with the fix the compiler inlines all of
`Tasks::AllocPermanent` into it).

**Calls through a register** are findings of their own (target `indirect call`); `census.md` names their targets.

**Key format:** `Opener [kind] -> target`, with the file for a static function. Each new finding gives one line in the format of
compiler errors.

## What the check does not see

- The targets of calls through a register, which `census.md` reads by hand. A new target of an existing call does not make a new
  finding.
- Correlated conditions: a call on a path that cannot run inside the region is still found.
- The run-time context of a function that does not open the region itself.
- An allocation that the compiler inlines into a function of another name together with the mutex functions.
- Boards that it does not build: the default is those of the release workflow (TOOL1LC, EXP1HCL, SZP), the same as
  `Tools/pointer-check`.
- Blocking calls inside a region: `census.md` lists those that the census found (B1-B3).

## Use

The Makefile needs the Arm GNU toolchain (`CROSS_COMPILE`, default `arm-none-eabi-`: `objdump`, `nm`, `addr2line`, and `g++` for
the selftest) and python3. The firmware build container of DuetSoftwareFramework has both. The ELF files are those that
`Tools/pointer-check` builds with `-g`.

    make check                      # build the boards with -g (Tools/pointer-check), then check them
    make check CONFIGS=TOOL1LC      # only this board
    make scan                       # check the ELF files that are already there, without building
    make update                     # rewrite allowlist.txt from the ELF files that are there
    make selftest                   # check alloc-check.py against selftest/src/sample.cpp and no-alloc.cpp

`make selftest` compiles `selftest/src/sample.cpp` with `-O3` for the Cortex-M0+ and the Cortex-M4 and compares the findings with
`selftest/expected.txt` (without the `[basepri]` case on the Cortex-M0+). `SwitchInRegion` and `FarJumpInRegion` cover the
Thumb-1 jump tables and the far `bl`; without them the Cortex-M0+ run reports `SwitchInRegion` as a call through a register and
misses `FarJumpInRegion`.

The Meltingplot release workflow (`.github/workflows/mp-release.yml`) runs `make selftest` and `make scan` right after the raw
pointer check, on the ELF files it has built. A new finding stops the release. The firmware builds do not use this folder.

## A new finding

Allocate before the region and free after it, as `MoveSegment::PrimeFreeList` does. If that is not possible, add the key from the
error message to `allowlist.txt` with the reason why the heap is not reached there.
