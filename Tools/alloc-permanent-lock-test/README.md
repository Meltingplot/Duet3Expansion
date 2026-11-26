# Permanent allocation lock host test

This test runs the real `Tasks::AllocPermanent`, `GetMallocMutex` and `ReleaseMallocMutex` from `src/Platform/Tasks.cpp`, and
the real `_sbrk` and `CoreAllocPermanent` from CoreN2G's `src/syscalls.h`, on a PC. The Makefile copies their text out of the
sources at build time, because the files as a whole need the hardware headers. `alloc_permanent_lock_test.cpp` plays the interrupt
mask, the malloc mutex and the scheduler state.

## What is tested and why

Duet3Expansion's `malloc` is `src/libc/nano-mallocr.c`, whose lock is `GetMallocMutex`. It grows the heap with `_sbrk`, which
raises `heapTop` up to `heapLimit`. `Tasks::AllocPermanent` takes memory with `CoreAllocPermanent`, which lowers `heapLimit` down
to `heapTop`. `MoveSegment` (`src/Movement/MoveSegment.h`) and `CanMessageBuffer` (`MessageBufferAlloc` in `Tasks.cpp`) use it.
So `_sbrk` and `CoreAllocPermanent` must not interleave.

Up to 3.7-dev 9ac5016c, `AllocPermanent` called `__malloc_lock` and `__malloc_unlock`. Outside the RP2040 builds these are
newlib's, whose `__retarget_lock_acquire_recursive` is empty: it is `bx lr` in the TOOL1LC, EXP1HCL and SZP ELF files. So
`AllocPermanent` took no lock on these boards. A task that took permanent memory while another task was inside `malloc` read and
wrote `heapTop` and `heapLimit` at the same time as the other task's `_sbrk`. Both could then hand out the same memory. The call
came with e539a216 (2023-01-18).

The fork fixed this in #20 by taking the malloc mutex in `AllocPermanent`. Upstream fixed it at its source: CoreN2G c9b23d7 does
the check and the update of `_sbrk` and of `CoreAllocPermanent` each inside an `AtomicCriticalSectionLocker`, and Duet3Expansion
0f63349f removes the lock from `AllocPermanent`. The fork took upstream's fix (CLAUDE.md rule 8), and this test checks it. The same
commit makes `GetMallocMutex` take the mutex whenever the scheduler has started. With the scheduler suspended, FreeRTOS then asserts
(`xQueueSemaphoreTake`, `src/queue.c:1682` in FreeRTOS d3ce7a3), so heap use with the scheduler suspended stops at an assertion
instead of running unprotected.

The cases:
- `AllocPermanent` lowers the heap limit inside the interrupt lock, once, and takes no malloc mutex.
- `_sbrk` raises the heap top inside the interrupt lock.
- `AllocPermanent` works while the scheduler is suspended or not started.
- `AllocPermanent` leaves the interrupt mask as it found it, masked or not.
- `GetMallocMutex` takes the mutex while the scheduler is suspended (the played mutex then records FreeRTOS's assertion), and takes
  nothing before the scheduler has started.
- `GetMallocMutex` asserts with PRIMASK or BASEPRI set and the scheduler running, since e847c690. The test compiles the SAME51
  form of the assertion; the SAMC21 form tests PRIMASK only.
- `threads`: one thread runs what `malloc` does around `_sbrk` (`GetMallocMutex`, `_sbrk`, `ReleaseMallocMutex`), and the other
  runs `AllocPermanent`, each 20000 times. The interrupt lock is played as one mutex for both threads, because nothing else runs
  on the single core while interrupts are disabled. ThreadSanitizer reports a data race on `heapTop` or `heapLimit` if `_sbrk` and
  `CoreAllocPermanent` do not both take it.

## Running

    make check                           # plain and ASan run the cases on one thread, TSan the case with two threads
    make check D3E=/path/to/D3E          # against another Duet3Expansion checkout
    make check COREN2G=/path/to/CoreN2G  # against another CoreN2G, default the one next to Duet3Expansion

Run it in WSL, not in the firmware build container: ThreadSanitizer of g++ 13 stops with "unexpected memory mapping" on newer
kernels, so the Makefile runs it under `setarch -R`, which the container does not permit.

Results on 2026-11-26 (g++ 13.3, WSL), Duet3Expansion with 0f63349f and CoreN2G with c9b23d7:
- plain and ASan "6 passed, 0 failed"; TSan passes.
- With `AtomicCriticalSectionLocker` removed from a copy of `syscalls.h`: TSan exits with 66, "data race".
