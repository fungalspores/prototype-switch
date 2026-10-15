"""Prototype (Switch): speed patches on the generated code, after Prototype 2 NX tools/prepare_switch_codegen.py
(penettrator, GPL-3.0). Idempotent; run after every codegen.

1. msr regions. The game brackets every lwarx/stwcx. with mfmsr / mtmsrd r13,1 ... mtmsrd rX,1 ("interrupts
   off"). ReXGlue maps that to one process-wide mutex (REX_ENTER/LEAVE_GLOBAL_LOCK), and mfmsr even takes it for
   a moment (REX_CHECK_GLOBAL_LOCK), so every atomic of every guest thread serialises on it. Each region holds
   only the reservation load and the CAS store (checked here: refuses otherwise), which are atomic by
   themselves. Mode (rex_proto_msr_lock, cvar proto_msr_lock_mode): 0 = no lock, 1 = a lock per address stripe
   (regions on the same word stay serialised, like on the console), 2 = the SDK original.
   P2 notes: fully lock-free crashed its level loads; per-address stripes did not.
"""
import glob
import re
import sys
from pathlib import Path

GEN = Path(sys.argv[1] if len(sys.argv) > 1 else 'generated/default')
pch = GEN / 'prototype_pch.h'
text = pch.read_text(encoding='utf-8')

files = sorted(glob.glob(str(GEN / 'prototype_recomp.*.cpp')))

# --- check every region is "ea = ...; reserved = load; ...; CAS" with no guest call/load/store -------------
bad = []
for f in files:
    s = Path(f).read_text(encoding='utf-8', errors='replace')
    pos = 0
    while True:
        i = s.find('REX_ENTER_GLOBAL_LOCK();', pos)
        if i < 0:
            break
        pos = i + 10
        j = s.find('REX_LEAVE_GLOBAL_LOCK();', i)
        k = s.find('DEFINE_REX_FUNC', i)
        body = s[i:j] if j >= 0 else ''
        if (j < 0 or (0 <= k < j) or '(ctx, base)' in body or 'REX_STORE' in body or 'REX_LOAD' in body
                or 'ctx.reserved' not in body):
            bad.append(f'{Path(f).name}@{i}')
if bad:
    sys.exit(f'msr regions that are not plain atomics ({len(bad)}), e.g. {bad[:3]}; refusing')

# --- the lock goes after the reservation address is known: ENTER_GLOBAL_LOCK -> ENTER_ADDR_LOCK(ea) -------
moved = 0
pat = re.compile(r'\tREX_ENTER_GLOBAL_LOCK\(\);\n((?:\t//[^\n]*\n)*)(\tea = [^\n]*\n)')
for f in files:
    s = Path(f).read_text(encoding='utf-8', errors='replace')
    out, n = pat.subn(lambda m: m.group(1) + m.group(2) + '\tREX_ENTER_ADDR_LOCK(ea);\n', s)
    if n:
        Path(f).write_text(out, encoding='utf-8', newline='\n')
        moved += n
left = sum(Path(f).read_text(encoding='utf-8', errors='replace').count('REX_ENTER_GLOBAL_LOCK();') for f in files)
if left:
    sys.exit(f'{left} regions still use REX_ENTER_GLOBAL_LOCK (unexpected shape); refusing')
entries = sum(Path(f).read_text(encoding='utf-8', errors='replace').count('REX_ENTER_ADDR_LOCK(ea);') for f in files)
leaves = sum(Path(f).read_text(encoding='utf-8', errors='replace').count('REX_LEAVE_GLOBAL_LOCK();') for f in files)
# A compare-and-swap region has two exits (match / no match), each with its own LEAVE.
if leaves < entries:
    sys.exit(f'{entries} lock entries but only {leaves} leaves; refusing')

marker = '// PROTO: msr region locks'
if marker not in text:
    a = text.index('#define REX_CHECK_GLOBAL_LOCK()')
    b = text.index('} while (0)', text.index('#define REX_LEAVE_GLOBAL_LOCK()')) + len('} while (0)\n')
    new = (marker + ' (tools/prepare_proto_codegen.py; every region is lwarx..stwcx. only)\n'
           '/* rex_proto_msr_lock: 0 = none, 1 = one lock per address stripe, 2 = the SDK global mutex.\n'
           '   Set from proto_msr_lock_mode at startup (src/proto_speed.cpp). */\n'
           'inline int rex_proto_msr_lock = 1;\n'
           'inline std::mutex& rex_proto_addr_lock_(uint32_t a) { static std::mutex m[256]; return m[(a >> 3) & 255]; }\n'
           'inline thread_local std::mutex* rex_proto_held_lock_ = nullptr;\n'
           '#define REX_CHECK_GLOBAL_LOCK() ([&]() -> u64 { if (rex_proto_msr_lock == 2) { auto lock_ = '
           'rex::thread::global_critical_region::AcquireDirect(); return ppc_global_lock_count_().load() ? 0 : 0x8000; } '
           'return 0x8000; }())\n'
           '#define REX_ENTER_GLOBAL_LOCK() do { } while (0)\n'
           '#define REX_ENTER_ADDR_LOCK(a) do { if (rex_proto_msr_lock == 1) { rex_proto_held_lock_ = '
           '&rex_proto_addr_lock_(a); rex_proto_held_lock_->lock(); } else if (rex_proto_msr_lock == 2) { '
           'rex::thread::global_critical_region::mutex().lock(); ppc_global_lock_count_().fetch_add(1); } } while (0)\n'
           '#define REX_LEAVE_GLOBAL_LOCK() do { if (rex_proto_held_lock_) { rex_proto_held_lock_->unlock(); '
           'rex_proto_held_lock_ = nullptr; } else if (rex_proto_msr_lock == 2) { ppc_global_lock_count_().fetch_sub(1); '
           'rex::thread::global_critical_region::mutex().unlock(); } } while (0)\n')
    text = text[:a] + new + text[b:]
    pch.write_text(text, encoding='utf-8', newline='\n')
print(f'msr regions: {entries} per-address locks ({moved} moved now)')
