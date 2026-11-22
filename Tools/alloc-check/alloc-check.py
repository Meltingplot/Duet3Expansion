#!/usr/bin/env python3
"""Lists the paths from inside a protected region of Duet3Expansion to the heap, and fails for each one that allowlist.txt does
not name. See README.md.

A protected region is code that runs while the scheduler is suspended (vTaskSuspendAll), inside a FreeRTOS critical section
(vPortEnterCritical), with BASEPRI raised (msr BASEPRI_MAX), with PRIMASK set (cpsid i), or in an exception handler of the
vector table. The check reads the machine code of the linked firmware with the objdump of the Arm GNU toolchain, follows the
control flow of each function and the direct calls between functions, and reports each call inside a region from which the heap
can be reached, and each call inside a region whose target it cannot see.

    alloc-check.py [--allowlist FILE] [--update] [--keys] [--verbose] [--root DIR] ELF...
"""
import argparse
import collections
import os
import re
import struct
import subprocess
import sys

# The functions that change or read the heap of src/libc/nano-mallocr.c, the end of the heap (_sbrk) and its limit
# (CoreAllocPermanent) in CoreN2G src/syscalls.h, the malloc mutex of src/Platform/Tasks.cpp, and newlib's malloc lock
# (__malloc_lock, which Tasks::AllocPermanent of Duet3Expansion calls). A function that calls one of them, directly or through
# other direct calls, reaches the heap.
HEAP = {'malloc', 'free', 'realloc', 'calloc', 'cfree', 'memalign', 'valloc', 'pvalloc', 'mallinfo', 'malloc_stats', '_malloc_r',
        '_sbrk', 'CoreAllocPermanent', 'GetMallocMutex', 'ReleaseMallocMutex', '__malloc_lock', '__malloc_unlock'}
# A finding names the first of these on its chain, and a function that calls one reaches the heap even when the compiler has
# inlined the heap functions into it: the C++ allocation functions of libsupc++, the exception allocation of
# libsupc++, Tasks::AllocPermanent of src/Platform/Tasks.cpp, and MessageBufferAlloc, through which CANlib's
# CanMessageBuffer allocates (CANlib src/CanMessageBuffer.h); the compiler can inline all of Tasks::AllocPermanent into it
ENTRY = re.compile(r'^(operator (new|delete)(\[\])?|Tasks::AllocPermanent|MessageBufferAlloc|'
                   r'__cxa_(allocate|free)(_dependent)?_exception)\b')
CLONE_RE = re.compile(r'(\.(part|isra|constprop|cold|lto_priv)\.\d+)+$| \[clone [^\]]*\]')


def is_heap(name):
    """A heap function or an allocation function, also as a clone that the compiler split off (GetMallocMutex.part.0): a
    function that calls one reaches the heap."""
    return CLONE_RE.sub('', name) in HEAP or ENTRY.match(name) is not None


# The region primitives. RTOSIface::EnterTaskCriticalSection, LeaveTaskCriticalSection, TaskCriticalSectionLocker and
# InterruptCriticalSectionLocker (RRFLibraries src/RTOSIface/RTOSIface.h) and taskENTER_CRITICAL, taskEXIT_CRITICAL (FreeRTOS
# portmacro.h) are inline and leave these calls. IrqDisable, IrqSave, IrqRestore, AtomicCriticalSectionLocker (CoreN2G Core.h,
# CoreIO.h) leave cpsid i, mrs/msr PRIMASK; ChangeBasePriority, RestoreBasePriority and BasePriorityBooster (CoreIO.h) leave
# msr BASEPRI_MAX and msr BASEPRI.
SUSPEND = {'vTaskSuspendAll': 1, 'xTaskResumeAll': -1}
CRITICAL = {'vPortEnterCritical': 1, 'vPortExitCritical': -1}
PRIMITIVES = set(SUSPEND) | set(CRITICAL)
KINDS = ('suspended', 'critical', 'basepri', 'primask')
ZERO = (0, 0, 0, 0)
CAP = 3                                                         # the nesting depth that the analysis keeps apart

