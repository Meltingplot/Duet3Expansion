# Census of the unmask allowlist

Each of the 8 entries of `unmask-allowlist.txt` was read in the code:
- for a call, every chain that its sites reach and the code of the region after the site;
- for a call through a register, every target that the firmware sets, and what that target reaches.

The verdict per entry is its comment in `unmask-allowlist.txt`; this file has the reasoning and the findings.

- **Basis:** 3.7-dev at 5c02bb99 (2026-11-22) with RRFLibraries 3.7-dev 888b479, CoreN2G 3.7-dev 7d41892 and FreeRTOS 3.7-dev
  980af10. Boards: TOOL1LC (3 entries), EXP1HCL (6) and SZP (1). TOOL1LC and SZP are SAMC21 (Cortex-M0+), EXP1HCL is SAME51
  (Cortex-M4). Line numbers refer to that state, except in a row that names a fix branch: there they refer to the tree of that
  branch.
- **Fixed:** the two SUSPECT entries (`census.md` B1 and B2) with fix/step-error-print-in-region: `LogStepError` writes its text
  into `Platform::genericDebugBuffer`, and `Platform::Spin` prints it. Their keys stay, now SAFE-NO-UNMASK.
- **Counts:** 0 SUSPECT, 2 SAFE-GUARDED, 6 SAFE-NO-UNMASK.
- **How:**
  - `alloc-check.py --unmask` lists each site with the shortest chain.
  - The targets of the calls through a register are those that `census.md` names for the heap check (`census.md:76-122`).
  - `--unmask --query` showed for each target whether it reaches a FreeRTOS call or `cpsie i`.
- **Origin:** every entry is upstream code. Duet3D 3.7-dev 02000a7c has the same regions and calls.

## Verdicts

| Verdict | Meaning |
|---|---|
| SUSPECT | The mask can be lifted while code that the region protects still runs. The fix branch is in brackets, or "open" |
| SAFE-GUARDED | The path to the instruction runs only under a run-time condition that the check does not evaluate, and the condition excludes it inside the region |
| SAFE-NO-UNMASK | A call through a register: no target that the firmware sets lifts the mask |

## Facts that several entries share

- **Ports.**
  - EXP1HCL builds FreeRTOS with the Cortex-M4F port (`Makefiles/EXP1HCL.mk:81`). Its critical section raises BASEPRI and leaves
    PRIMASK alone (FreeRTOS `ARM_CM4F/portmacro.h:213-225`, `port.c:475-490`). Its end writes BASEPRI 0 at nesting 0
    (`port.c:492-500`). A `FromISR` function restores the BASEPRI that it found (`portmacro.h:229-255`).
  - TOOL1LC and SZP use the Cortex-M0 port (`Makefiles/TOOL1LC.mk:83`, `SZP.mk:84`). Its critical section disables the
    interrupts, and its end at nesting 0 runs `cpsie i` (`ARM_CM0/port.c:790-812`). A `FromISR` function saves PRIMASK and
    restores it (`portasm.c:243-268`).
- **`debugPrintf`** (`src/RepRapFirmware.cpp:30-40`):
  - It writes through `vuprintf` with the putc `CanInterface::DebugPutc`, because `USE_SERIAL_DEBUG` is 0 in a release build of
    the three boards: EXP1HCL takes the default (`Config/BoardDef.h:106-107`), and TOOL1LC and SZP set it only for a `DEBUG`
    build (`Config/TOOL1LC.h:21`, `Config/SZP.h:21`).
  - `FormattedPrinter::Print` ends with `PutChar('\0')` (RRFLibraries `SafeVsnprintf.cpp:787`). For the 0, `DebugPutc` calls
    `WakeAsyncSender` (`CAN/CanInterface.cpp:962-974`). In a task that is `Give`, in an interrupt `GiveFromISR` (`:795-805`).
  - So every `debugPrintf` in a task ends with a FreeRTOS critical section. `--unmask --query CanInterface::DebugPutc` shows
    `xTaskGenericNotify -> vPortExitCritical` on all three boards.
- **Step error text** (since fix/step-error-print-in-region, line numbers of that branch):
  - `DriveMovement::LogStepError` (`DriveMovement.cpp:623-637`) formats with `StringRef::printf`, `MoveSegment::AppendDetails`
    (`MoveSegment.cpp:51-54`, `catf`) and `cat` into `Platform::genericDebugBuffer` (`Platform.cpp:110`), and sets
    `hasGenericDebug`. `Platform::Spin` prints it with `debugPrintf` in the main task (`Platform.cpp:990-995`, `Tasks.cpp:427`).
  - `StringRef::printf` and `catf` reach `FormattedPrinter` only through `SafeVsnprintf`, whose putc stores into the buffer
    (RRFLibraries `SafeVsnprintf.cpp:809-822`).
  - On TOOL1LC `StringRef::printf` and `catf` are calls through a register themselves: as variadic functions they return with
    `pop {r3}`, `add sp, #12`, `bx r3`.
  - A script listed, for every site of the step function keys below on TOOL1LC and EXP1HCL, what the site reaches: no function
    that lifts a mask, and no `FormattedPrinter` call other than through `SafeVsnprintf`. No exception handler reaches
    `debugPrintf` through direct calls any more.
