# Permanent allocation lock host test

This test runs the real `Tasks::AllocPermanent`, `GetMallocMutex` and `ReleaseMallocMutex` from `src/Platform/Tasks.cpp`, and
the real `_sbrk` and `CoreAllocPermanent` from CoreN2G's `src/syscalls.h`, on a PC. The Makefile copies their text out of the
sources at build time, because the files as a whole need the hardware headers. `alloc_permanent_lock_test.cpp` plays the malloc
mutex, the scheduler state and the interrupt mask.

## What is tested and why

Duet3Expansion's `malloc` is `src/libc/nano-mallocr.c`, whose lock is `GetMallocMutex` (`nano-mallocr.c:138-142`,
`Tasks.cpp:139-154`). It grows the heap with `_sbrk`, which raises `heapTop` up to `heapLimit` (`syscalls.h:49-64`).
`Tasks::AllocPermanent` takes memory with `CoreAllocPermanent`, which lowers `heapLimit` down to `heapTop` (`syscalls.h:66-78`,
"take the malloc mutex before calling this"). `MoveSegment` (`src/Movement/MoveSegment.h:83-84`) and `CanMessageBuffer`
(`MessageBufferAlloc`, `Tasks.cpp:1234-1237`) use it.

Up to 3.7-dev 9ac5016c, `AllocPermanent` called `__malloc_lock` and `__malloc_unlock`. In the RP2040 builds these are the
functions at `Tasks.cpp:139` and `:157`. In all other builds they are newlib's, whose `__retarget_lock_acquire_recursive` is
empty: it is `bx lr` in the TOOL1LC, EXP1HCL and SZP ELF files. So `AllocPermanent` took no lock on these boards. A task that took
permanent memory while another task was inside `malloc` read and wrote `heapTop` and `heapLimit` at the same time as the other
task's `_sbrk`. Both could then hand out the same memory. The call came with e539a216 (2023-01-18), which named the mutex function
`__malloc_lock` in all builds; 6d52ab31 (2023-03-31) named it `GetMallocMutex` again outside the RP2040 builds and left
`AllocPermanent` as it was. The fix takes the malloc mutex, as RepRapFirmware's
`Tasks::AllocPermanent` does.

The cases:
- With the scheduler running, `AllocPermanent` takes the malloc mutex once, lowers the heap limit while it holds it and releases
  it.
- While another task holds the mutex inside `malloc`, `AllocPermanent` waits until that task's `_sbrk` is done, and the two
  allocations do not overlap.
- While the scheduler is suspended or not started, `AllocPermanent` takes nothing, as `malloc` does. This case passes without the
  fix too.
- With PRIMASK or BASEPRI set and the scheduler running, `AllocPermanent` asserts, as `malloc` does since e847c690. This is the
  behaviour change of the fix. The test compiles the SAME51 form of the assertion; the SAMC21 form tests PRIMASK only.
- `threads`: one thread runs what `malloc` does around `_sbrk` (`GetMallocMutex`, `_sbrk`, `ReleaseMallocMutex`), and the other
  runs `AllocPermanent`, each 20000 times. ThreadSanitizer reports a data race on `heapTop` or `heapLimit` if they do not share
  the lock.

## Running

    make check                           # plain and ASan run the cases on one thread, TSan the case with two threads
    make check D3E=/path/to/D3E          # against another Duet3Expansion checkout, e.g. one without the fix
    make check COREN2G=/path/to/CoreN2G  # against another CoreN2G, default the one next to Duet3Expansion

ThreadSanitizer of g++ 13 stops with "unexpected memory mapping" on newer kernels, so the Makefile runs it under `setarch -R`.

Results at 3.7-dev 9ac5016c with CoreN2G 3.7-dev 7d41892 (g++ 13.3, WSL):
- Without the fix: plain and ASan "1 passed, 3 failed". TSan exits with 66: "data race" between `CoreAllocPermanent`
  (`syscalls.h:72`) and `_sbrk` (`syscalls.h:55`) on `heapTop`.
- With the fix: plain and ASan "4 passed, 0 failed". TSan passes.
