# Heap use inside protected regions check

This check fails when Duet3Expansion can reach the heap from a new place inside a protected region, unless `allowlist.txt` names
it. With `--unmask` it fails when a new place lifts the mask of a region that masks interrupts too early, unless
`unmask-allowlist.txt` names it (see below). A protected region is code that runs
- with the scheduler suspended (`vTaskSuspendAll` to `xTaskResumeAll`),
- inside a FreeRTOS critical section (`vPortEnterCritical` to `vPortExitCritical`),
- with BASEPRI raised (`msr BASEPRI_MAX`, only on the SAME51; the Cortex-M0+ of the SAMC21 has no BASEPRI),
- with interrupts disabled (`cpsid i`, `msr PRIMASK`),
- or in an exception handler.

`alloc-check.py` is a copy of `Tools/alloc-check/alloc-check.py` of the Meltingplot RepRapFirmware fork (branch
`3.7-dev`). The copy differs only in the libraries that `LIBRARIES` names and in the name of the project in its
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

**Calls through a register** are findings of their own (target `indirect call`); `census.md` names their targets. The veneers
of the linker (`__*_veneer`), through which the SAMC21 boards call their step functions in RAM, are followed to the function at
the address in their literal, also for a C++ function, whose veneer has the mangled name.

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

## Interrupts unmasked inside a region (`--unmask`)

`make scan-unmask` runs the same analysis for a second error class: code that lifts the mask of a region that masks interrupts
before the region ends. `unmask-allowlist.txt` names the findings that may stay, and `unmask-census.md` has the reasoning.

**Why.** A BASEPRI or PRIMASK region keeps an interrupt out while the code works on data that the interrupt uses too, for
example the step interrupt. If the mask is lifted early, the interrupt runs while the region still relies on it. FreeRTOS lifts
masks in places that are easy to miss:
- On the SAME51 (Cortex-M4F port, FreeRTOS `Makefiles/SAME51.mk:7`) every critical section in task context ends with BASEPRI 0
  when its nesting count drops to 0 (`vPortExitCritical`, `ARM_CM4F/port.c:492-500`). Task notifications, mutexes, queues and
  `xTaskResumeAll` (`tasks.c:4038-4162`, so the end of every `TaskCriticalSectionLocker`) work inside one. Inside a
  `ChangeBasePriority` region each of them sets BASEPRI to 0. PRIMASK stays as it was.
- On the SAMC21 (Cortex-M0 port, `Makefiles/SAMC21.mk:7`) a critical section disables the interrupts, and its end at nesting 0
  runs `cpsie i` (`ARM_CM0/port.c:790-812`). Inside `IrqSave` or an `AtomicCriticalSectionLocker` each of the calls above
  enables the interrupts.
- `IrqEnable` is an unconditional `cpsie i` (CoreN2G `src/Core.h:378-381`). A function that pairs `IrqDisable` with `IrqEnable`
  enables the interrupts for a caller that had disabled them.

**What counts.** The check follows the region state of each function as for the heap and reports:
- a call with PRIMASK set to a function that reaches a clear of PRIMASK (`cpsie i`, or `msr PRIMASK` with 0);
- a call with BASEPRI raised to a function that reaches a write of BASEPRI 0;
- a call inside the function's own BASEPRI or PRIMASK region, when it reaches `vPortExitCritical` while the function has no
  critical section of its own open and critical sections use that mask; and the end of the function's own outermost critical
  section inside such a region;
- inside a critical section of the function (`critical`), a call that reaches a clear of the mask that critical sections use;
- a clear in the function itself while one of its regions is still open around it (nested regions), with the function as target;
- a call through a register inside such a region, and a call to a function that can reach one.

Which mask a critical section uses comes from what `vPortEnterCritical` leaves raised: BASEPRI on the Cortex-M4F, PRIMASK on the
Cortex-M0+. `vPortExitCritical` is known by name, because the check cannot see the value that it writes. The chains pass
through `xTaskResumeAll`. An exception handler is not a region of its own here: an interrupt at the handler's priority or below
stays out whatever BASEPRI and PRIMASK are, so only the regions that a handler opens count.

**Key.** `Opener [kind] -> target` as above, with the kinds `basepri`, `primask` and `critical`. The target is the function that
holds the instruction that lifts the mask (`vPortExitCritical`, ...), the opener itself for a nested clear, or `indirect call`. A
call that reaches both gives only the key of the function. The message is `interrupts unmasked inside a protected region` or
`call through a register inside a region that masks interrupts`.

**What it does not see**, in addition to the list above:
- **The putc of `debugPrintf`.** `CanInterface::DebugPutc` wakes the CAN sender for the end of each message
  (`CAN/CanInterface.cpp:962-974`). It is reached through a register (a `function_ref`), so a print inside a region is a call
  through a register and not a FreeRTOS call. B1 and B2 of `census.md` are found this way.
- **A critical section that a caller holds.** A FreeRTOS call then leaves the mask in place, but the check reports it.
- **When the mask is lifted.** A call that lifts it as the last thing in the region is found like one at its start.

Better than an allowlist entry: make the FreeRTOS call after the region.

## Use

The Makefile needs the Arm GNU toolchain (`CROSS_COMPILE`, default `arm-none-eabi-`: `objdump`, `nm`, `addr2line`, and `g++` for
the selftest) and python3. The firmware build container of DuetSoftwareFramework has both. The ELF files are those that
`Tools/pointer-check` builds with `-g`.

    make check                      # build the boards with -g (Tools/pointer-check), then run both checks
    make check CONFIGS=TOOL1LC      # only this board
    make scan                       # the heap check on the ELF files that are already there, without building
    make scan-unmask                # the unmask check on the same ELF files
    make update                     # rewrite allowlist.txt and unmask-allowlist.txt from the ELF files that are there
    make selftest                   # check alloc-check.py against selftest/src/sample.cpp and no-alloc.cpp

`make selftest` compiles `selftest/src/sample.cpp` with `-O3` for the Cortex-M0+ and the Cortex-M4 and compares the findings with
`selftest/expected.txt` (without the `[basepri]` case on the Cortex-M0+). `SwitchInRegion` and `FarJumpInRegion` cover the
Thumb-1 jump tables and the far `bl`; without them the Cortex-M0+ run reports `SwitchInRegion` as a call through a register and
misses `FarJumpInRegion`. The unmask cases have a list per core, `selftest/expected-unmask-m0plus.txt` and
`selftest/expected-unmask-m4.txt`: the runtime stand-ins model the Cortex-M0 port on the Cortex-M0+ and the Cortex-M4F port on
the Cortex-M4, so a task notification with PRIMASK set is found only on the Cortex-M0+. `no-alloc.cpp` must give no finding of
either check.

The Meltingplot release workflow (`.github/workflows/mp-release.yml`) runs `make selftest` and `make scan` right after the raw
pointer check, on the ELF files it has built, and `make scan-unmask` as a step of its own after them. A new finding stops the
release. The firmware builds do not use this folder.

## A new finding

Allocate before the region and free after it, as `MoveSegment::PrimeFreeList` does. If that is not possible, add the key from the
error message to `allowlist.txt` with the reason why the heap is not reached there.
