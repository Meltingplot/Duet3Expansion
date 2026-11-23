# Census of the heap-in-region allowlist

Each of the 90 entries of `allowlist.txt` was checked. For a heap finding: which task runs the region, whether the heap can
really be reached there, and which tasks could be inside `malloc` at that moment. For a call through a register: every target
that the firmware sets, and whether one of them reaches the heap. The verdict per entry is its comment in `allowlist.txt`; this
file has the reasoning and the findings.

- **Basis:** 3.7-dev at 9ac5016c (2026-11-22), RRFLibraries 3.7-dev 696a015, CoreN2G 3.7-dev 7d41892, FreeRTOS 3.7-dev 980af10,
  boards TOOL1LC (24 entries), EXP1HCL (72) and SZP (21). TOOL1LC and SZP are SAMC21 (Cortex-M0+), EXP1HCL is SAME51
  (Cortex-M4). All three are the CHX expansion boards, so every entry is a CHX path. Line numbers refer to that state.
- **Counts:** 2 SUSPECT, 2 SAFE-PRIMED, 83 SAFE-NO-HEAP, 1 SAFE-NO-TARGET, 2 SAFE-RETURN. 71 of the entries are exception handlers,
  53 of them one per sub-vector of the SAME51 (EIC, SERCOM, DMAC).
- **How:** the heap findings were read in the code. The targets of the calls through a register were read by an agent with file
  and line for each target, checked with `alloc-check.py --query` on each target, and spot-checked. One finding outside the
  allowlist came up on the way: P1 below.
- **Veneers (later):** the check now follows a veneer to the function at the address in its literal. With that,
  `Platform::DoDiagnosticTest [primask]` and `StepTimer::ScheduleMovementCallbackFromIsr [primask]` are no longer findings: their
  only call through a register was a veneer. The other entries keep their verdicts. Behind the veneers the check now sees two
  more calls through a register on TOOL1LC:
  - the `mov pc` switch of `CalcNextStepTimeFull` (see Step timer);
  - the Qfplib compare wrappers, which return with `bx r3` to the address that they copied from `lr` (Qfplib-M0-full
    `qfplib-m0-full_gcc.S:364-392`).

## Verdicts

| Verdict | Meaning |
|---|---|
| SUSPECT | The heap is reached with the scheduler suspended or interrupts masked. The fix branch is in brackets |
| SAFE-PRIMED | The allocation in the region is always served by a free list that is filled just before the region |
| SAFE-NO-HEAP | A call through a register: no target that the firmware sets reaches the heap |
| SAFE-NO-TARGET | A call through a register whose pointer the firmware never sets |
| SAFE-RETURN | The jump through a register is an exception return into a task, not a call |

## Task priorities and who can be inside malloc

From `src/Platform/TaskPriorities.h:14-34`: main 1, LED 1; heat, USB, analog in, TMC (open loop) 2; CAN receiver, Move,
accelerometer 3; TMC (closed loop) 4; CAN sender and CAN clock 5; LDC1612 (SZP) 6. A region in a task of priority p collides with
a task of priority p or lower that was preempted inside `malloc` (see the README of the RepRapFirmware copy).

## Heap findings

| Entry | Task(s) | Verdict | Fix |
|---|---|---|---|
| `ReadWriteLock::LockForReading`, `LockForWriting` (K23) | every task that takes a lock: heat (sensor and heater lists, `Heat.cpp:278`, `:306` in `Heat::TaskLoop`), CAN sender (`InputMonitor::AddStateChanges`, `InputMonitor.cpp:539`, from `CanInterface.cpp:1003`), main | SUSPECT: the heat task (2) or the CAN sender (5) against the main task (1) inside `malloc`, whenever the lock record freelist is empty | RRFLibraries `fix/alloc-outside-critical-section` 1e34289 |
| `Move::AddLinearSegments [primask]` (TOOL1LC), `[basepri]` (EXP1HCL) `-> Tasks::AllocPermanent` | Move | SAFE-PRIMED | upstream e847c690 |