- **Veneers.** On the SAMC21 boards the step functions run from RAM and are called through veneers (`census.md:96-99`). The check
  follows a veneer to the function at the address in its literal (`Program.resolve` in `alloc-check.py`). Behind them it sees on
  TOOL1LC two calls through a register that lift no mask:
  - the `mov pc` switch of `CalcNextStepTimeFull` (`DriveMovement.cpp:785`), which stays inside the function (`census.md:97-98`);
  - the Qfplib compare wrappers, which return with `bx r3` to the address that they copied from `lr` (Qfplib-M0-full
    `qfplib-m0-full_gcc.S:364-392`).

## Findings

| Entry | Boards | Verdict | Reason |
|---|---|---|---|
| `Move::AddLinearSegments [basepri] -> indirect call` | EXP1HCL | SAFE-NO-UNMASK (fixed in fix/step-error-print-in-region; census.md B1) | Region: `ChangeBasePriority(NvicPriorityStep)` (`Movement/Move.cpp:1306`) to `RestoreBasePriority` (`:1349`).<br>- The sites `ScheduleFirstSegment` (`:1327`) and `Interrupt()` (`:1341`) reach `LogStepError` through `CalcNextStepTimeFull`.<br>- Before the fix `LogStepError` printed with `debugPrintf` on every step error. The Move task runs the region (`Move.cpp:444`, `:477`), so the print woke the CAN sender with `Give`, whose critical section ends with BASEPRI 0. After that, `moveInstance->LogStepError` and, on the `Interrupt()` path, the rest of `Move::Interrupt` ran with the step interrupt enabled. That interrupt runs `Move::Interrupt` too (`TC0_Handler`, `census.md:96`).<br>- Now it writes the text into `Platform::genericDebugBuffer` (`DriveMovement.cpp:623-637`, see Step error text). The only calls through a register on the way are those of `FormattedPrinter` with the putc of `SafeVsnprintf`. |
| `Move::AddLinearSegments [primask] -> indirect call` | TOOL1LC | SAFE-NO-UNMASK (fixed in fix/step-error-print-in-region; census.md B2) | Region: `IrqSave` (`Move.cpp:1304`) to `IrqRestore` (`:1347`). The sites `ScheduleFirstSegment` (`:1327`) and `Interrupt()` (`:1341`) reach `DriveMovement::NewSegment`, `CalcNextStepTimeFull` and `LogStepError`, which run from RAM; the check follows their veneers. Before the fix the print in `LogStepError` reached `vPortExitCritical` as above. On the Cortex-M0 that runs `cpsie i`, so the rest of the region ran with interrupts enabled. Now the calls through a register on the way are the putc of `SafeVsnprintf` and the returns of `StringRef::printf` and `catf` (see Step error text), the `mov pc` switch (`DriveMovement.cpp:790`) and the Qfplib wrappers (see Veneers). None lifts a mask |
| `Move::AddLinearSegments [basepri] -> vPortExitCritical` | EXP1HCL | SAFE-GUARDED | `Split` (`Move.cpp:1176`, `MoveSegment.h:297-299`) calls `MoveSegment::Allocate`, which allocates only with an empty free list (`MoveSegment.cpp:16-30`). `PrimeFreeList` (`Move.cpp:1119`, `MoveSegment.h:160`) fills the free list right before the region (`:1121-1123`). The region splits at most one segment and then breaks (`:1191`), and only the Move task allocates segments (census.md, SAFE-PRIMED) |
| `Move::AddLinearSegments [primask] -> vPortExitCritical` | TOOL1LC | SAFE-GUARDED | As above |
| `ClosedLoop::InstanceControlLoop [primask] -> indirect call` | EXP1HCL | SAFE-NO-UNMASK | `GetCurrentMotion` holds an `AtomicCriticalSectionLocker` (`DriveMovement.h:286`) around `NewSegment` (`:310`, `:318`). `NewSegment` can reach `LogStepError` and its text. At the basis the putc was `DebugPutc`, and its wake-up of the CAN sender does not touch PRIMASK on the Cortex-M4F, in a task or in an interrupt (see Ports). Since fix/step-error-print-in-region it is the putc of `SafeVsnprintf` (see Step error text). The interrupts stay disabled. The TMC task runs it (`census.md:111-117`) |
| `Move::PhaseStepControlLoop [primask] -> indirect call` | EXP1HCL | SAFE-NO-UNMASK | As above, through `GetCurrentMotion` (`Move.cpp:2606`); TMC task |
| `Move::ProcessM970 [primask] -> indirect call` | EXP1HCL | SAFE-NO-UNMASK | As above, through `GetCurrentMotion` (`Move.cpp:2500`); main task |
| `CanDevice::Interrupt [primask] -> indirect call` | all | SAFE-NO-UNMASK | `rxCallback` inside the locker (CoreN2G `CanDevice.cpp:1289-1290`) is `StepTimer::CanTimingMessageReceived` (`CAN/CanInterface.cpp:268`). It stores three values and reads the timer (`Movement/StepTimer.cpp:195-200`), and `--unmask --query` shows that it reaches nothing that lifts a mask |