COND = r'(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)'
CALL_RE = re.compile(r'^bl' + COND + r'?(?:\.w)?$')
BRANCH_RE = re.compile(r'^b(' + COND + r')?(?:\.[nw])?$')
CBZ_RE = re.compile(r'^cbn?z$')
IT_RE = re.compile(r'^it[te]{0,3}$')
COPY_RE = re.compile(r'^(uxt[bh]|movs?)(\.[nw])?$')
NO_WRITE = ('cmp', 'cmn', 'tst', 'teq', 'str', 'cb', 'push')   # instructions whose first operand is not written
FUNC_RE = re.compile(r'^([0-9a-f]{8}) <(.+)>:$')
INSN_RE = re.compile(r'^\s*([0-9a-f]+):\t(\S+)(?:\t(.*))?$')
TARGET_RE = re.compile(r'(?:^|, )([0-9a-f]+) <(.+?)(\+0x[0-9a-f]+)?>$')
REG = r'(r\d+|sl|fp|ip|sb|lr)'
LITERAL_RE = re.compile(r'^' + REG + r', \[pc, #-?\d+\]\s*@ \(([0-9a-f]+) ')
VENEER_RE = re.compile(r'^__(.+)_veneer$')
LIBRARIES = ('CANlib', 'CoreN2G', 'FreeRTOS', 'Qfplib-M0-full', 'RRFLibraries')


def tool_lines(cmd):
    """Yields the output lines of a toolchain program without keeping all of it in memory."""
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, errors='replace') as p:
        yield from p.stdout
    if p.returncode:
        sys.exit(f'error: {" ".join(cmd)} failed with exit code {p.returncode}')