- **K23.** The same code as in RepRapFirmware (RRFLibraries `RTOSIface.cpp:235-470`): the lock record is allocated after
  `EnterTaskCriticalSection`. Built against the fixed RRFLibraries (with `LIBRARIES_DIR`), all three boards have exactly
  the same entries minus the two `ReadWriteLock` keys. The host test of the fix is `Tools/lock-record-alloc-test` in the
  RepRapFirmware fork (branch `fix/lock-record-alloc`).
- **`Move::AddLinearSegments`.** `MoveSegment::PrimeFreeList` (`Move.cpp:1119`) puts a segment on the free list before
  `IrqSave` or `ChangeBasePriority` (`:1120-1124`). The region splits at most one segment (`:1176`, `MoveSegment::Split` in
  `MoveSegment.h:297-310`, which calls `MoveSegment::Allocate`) and then breaks out of the loop (`:1191`). Only `AddSegment` and
  `AddLinearSegments` allocate segments (`Move.cpp:965`, `:1015`, `:1060`, `:1082`, `:1176`), in the Move task (`Move.cpp:627`,
  `:635`); the step ISR only releases them. SZP has no such key.

### P1: Tasks::AllocPermanent takes no lock

`Tasks::AllocPermanent` (`Tasks.cpp:1141-1147`) calls `__malloc_lock(nullptr)` and `__malloc_unlock(nullptr)`. Only the RP2040
builds define these (`Tasks.cpp:139`, `:157`); in the other builds they are newlib's, whose `__retarget_lock_acquire_recursive`
is `bx lr` in all three ELF files (TOOL1LC 0x22abc, EXP1HCL 0x39dfc, SZP 0x1b894). So `CoreAllocPermanent` lowers `heapLimit`
(CoreN2G `syscalls.h:66-78`) without the malloc mutex, while `_sbrk` (`syscalls.h:49-64`) in a task inside `malloc` raises
`heapTop` under the mutex. If one is preempted between its test and its store, both can hand out the same memory.

- Callers: `MoveSegment::operator new` (`MoveSegment.h:83-84`, at run time in the Move task when the free list is empty) and
  `MessageBufferAlloc` (`Tasks.cpp:1234-1237`, `CanMessageBuffer::Init` at `CanInterface.cpp:322`, in the main task before the
  CAN tasks are created). So the window is open at run time on TOOL1LC and EXP1HCL (Move, priority 3, against any task inside
  `malloc`); on SZP only at start-up.
- The check did not see it, because `Tasks::AllocPermanent` called no heap function: `__malloc_lock` was not a target, and
  `CoreAllocPermanent` is inline. It came up because the SAFE-PRIMED key of RepRapFirmware was missing here. `__malloc_lock`,
  `__malloc_unlock`, `Tasks::AllocPermanent` and `MessageBufferAlloc` are targets now.
- Fix: branch `fix/alloc-permanent-lock`, `Tasks::AllocPermanent` takes `GetMallocMutex`, as RepRapFirmware's does
  (`Tasks.cpp:323-329` there). Host test `Tools/alloc-permanent-lock-test`. With the fix the three boards have the same entries.
- Since e539a216 (2023-01-18) and 6d52ab31 (2023-03-31), see the README. Upstream 3.7-dev has the same code.

## Calls through a register

Each group shares one reason in `allowlist.txt`. CoreN2G is abbreviated CN, `CN/SX` is `CoreN2G/src/SAME5x_C21`.

**Exception handlers.**
- **SERCOM (38 keys).** `sercomIrq[n]` (`CN/SX/Serial.cpp:166-206`) is `DummyHandler` (`:144-148`) or
  `SharedI2CMaster::CommonInterrupt`, which `SharedI2CMaster`'s constructor sets (`CN/SX/SharedI2CMaster.cpp:73`, `:75`; objects at
  `Platform.cpp:920`, `:925`). It gives a task notification (`:328`, `:357`, `:393`). `AsyncSerial` is not linked.
