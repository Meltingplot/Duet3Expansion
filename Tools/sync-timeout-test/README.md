# Sync loss timeout host test

This test checks that `CanInterface::UpdateSyncLockState` (`src/CAN/CanInterface.cpp`) does not switch the heaters off when a sync
message from the main board arrives while it checks the sync loss timeout.

While the board resyncs after a loss of time sync, `Platform::Spin` in the MAIN task (priority 1) reports it as not synced
(`src/Platform/Platform.cpp:1130`). `UpdateSyncLockState` then switches all heaters off once the last sync message is older than the
connection timeout. The time of the last sync message is written by `StepTimer::ProcessTimeSyncMessage` (`src/Movement/StepTimer.cpp:256`)
in the CanClock task (priority 5).
- The check read `millis()` before `StepTimer::GetWhenLastSynced()`. A sync message processed between the two stores a time newer
  than `now`, `now - whenLastSynced` wraps to about 2^32, and all heaters go off although sync messages are arriving.
- `M122 B<n>` then shows a timeout shutdown without a main board timeout (`CanInterface.cpp:719`).
- `StepTimer::CheckSynced` reads the same stamp in the right order (`StepTimer.cpp:175`, "capture whenLastSynced before we call
  millis in case we get interrupted"), and the fix copies it.

Found by the shared-data census of 2026-11-23 (F2), which read the order "clock call, then load" in the TOOL1LC ELF (`bl millis` at
0x51e0, `ldr` at 0x51e4, `SwitchOffAll` at 0x51f2). How often the window is hit on a board was not measured.

## What the test compiles

The Makefile copies `CanInterface::UpdateSyncLockState` out of `src/CAN/CanInterface.cpp` at build time (`extract.awk` from
RepRapFirmware's host tests, with `#line` directives). `sync_timeout_test.cpp` gives it its file-static state
(`CanInterface.cpp:156-169`), `StepTimer::GetWhenLastSynced`, `Heat::SwitchOffAll` and a `millis()` that runs a writer, one tick
later, right after it has read the clock.

Cases, with sync lost and a connection timeout of 1000 ms:
- last sync message 100 ms ago: heaters stay on;
- 1500 ms ago: heaters off once;
- 100 ms ago, and a newer one during the check: heaters stay on.

Each case also checks that the sync loss is counted once. Without the fix the heaters go off in the third case.

The firmware builds do not use this folder: make and CMake only read `src/`, and the `*-test` resource filter in `.project` hides it
from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/sync-timeout-test and build/asan-sync-timeout-test (AddressSanitizer + UBSan), then run both

To see the failure, run it against the source without the fix: `make clean check D3E=<checkout of the previous commit>`.