class Elf:
    """Reads the allocated sections of a little-endian 32-bit ELF file: jump tables, literals and the vector table."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        if self.data[:4] != b'\x7fELF' or self.data[4] != 1 or self.data[5] != 1:
            sys.exit(f'error: {path} is not a little-endian 32-bit ELF file')
        shoff, = struct.unpack_from('<I', self.data, 0x20)
        shentsize, shnum = struct.unpack_from('<HH', self.data, 0x2e)
        self.sections = []
        for i in range(shnum):
            _, stype, flags, addr, off, size = struct.unpack_from('<IIIIII', self.data, shoff + i * shentsize)
            if stype == 1 and flags & 2 and size:              # SHT_PROGBITS, SHF_ALLOC
                self.sections.append((addr, size, off))

    def read(self, addr, size):
        for base, length, off in self.sections:
            if base <= addr and addr + size <= base + length:
                return self.data[off + addr - base:off + addr - base + size]
        return None

    def word(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('<I', b)[0] if b else None


def target_of(ops):
    m = TARGET_RE.search(ops.split('\t@')[0].split(' @ ')[0].strip())
    return (int(m.group(1), 16), m.group(2), m.group(3)) if m else None


def strip_cond(mn):
    """The instruction without the condition that the disassembler appends inside an IT block, e.g. popeq -> pop."""
    m = re.match(r'^(pop|ldmia|ldm|ldr|bx|blx|bl|b|mov|movs|msr|cpsid|cpsie|tbb|tbh|udf)' + COND + r'((?:\.[nw])?)$', mn)
    return m.group(1) + m.group(2) if m else mn


def is_return(mn, ops):
    if mn.startswith('bx') and ops == 'lr':
        return True
    if mn.startswith(('pop', 'ldm')) and re.search(r'\bpc\b', ops):
        return True
    return mn.startswith('ldr') and ops.startswith('pc,') and '[sp]' in ops


class Function:
    def __init__(self, name, start):
        self.name, self.start, self.end = name, start, start
        self.insns = []                                         # (address, mnemonic, operands)

    def prepare(self, prog):
        """Classifies each instruction once: (what, conditional, detail), and collects the calls."""
        self.ops = []
        self.calls = []                                         # (index, callee, tail call)
        self.indirect = []                                      # indices of calls and jumps through a register
        index = {a: k for k, (a, _, _) in enumerate(self.insns)}
        it_left = 0
        for k, (addr, mn, ops) in enumerate(self.insns):
            cond = it_left > 0
            if it_left:
                it_left -= 1
            if IT_RE.match(mn):
                it_left = len(mn) - 1
                self.ops.append(('next', False, None))
                continue
            b = strip_cond(mn) if cond else mn
            t = target_of(ops) if ('<' in ops and '>' in ops) else None
            if CALL_RE.match(b) and t and t[2] is not None and self.start <= t[0] < self.end:
                # Thumb-1 code (Cortex-M0+) has no long unconditional branch and jumps far inside a function with bl
                self.ops.append(('branch', cond, index.get(t[0])))
            elif CALL_RE.match(b):
                callee = prog.resolve(t[1]) if t and t[2] is None else None
                if callee:
                    self.calls.append((k, callee, False))
                self.ops.append(('call', cond, callee))
            elif BRANCH_RE.match(b) and t:
                conditional = cond or BRANCH_RE.match(b).group(1) is not None
                if self.start <= t[0] < self.end:
                    self.ops.append(('branch', conditional, index.get(t[0])))
                else:
                    callee = prog.resolve(t[1])
                    self.calls.append((k, callee, True))
                    self.ops.append(('tail', conditional, callee))
            elif CBZ_RE.match(b) and t:
                self.ops.append(('branch', True, index.get(t[0])))
            elif is_return(b, ops):
                self.ops.append(('return', cond, None))
            elif b in ('blx', 'bx') and re.match(r'^' + REG + r'$', ops):
                callee = prog.literal_target(self, k, ops)
                tail = b == 'bx'
                if callee:
                    self.calls.append((k, callee, tail))
                    self.ops.append(('tail' if tail else 'call', cond, callee))
                else:
                    self.indirect.append(k)
                    self.ops.append(('itail' if tail else 'icall', cond, None))
            elif b.startswith(('mov', 'add')) and ops.startswith('pc,'):
                targets = prog.address_table(self, k, index) if b == 'mov' else None
                if targets is not None:
                    self.ops.append(('table', cond, targets))
                else:
                    self.indirect.append(k)
                    self.ops.append(('itail', cond, None))
            elif b.startswith(('tbb', 'tbh')):
                self.ops.append(('table', cond, prog.jump_table(self, k, index)))
            elif b == 'cpsid' and ops.startswith('i'):
                self.ops.append(('flag', cond, (3, 'raise')))
            elif b == 'cpsie' and ops.startswith('i'):
                self.ops.append(('flag', cond, (3, 'clear')))
            elif b == 'msr' and re.match(r'^(BASEPRI|BASEPRI_MAX|PRIMASK), ', ops, re.I):
                reg, src = [x.strip() for x in ops.split(',', 1)]
                value = prog.msr_value(self, k, src)
                slot = 2 if reg.upper().startswith('BASEPRI') else 3
                if value is None:
                    prog.unknown_msr.append((self.name, addr))
                    effect = 'raise' if reg.upper() == 'BASEPRI_MAX' else 'restore'
                elif reg.upper() == 'BASEPRI_MAX':
                    effect = 'none' if value == ('const', 0) else 'raise'
                elif value == 'saved':
                    effect = 'restore'
                else:
                    effect = 'clear' if value[1] == 0 else 'raise'
                self.ops.append(('flag', cond, (slot, effect)))
            elif b.startswith(('udf', '.')) or (b.startswith('b') and not b.startswith(('bic', 'bfc', 'bfi', 'bkpt')) and not t):
                self.ops.append(('stop', cond, None))           # data, a trap, or not an instruction
            else:
                self.ops.append(('next', cond, None))


class Program:
    def __init__(self, elf, objdump, nm):
        self.elf = Elf(elf)
        self.funcs = []
        cur = None
        for line in tool_lines([objdump, '-d', '-C', '--no-show-raw-insn', elf]):
            line = line.rstrip('\n')
            m = FUNC_RE.match(line)
            if m:
                cur = Function(m.group(2), int(m.group(1), 16))
                self.funcs.append(cur)
                continue
            m = INSN_RE.match(line) if cur is not None else None
            if m:
                cur.insns.append((int(m.group(1), 16), m.group(2), (m.group(3) or '').strip()))
        self.funcs.sort(key=lambda f: f.start)
        for i, f in enumerate(self.funcs):
            f.end = self.funcs[i + 1].start if i + 1 < len(self.funcs) else (f.insns[-1][0] + 4 if f.insns else f.start)
        self.by_name, self.by_start = {}, {}
        for f in self.funcs:
            self.by_name.setdefault(f.name, f)
            self.by_start[f.start] = f
        self.local, self.symbols = set(), {}
        for line in tool_lines([nm, '-S', '-C', '--defined-only', elf]):
            parts = line.rstrip('\n').split(' ', 3)
            if len(parts) == 4:
                self.symbols[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
                if parts[2] == 't':
                    self.local.add(parts[3])
        self.unbounded_tables, self.unknown_msr = [], []
        for f in self.funcs:
            f.prepare(self)
        # A function that has no return and no jump through a register, and whose tail calls all go to functions that do not
        # return, does not return (vAssertCalled, SoftwareReset, a function that throws): a call to it ends the path
        self.noreturn = set()
        while True:
            more = {f.name for f in self.funcs if f.insns and f.name not in self.noreturn and not any(
                what in ('return', 'itail') or (what == 'tail' and detail not in self.noreturn) for what, _, detail in f.ops)}
            if not more:
                break
            self.noreturn |= more

    def resolve(self, name):
        m = VENEER_RE.match(name)
        return m.group(1) if m and m.group(1) in self.by_name else name

    def literal_target(self, f, k, reg):
        """The function whose address the register holds at a call through it, if it was loaded from a literal a few
        instructions before (vAssertCalled jumps to its dispatcher this way)."""
        for q in range(k - 1, max(k - 8, -1), -1):
            _, mn, ops = f.insns[q]
            if mn.startswith('bl'):
                return None
            if re.match(r'^' + reg + r'\b', ops) and not mn.startswith(('cmp', 'tst', 'str', 'cb')):
                m = LITERAL_RE.match(ops) if mn.startswith('ldr') else None
                if m:
                    value = self.elf.word(int(m.group(2), 16))
                    if value is not None and value & 1 and (value & ~1) in self.by_start:
                        return self.by_start[value & ~1].name
                return None
        return None

    def table_size(self, f, k, reg):
        """The number of entries of the jump table that reg indexes at f.insns[k]: one more than the immediate of the bounds check
        (cmp reg, #imm) a few instructions before, also when the check compares a copy of reg (uxtb, uxth, mov), as the compiler
        does. None if there is no such check, or reg changes after it."""
        def writes(mn, ops, regs):
            return not mn.startswith(NO_WRITE) and re.match(r'^(' + '|'.join(regs) + r')\b', ops)
        for q in range(k - 1, max(k - 8, -1), -1):
            _, mn, ops = f.insns[q]
            m = re.match(r'^' + REG + r', #(\d+)(?:\s|$)', ops) if mn.startswith('cmp') else None
            if m and m.group(1) == reg:
                return int(m.group(2)) + 1
            if m:
                for p in range(q - 1, max(q - 6, -1), -1):
                    _, mn2, ops2 = f.insns[p]
                    c = re.match(r'^' + REG + r', ' + REG + r'$', ops2) if COPY_RE.match(mn2) else None
                    if c and {c.group(1), c.group(2)} == {reg, m.group(1)}:
                        return int(m.group(2)) + 1
                    if writes(mn2, ops2, (reg, m.group(1))):
                        return None
                return None
            if not (BRANCH_RE.match(mn) or CALL_RE.match(mn)) and writes(mn, ops, (reg,)):
                return None
        return None

    def jump_table(self, f, k, index):
        """The successors of tbb/tbh at f.insns[k], read from the table after it. The number of entries comes from the bounds
        check before it; without one every later instruction of the function counts as a successor."""
        addr, mn, ops = f.insns[k]
        m = re.match(r'^\[pc, ' + REG + r'(?:, lsl #1)?\]$', ops)
        count = self.table_size(f, k, m.group(1)) if m else None
        size = 2 if mn.startswith('tbh') else 1
        data = self.elf.read(addr + 4, count * size) if count else None
        if data is None:
            self.unbounded_tables.append((f.name, addr))
            return list(range(k + 1, len(f.insns)))
        offsets = data if size == 1 else struct.unpack(f'<{count}H', data)
        return sorted({index[addr + 4 + 2 * o] for o in offsets if addr + 4 + 2 * o in index})

    def address_table(self, f, k, index):
        """The successors of mov pc, rX at f.insns[k] if it jumps through a table of addresses, as switch statements do in Thumb-1
        code (Cortex-M0+): ldr rT, [pc, #n] (the table), lsls rS, rI, #2, ldr rX, [rT, rS], mov pc, rX. The number of entries
        comes from the bounds check before it; without one every later instruction of the function counts as a successor. None
        if the instructions before do not have this form or an entry is not an address in the function."""
        m = re.match(r'^pc, ' + REG + r'$', f.insns[k][2])
        load = re.match(r'^' + m.group(1) + r', \[' + REG + r', ' + REG + r'\]$', f.insns[k - 1][2]) \
            if m and k > 0 and f.insns[k - 1][1] == 'ldr' else None
        if not load:
            return None
        base, scaled = load.group(1), load.group(2)
        table = shift = None
        for q in range(k - 2, max(k - 6, -1), -1):
            _, mn, ops = f.insns[q]
            mm = re.match(r'^' + scaled + r', ' + REG + r', #2$', ops) if mn == 'lsls' and shift is None else None
            if mm:
                shift = q
                continue
            mm = LITERAL_RE.match(ops) if mn == 'ldr' and ops.startswith(base + ',') and table is None else None
            if mm:
                table = self.elf.word(int(mm.group(2), 16))
                continue
            if mn.startswith('bl') or re.match(r'^(' + base + '|' + scaled + r')\b', ops):
                break
        if table is None or shift is None:
            return None
        count = self.table_size(f, shift, re.match(r'^' + scaled + r', ' + REG, f.insns[shift][2]).group(1))
        data = self.elf.read(table, 4 * count) if count else None
        if data is None:
            self.unbounded_tables.append((f.name, f.insns[k][0]))
            return list(range(k + 1, len(f.insns)))
        # mov pc ignores bit 0 of the address (BranchWritePC in the ARMv6-M Architecture Reference Manual), and GCC leaves it clear
        entries = [e & ~1 for e in struct.unpack(f'<{count}I', data)]
        if not all(e in index for e in entries):
            return None
        return sorted({index[e] for e in entries})

    def msr_value(self, f, k, reg):
        """What the register that msr at f.insns[k] writes holds: 'saved' (read with mrs or reloaded from memory),
        ('const', n), or None if the few instructions before do not tell."""
        for q in range(k - 1, -1, -1):
            _, mn, ops = f.insns[q]
            if mn == 'mrs' and ops.startswith(reg + ','):
                return 'saved'
            m = re.match(r'^' + reg + r', #(\d+)', ops)
            if m and re.match(r'^(movs?|movw)(\.w)?$', mn):
                return ('const', int(m.group(1)))
            m = re.match(r'^' + reg + r', ' + REG + r'$', ops)
            if m and re.match(r'^movs?(\.w)?$', mn):
                reg = m.group(1)
                continue
            if mn.startswith('ldr') and ops.startswith(reg + ', ['):
                return 'saved'
            if mn.startswith('bl') and reg in ('r0', 'r1', 'r2', 'r3', 'ip'):
                return None
            if re.match(r'^' + reg + r'\b', ops) and not mn.startswith(('cmp', 'tst', 'str', 'cb', 'msr')):
                return None
        return None

    def analyse(self, f, summaries):
        """Data flow over one function. The state is (suspended, critical, basepri, primask), each a nesting depth relative to the
        entry of the function, capped at +-CAP. Returns the set of states before each instruction and at the returns."""
        n = len(f.insns)
        states = [set() for _ in range(n)]
        exits = set()
        if not n:
            return states, {ZERO}
        states[0].add(ZERO)
        work = collections.deque([0])

        def clamp(v):
            return max(-CAP, min(CAP, v))

        def flag(st, slot, effect):
            v = st[slot]
            v = clamp(v + 1) if effect == 'raise' else clamp(v - 1) if effect == 'restore' else min(v, 0) if effect == 'clear' else v
            return st[:slot] + (v,) + st[slot + 1:]

        def through(st, callee):
            if callee in SUSPEND:
                return {(clamp(st[0] + SUSPEND[callee]),) + st[1:]}
            if callee in CRITICAL:
                return {(st[0], clamp(st[1] + CRITICAL[callee])) + st[2:]}
            if callee in summaries:
                d = summaries[callee]
                return {tuple(clamp(a + b) for a, b in zip(st, d))}
            return {st}

        while work:
            k = work.popleft()
            what, cond, detail = f.ops[k]
            nxt = k + 1 if k + 1 < n else None
            for st in list(states[k]):
                succ = []
                if what == 'call':
                    outs = through(st, detail) if detail else {st}
                    if detail not in self.noreturn:
                        succ.extend(outs)
                    if cond:
                        succ.append(st)
                    succ = [(nxt, s) for s in succ]
                elif what == 'tail':
                    exits.update(through(st, detail))
                    if cond:
                        succ.append((nxt, st))
                elif what in ('return', 'itail'):
                    exits.add(st)
                    if cond:
                        succ.append((nxt, st))
                elif what == 'branch':
                    if detail is not None:
                        succ.append((detail, st))
                    if cond:
                        succ.append((nxt, st))
                elif what == 'table':
                    succ.extend((q, st) for q in detail)
                elif what == 'flag':
                    succ.append((nxt, flag(st, *detail)))
                    if cond:
                        succ.append((nxt, st))
                elif what == 'stop':
                    if cond:
                        succ.append((nxt, st))
                else:                                           # next, icall
                    succ.append((nxt, st))
                for q, s2 in succ:
                    if q is not None and s2 not in states[q]:
                        states[q].add(s2)
                        work.append(q)
        return states, exits

    def changes_state(self, f):
        return any(what == 'flag' for what, _, _ in f.ops) or any(c in PRIMITIVES for _, c, _ in f.calls)

    def summaries(self, kernel):
        """The region state that a function leaves to its caller, for the functions that change it on every path in the same way
        (e.g. an out-of-line constructor of TaskCriticalSectionLocker). A function that changes it on some paths only is taken
        as leaving it unchanged and listed in mixed. FreeRTOS kernel functions (kernel) are taken as leaving it unchanged: they
        inline vTaskSuspendAll, which the check cannot see, and each API function returns with the state that it found."""
        result, mixed = {}, {}
        candidates = [f for f in self.funcs if f.name not in PRIMITIVES and f.name not in kernel and self.changes_state(f)]
        for _ in range(8):
            changed = False
            for f in candidates:
                _, exits = self.analyse(f, result)
                deltas = set(exits) - {ZERO} if exits else set()
                if len(exits) == 1 and deltas:
                    d = next(iter(deltas))
                    if result.get(f.name) != d:
                        result[f.name] = d
                        changed = True
                elif deltas:
                    mixed[f.name] = sorted(exits)
            if not changed:
                break
            candidates = [f for f in self.funcs if f.name not in PRIMITIVES and f.name not in kernel and
                          (self.changes_state(f) or any(c in result for _, c, _ in f.calls))]
        for name in result:
            mixed.pop(name, None)
        return result, mixed

    def reach(self, roots, stop=frozenset()):
        """For each function that reaches one of the roots through direct calls, the shortest chain of names to it. The chains
        do not pass through the functions in stop."""
        callers = collections.defaultdict(set)
        for f in self.funcs:
            for _, name, _ in f.calls:
                if name != f.name:
                    callers[name].add(f.name)
        chains = {r: [r] for r in roots}
        queue = collections.deque(sorted(roots))
        while queue:
            n = queue.popleft()
            if n in stop:
                continue
            for caller in sorted(callers[n]):
                if caller not in chains and caller not in PRIMITIVES:
                    chains[caller] = [caller] + chains[n]
                    queue.append(caller)
        return chains


def query(prog, names):
    """For each function named, every heap function and every call through a register that it reaches through direct calls,
    each with one chain (the census uses this to see everything that a region calls)."""
    calls = collections.defaultdict(set)
    for f in prog.funcs:
        for _, name, _ in f.calls:
            calls[f.name].add(name)
    lines = []
    for wanted in names:
        starts = [f.name for f in prog.funcs if f.name == wanted or short(f.name) == wanted]
        if not starts:
            lines.append(f'{wanted}: not in this firmware')
        for start in starts:
            chains, queue, hits = {start: [start]}, collections.deque([start]), []
            while queue:
                n = queue.popleft()
                if is_heap(n):
                    hits.append(' -> '.join(short(c) for c in chains[n]))
                    continue
                f = prog.by_name.get(n)
                if f is not None and f.indirect and n not in prog.noreturn:
                    hits.append(' -> '.join(short(c) for c in chains[n]) + ' -> (through a register)')
                for c in sorted(calls[n]):
                    if c not in chains and c not in PRIMITIVES:
                        chains[c] = chains[n] + [c]
                        queue.append(c)
            lines.append(f'{start}: ' + ('nothing' if not hits else ''))
            lines += [f'  {h}' for h in hits]
    return lines


def short(name):
    """A function name without its parameter list, clone suffix and template arguments, for the keys."""
    name = re.sub(r' \[clone [^\]]*\]', '', name)
    depth, cut = 0, len(name)
    for i in range(len(name) - 1, -1, -1):                      # the last top-level parenthesis opens the parameter list
        if name[i] == ')':
            depth += 1
        elif name[i] == '(':
            depth -= 1
            if depth == 0:
                cut = i
                break
    base = name[:cut].rstrip()
    while True:
        stripped = re.sub(r'<[^<>]*>', '', base)
        if stripped == base:
            return base
        base = stripped


def target_name(chain):
    """The heap function that a finding names: the first allocation function or heap function on the chain."""
    for n in chain:
        if is_heap(n):
            return short(CLONE_RE.sub('', n))
    return short(chain[-1])


def relative(loc, root):
    """file:line relative to the Duet3Expansion directory (src/...) or, in a library, to the library's parent (CoreN2G/src/...)."""
    loc = re.sub(r' \(discriminator \d+\)$', '', loc)
    path, _, line = loc.rpartition(':')
    if not path:
        return loc
    path = os.path.normpath(path)
    if path.startswith(root + os.sep):
        path = path[len(root) + 1:]
    else:
        parts = path.split(os.sep)
        for i in range(len(parts) - 1, -1, -1):
            if parts[i] in LIBRARIES:
                path = os.sep.join(parts[i:])
                break
    return f'{path}:{line}'


def locate(elf, addrs, addr2line, root):
    """Maps each address to its chain of (function, file:line) from the innermost inlined function to the outermost one."""
    if not addrs:
        return {}
    p = subprocess.run([addr2line, '-a', '-i', '-f', '-C', '-e', elf], input=''.join(f'0x{a:x}\n' for a in addrs),
                       capture_output=True, text=True, errors='replace')
    if p.returncode:
        sys.exit(f'error: {addr2line} failed with exit code {p.returncode}')
    result, cur, lines, i = {}, None, p.stdout.splitlines(), 0
    while i < len(lines):
        if re.match(r'^0x[0-9a-f]+$', lines[i]):
            cur = int(lines[i], 16)
            result[cur] = []
            i += 1
        elif cur is not None and i + 1 < len(lines):
            result[cur].append((lines[i], relative(lines[i + 1], root)))
            i += 2
        else:
            i += 1
    return result


def scan(elf, root, tools):
    """Returns {key: [(address, function, chain, source locations)]}, the program, and notes about what the check cannot see."""
    prog = Program(elf, tools['objdump'], tools['nm'])
    heap = {f.name for f in prog.funcs if is_heap(f.name)}
    if 'malloc' not in heap:
        sys.exit(f'error: {elf} has no malloc, so it is not a firmware that this check knows')
    # The functions of the FreeRTOS kernel, from their source files
    changers = [f for f in prog.funcs if prog.changes_state(f)]
    where = locate(elf, [f.start for f in changers], tools['addr2line'], root)
    kernel = {f.name for f in changers if any(loc.startswith('FreeRTOS' + os.sep) for _, loc in where.get(f.start, []))}
    summaries, mixed = prog.summaries(kernel)
    to_heap = prog.reach(heap)
    # A function that does not return either resets the processor (vAssertCalled, SoftwareReset) or throws, and a throw
    # allocates its exception (__cxa_allocate_exception), which the heap chains find. So the chains to calls through a register
    # do not pass through such functions
    to_indirect = prog.reach({f.name for f in prog.funcs if f.indirect and f.name not in prog.noreturn}, prog.noreturn)
    found = collections.defaultdict(list)                       # (opener, kind, target) -> [(address, opener, chain)]

    for f in prog.funcs:
        if f.name in PRIMITIVES or is_heap(f.name) or f.name in kernel or not (
                prog.changes_state(f) or any(c in summaries for _, c, _ in f.calls)):
            continue
        states, _ = prog.analyse(f, summaries)
        for k, st in enumerate(states):
            inside = sorted({i for s in st for i in range(4) if s[i] > 0})
            if not inside:
                continue
            kind = KINDS[inside[0]]
            what, _, callee = f.ops[k]
            addr = f.insns[k][0]
            if what in ('call', 'tail') and callee and callee not in PRIMITIVES and callee not in summaries:
                if callee in to_heap:
                    found[(f.name, kind, target_name(to_heap[callee]))].append((addr, f.name, to_heap[callee]))
                elif callee in to_indirect:
                    found[(f.name, kind, 'indirect call')].append((addr, f.name, to_indirect[callee]))
            elif what in ('icall', 'itail'):
                found[(f.name, kind, 'indirect call')].append((addr, f.name, []))

    # Exception handlers: every entry of the vector table except the initial stack pointer and the reset handler
    if 'exception_table' in prog.symbols:
        base, size = prog.symbols['exception_table']
        seen = set()
        for i in range(2, size // 4):
            f = prog.by_start.get((prog.elf.word(base + 4 * i) or 0) & ~1)
            if f is None or f.name in seen:
                continue
            seen.add(f.name)
            if f.name in to_heap:
                found[(f.name, 'isr', target_name(to_heap[f.name]))].append((f.start, f.name, to_heap[f.name]))
            elif f.name in to_indirect:
                found[(f.name, 'isr', 'indirect call')].append((f.start, f.name, to_indirect[f.name]))

    analysed = {f.name for f in prog.funcs if f.name not in kernel and prog.changes_state(f)}
    addrs = sorted({a for sites in found.values() for a, _, _ in sites} |
                   {prog.by_name[o].start for (o, _, _) in found if o in prog.local})
    where = locate(elf, addrs, tools['addr2line'], root)
    result = collections.defaultdict(list)
    for (opener, kind, target), sites in found.items():
        name = short(opener)
        if opener in prog.local and ('::' not in name or '(anonymous namespace)' in name):
            # a static function or one in an anonymous namespace: its file makes the key unique
            loc = where.get(prog.by_name[opener].start) or [('?', '?:0')]
            name = f'{loc[-1][1].rsplit(":", 1)[0]}: {name}'
        for addr, fn, chain in sites:
            result[f'{name} [{kind}] -> {target}'].append((addr, fn, chain, where.get(addr, [])))
    notes = [f'jump table at 0x{a:x} in {n} without a bounds check: all later instructions taken as its targets'
             for n, a in prog.unbounded_tables]
    notes += [f'the value that msr at 0x{a:x} in {n} writes is not known: taken as a restore (BASEPRI_MAX: a raise)'
              for n, a in sorted(set(prog.unknown_msr)) if n in analysed]
    notes += [f'{short(n)} changes the region state on some paths only ({", ".join(map(str, e))}): taken as unchanged'
              for n, e in sorted(mixed.items())]
    notes += [f'{short(n)} leaves the region state changed by {d} on every path: its callers count it'
              for n, d in sorted(summaries.items())]
    return result, notes


def read_allowlist(path):
    """Returns the leading comment block and {key: comment}. A key ends at the first '  #'."""
    header, keys = [], {}
    if path and os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.rstrip('\n')
                text, sep, comment = line.partition('  #')
                if line.lstrip().startswith('#') or not line.strip():
                    if not keys:
                        header.append(line)
                    continue
                keys[text.strip()] = comment.strip()
    return header, keys


def describe(sites):
    """The outermost source line of the first site, and every site with its inlined functions and its chain to the heap."""
    first = sites[0][3][-1][1] if sites[0][3] else f'0x{sites[0][0]:x}'
    parts = []
    for addr, _, chain, loc in sites[:3]:
        inl = ' in '.join(f'{short(fn)} {fl}' for fn, fl in loc)
        via = ' -> '.join(short(c) for c in chain[:6]) + (' -> ...' if len(chain) > 6 else '')
        parts.append(f'0x{addr:x} {inl}: ' + (f'calls {via}' if via else 'calls through a register'))
    if len(sites) > 3:
        parts.append(f'{len(sites) - 3} more')
    return first, '; '.join(parts)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('elf', nargs='+', help='linked firmware ELF files with debug information (DEBUG_FLAGS=-g)')
    ap.add_argument('--allowlist', help='findings that may stay, default none')
    ap.add_argument('--update', action='store_true', help='rewrite the allowlist with the findings, keeping comments')
    ap.add_argument('--keys', action='store_true', help='print the key of every finding and nothing else')
    ap.add_argument('--verbose', action='store_true', help='also print the allowlisted findings and the notes')
    ap.add_argument('--query', action='append', metavar='FUNCTION',
                    help='print what the function reaches (heap and calls through a register) and nothing else')
    ap.add_argument('--root', default=os.path.normpath(os.path.join(here, '..', '..')),
                    help='the Duet3Expansion directory the firmware was compiled in, default ../..')
    prefix = os.environ.get('CROSS_COMPILE', 'arm-none-eabi-')
    for tool in ('objdump', 'nm', 'addr2line'):
        ap.add_argument('--' + tool, default=prefix + tool, help=f'default {prefix}{tool}')
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    tools = {'objdump': args.objdump, 'nm': args.nm, 'addr2line': args.addr2line}
    if args.query:
        for elf in args.elf:
            print(f'== {os.path.basename(elf)}')
            print('\n'.join(query(Program(elf, args.objdump, args.nm), args.query)))
        return 0
    found, notes = collections.defaultdict(list), []
    for elf in args.elf:
        result, elf_notes = scan(elf, root, tools)
        for key, sites in result.items():
            found[key].extend(sites)
        notes += [f'note: {os.path.basename(elf)}: {n}' for n in elf_notes]

    if args.keys:
        print('\n'.join(sorted(found)))
        return 0

    header, allowed = read_allowlist(args.allowlist)
    if args.update:
        with open(args.allowlist, 'w') as f:
            for line in header:
                f.write(line + '\n')
            width = max((len(k) for k in found), default=0)
            for key in sorted(found):
                f.write(f'{key:{width}}  # {allowed.get(key) or describe(found[key])[0]}\n')
        print(f'{args.allowlist}: {len(found)} findings ({len(set(found) - set(allowed))} added, '
              f'{len(set(allowed) - set(found))} removed)')
        return 0

    if args.verbose:
        print('\n'.join(notes))
    new = sorted(set(found) - set(allowed))
    for key in new:
        first, text = describe(found[key])
        if key.endswith('-> indirect call'):
            print(f'{first}: error: call through a register inside a protected region: {key}: {text}. Make sure that its '
                  f'targets do not reach the heap and add the key to {args.allowlist or "the allowlist"} with the reason')
        else:
            print(f'{first}: error: heap reached inside a protected region: {key}: {text}. Allocate and free outside the '
                  f'region, or add the key to {args.allowlist or "the allowlist"} with the reason')
    if args.verbose:
        for key in sorted(set(found) & set(allowed)):
            first, text = describe(found[key])
            print(f'{first}: allowlisted: {key}: {text}')
    for key in sorted(set(allowed) - set(found)):
        print(f'note: {key} is in the allowlist but not found in the checked firmware')
    print(f'{len(found)} findings in {len(args.elf)} ELF file(s), {len(found) - len(new)} allowlisted, {len(new)} new')
    return 1 if new else 0


if __name__ == '__main__':
    sys.exit(main())