- **EIC (17 keys).** `exintCallbacks` (`CN/SX/Interrupts.cpp:27`, set at `:114-115`) are the LIS3DH INT1 (`LISAccelerometer.cpp:291`),
  the fan tacho (`LocalFan.cpp:200`), `InputMonitor::CommonDigitalPortInterrupt` (`InputMonitor.cpp:74`, `:79`), the filament
  monitors' `InterruptEntry` (`FilamentMonitor.cpp:108`, the three `Interrupt` overrides), the TMC22xx DIAG pin (TOOL1LC,
  `TMC22xx.cpp:1215`, `:2438`) and the LDC1612 (SZP, `ScanningSensorHandler.cpp:252`): counters, flags, timer reads and
  `GiveFromISR`.
- **DMAC (6 keys).** The callbacks (`CN/SX/DmacManager.cpp:294-297`) of TMC22xx (`TMC22xx.cpp:2363`), the SAMC21 ADCs
  (`CN/SX/SAMC21/AnalogIn.cpp:277`, `:517`), `SharedI2CMaster` (`:228`), TMC51xx (`TMC51xx.cpp:2154`, `:2198`) and `SpiDevice`
  (`CN/SX/SpiDevice.cpp:237`) give a task notification.
- **CAN.** `CAN0_Handler` and `CanDevice::Interrupt` (`CN/CanDevice.cpp:1288-1290`, `:1369`): `rxCallback` is
  `StepTimer::CanTimingMessageReceived` (`CanInterface.cpp:268`), `txCallback` is null (`:264`) and its interrupt is enabled only
  when it is set (`CN/CanDevice.cpp:471-475`). `CAN1_Handler` is SAFE-NO-TARGET: only the port of `instanceNumber` 0 is
  initialised and its IRQ enabled (`CN/CanDevice.cpp:456-479`).
- **Step timer** (`TC2_Handler` on SAMC21, `TC0_Handler` on EXP1HCL, `StepTimer.cpp:416-449`). `Move::Interrupt` and the step
  functions run from RAM and are called through veneers with fixed literals. The `mov pc` switch in
  `DriveMovement::CalcNextStepTimeFull` (`DriveMovement.cpp:785`) stays inside the function. The generic timer list has no target
  on the SAMC21 boards (`ScheduleCallbackFromIsr` is not linked) and `TmcTimerCallback` (`TMC51xx.cpp:1867-1870`) on EXP1HCL.
- **NVMCTRL (3 keys).** `flash_ready`/`flash_error` call `cb_ready`/`cb_error` only when set (`hal_flash.c:295-313`), and
  `flash_register_callback` has no caller.
- **PendSV, SVC (SAMC21).** SAFE-RETURN: the `bx` is the EXC_RETURN popped from the task stack (FreeRTOS
  `ARM_CM0/portasm.c:392-433`) or the start of the first task (`:498-522`, `port.c:871-876`).
- **SysTick.** Only on the stuck-task path before a reset: `Heat::SwitchOffAllLocalFromISR` (`Heat.cpp:662-671`) calls
  `LocalHeater::SwitchOff`, which sets the PWM. Unlike RepRapFirmware (B3 there) it prints nothing.

**Task regions.**
- **AnalogIn** (`AdcClass::ExecuteCallbacks`, `SdAdcClass::ExecuteCallbacks` on SAMC21, `TaskLoop` on EXP1HCL; AIN task, 2):
  `AveragingFilter::CallbackFeedIntoFilter` (`AveragingFilter.h:84-91`) or `InputMonitor::CommonAnalogPortInterrupt`
  (`InputMonitor.cpp:112`, a task notification).
- **Step functions** (`Move::AddLinearSegments` in the Move task, 3; `Move::PhaseStepControlLoop` and
  `ClosedLoop::InstanceControlLoop` in the TMC task; `Move::ProcessM970` in main): `debugPrintf` (`RepRapFirmware.cpp:30-39`),
  whose putc `CanInterface::DebugPutc` (`CanInterface.cpp:962-974`) writes a ring buffer and wakes the CAN sender, and on TOOL1LC
  the `mov pc` switch and the Qfplib wrappers behind the veneers (see Veneers above). See B1 and B2.
- **`Platform::DoDiagnosticTest`** (main): the veneer to `fastSqrtf` in the timing test (`Platform.cpp:1594-1602`). No longer a
  finding since the check follows veneers.
- **Closed loop** (`ClosedLoop::InstanceDiagnostics`, `ProcessM569Point4`, `ResetError`, `SetClosedLoopEnabled`; EXP1HCL, main
  task and Move task): the `TakeReading` overrides of `AbsoluteRotaryEncoder` (AS5047D, MT6835), `RelativeEncoder` (quadrature
  PDEC) and `LinearCompositeEncoder`. See B3.

## Findings outside this class

None of these allocates, so the check does not report them, and none is fixed here.
- **B1, EXP1HCL: a step error inside `Move::AddLinearSegments` lifts the step interrupt guard.** On the path
  `ScheduleFirstSegment` or `Move::Interrupt` to `CalcNextStepTimeFull` to `LogStepError` (`DriveMovement.cpp:620-631`) to
  `debugPrintf`, `CanInterface::DebugPutc` calls `WakeAsyncSender` (`CanInterface.cpp:795-806`). In a task this is
  `TaskBase::Give`, that is `xTaskGenericNotify`, which leaves its critical section with `vPortExitCritical`. On the Cortex-M4
  that writes BASEPRI = 0 (FreeRTOS `ARM_CM4F/port.c:492-501`), so the `ChangeBasePriority(NvicPriorityStep)` of
  `Move.cpp:1306` ends early.
- **B2, TOOL1LC: the same path inside `IrqSave` (`Move.cpp:1304`).** The Cortex-M0+ `vPortExitCritical` runs `cpsie i`
  (`ARM_CM0/port.c:803-812`), so interrupts are enabled again inside the region.

  `make scan-unmask` finds B1 and B2 as calls through a register (`unmask-census.md`), because the putc of `debugPrintf` is one.
- **B3, EXP1HCL: an SPI mutex inside a suspended region.** The AS5047D and MT6835 readings select the shared SPI device
  (`CN/SPI/SharedSpiDevice.h:20`, `Mutex::Take` with 0 ticks) inside the closed loop regions. There is no wait, FreeRTOS asserts only
  for a timeout other than 0 with the scheduler suspended (`queue.c:1682`), but the reading fails when another task holds the
  SPI bus, and `ClosedLoop::ResetError` ignores that (`ClosedLoop.cpp:1202`).
- **Debug buffer.** `debugBuffer` has two writers, the step ISR and the tasks, and `RingBuffer` assumes one
  (RRFLibraries `RingBuffer.h:59`, `:84-94`).

## UNCLEAR

- U1. Whether B1 and B2 can occur: `LogStepError` runs only on step errors (`DriveMovement.cpp:585-892`); the census did not settle
  whether one can occur inside `AddLinearSegments`.
- U2. Whether a caller of the EXP1HCL `[primask]` regions holds an outer `ChangeBasePriority`, which B1's `vPortExitCritical` would
  clear too.
- U3. On the SAMC21 some AnalogIn callbacks are passed in registers, not as literals; the targets rest on the source
  registrations and on `nm` (only the two functions above have the signature), not on a check for function pointer casts.
- U4. The NVIC priority that the FreeRTOS ports give SysTick, PendSV and SVC: `configLIBRARY_LOWEST_INTERRUPT_PRIORITY` is commented
  out in FreeRTOS `include/FreeRTOSConfig.h:314`; not traced.
- U5. Which tasks call `malloc` at run time while the Move task allocates segments (the window of P1): not traced.
