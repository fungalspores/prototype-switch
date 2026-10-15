"""Make the ReXGlue 0.10.0 codegen output build with devkitA64 GCC (idempotent).

Inserts the GCC branch of DEFINE_REX_FUNC from the masseffect-nx SDK template (BSD-3-Clause):
GCC drops attributes placed before extern "C", so the weak alias is never emitted.
Same step as Mercenaries2-NX tools/prepare_switch_codegen.py; game-specific patches come later.
"""
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[1]
target = root / 'project/generated/default/prototype2_pch.h'
gcc_branch = (
    '#elif defined(__GNUC__) && !defined(__clang__)\n'
    '// GCC rejects attributes ahead of extern "C" and drops them silently (masseffect-nx template).\n'
    '#define DEFINE_REX_FUNC(name)                                       \\\n'
    '  extern "C" REX_FUNC(name)                                         \\\n'
    '      __attribute__((alias("__imp__" #name), weak, noinline));      \\\n'
    '  REX_EXTERN(__imp__##name)\n')
text = target.read_text(encoding='utf-8')
if '#elif defined(__GNUC__) && !defined(__clang__)\n// GCC rejects' in text:
    print('GCC alias support already present')
else:
    anchor = '#else\n#define DEFINE_REX_FUNC(name)'
    if text.count(anchor) != 1:
        sys.exit('Unexpected generated macro; refusing to patch')
    target.write_text(text.replace(anchor, gcc_branch + anchor), encoding='utf-8', newline='\n')
    print('Applied GCC alias declaration fix')

# --- Prototype 2: lock-free "interrupt disable" around lwarx/stwcx (run17 profile) -------------
# The game brackets its atomics with mfmsr / mtmsrd r13,1 ... mtmsrd rX,1. ReXGlue turns those into
# REX_CHECK_GLOBAL_LOCK / REX_ENTER_GLOBAL_LOCK / REX_LEAVE_GLOBAL_LOCK = one process-wide mutex, so
# every atomic in every guest thread serialises on it (main game thread ~55% waiting, mostly here).
# Inside these regions there is only a reservation load and a CAS store (checked below on every
# build), which are atomic by themselves, so the mutex is dropped; the fences stay.
import glob, re
gen = root / 'project/generated/default'
bad = []
for f in sorted(glob.glob(str(gen / 'prototype2_recomp.*.cpp'))):
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
        if (j < 0 or (0 <= k < j) or '(ctx, base)' in body or 'REX_STORE' in body or
                'REX_LOAD' in body or 'ctx.reserved' not in body):
            bad.append(f'{Path(f).name}@{i}')
text = target.read_text(encoding='utf-8')
marker = '// P2: lock-free msr regions'
if bad:
    sys.exit(f'Global-lock regions that are not plain atomics ({len(bad)}), e.g. {bad[:3]}; '
             'refusing the lock-free msr patch')
if 'REX_ENTER_ADDR_LOCK(a)' in text:
    print('Striped msr region locks already applied')
else:
    if marker in text:
        # older lock-free version: replace from the marker to the end of the LEAVE macro
        a0 = text.index(marker); b0 = text.index('#define REX_LEAVE_GLOBAL_LOCK()', a0); b0 = text.index('\n', b0) + 1
        text = text[:a0] + '#define REX_CHECK_GLOBAL_LOCK() x\n#define REX_ENTER_GLOBAL_LOCK() x\n#define REX_LEAVE_GLOBAL_LOCK() do { x } while (0)\n' + text[b0:]
    a = text.index('#define REX_CHECK_GLOBAL_LOCK()')
    b = text.index('} while (0)', text.index('#define REX_LEAVE_GLOBAL_LOCK()')) + len('} while (0)\n')
    old = text[a:b]
    new = (marker + ' (tools/prepare_switch_codegen.py; every region is lwarx..stwcx only)\n'
           '/* rex_p2_msr_lock: 0 = no lock, 1 = a lock per address stripe, 2 = the SDK global mutex, 3 = one guest-only spinlock, 4 = the SDK global mutex taken by spinning on try_lock first.\n'
           '   Set from p2_msr_lock_mode at startup (prototype2_app.h). run21: fully lock-free crashed level\n'
           '   loads (bad vtable from a lock-free queue); stripes keep regions on one address serialised. */\n'
           'inline int rex_p2_msr_lock = 2;\n'
           'inline std::mutex& rex_p2_addr_lock_(uint32_t a) { static std::mutex m[256]; return m[(a >> 3) & 255]; }\n'
           'inline thread_local std::mutex* rex_p2_held_lock_ = nullptr;\n'
           '/* mode 2 = exactly the SDK original: mfmsr also takes the global lock for a moment. */\n#define REX_CHECK_GLOBAL_LOCK() ([&]() -> u64 { if (rex_p2_msr_lock == 2) { auto lock_ = rex::thread::global_critical_region::AcquireDirect(); return ppc_global_lock_count_().load() ? 0 : 0x8000; } return 0x8000; }())\n'
           '#define REX_ENTER_GLOBAL_LOCK() do { } while (0)\n'
           '#include <thread>\n#if defined(__aarch64__)\n#define REX_P2_CPU_RELAX() __asm__ volatile(\"yield\" ::: \"memory\")\n#else\n#define REX_P2_CPU_RELAX() std::atomic_signal_fence(std::memory_order_seq_cst)\n#endif\ninline std::atomic<uint32_t> rex_p2_spin_{0};\ninline thread_local bool rex_p2_held_spin_ = false;\ninline void rex_p2_spin_lock_() { for (uint32_t n = 0;; ++n) { if (!rex_p2_spin_.load(std::memory_order_relaxed) && !rex_p2_spin_.exchange(1, std::memory_order_acquire)) return; if (n < 128) { __asm__ volatile("yield" ::: "memory"); } else { std::this_thread::yield(); } } }\ninline thread_local bool rex_p2_held_global_ = false;\ninline void rex_p2_global_lock_spin_() { auto& m = rex::thread::global_critical_region::mutex(); for (uint32_t n = 0; n < 256; ++n) { if (m.try_lock()) return; REX_P2_CPU_RELAX(); } m.lock(); }\n#define REX_ENTER_ADDR_LOCK(a) do { if (rex_p2_msr_lock == 5) { rex::thread::global_critical_region::mutex().lock(); rex_p2_held_global_ = true; } else if (rex_p2_msr_lock == 4) { rex_p2_global_lock_spin_(); rex_p2_held_global_ = true; } else if (rex_p2_msr_lock == 3) { rex_p2_spin_lock_(); rex_p2_held_spin_ = true; } else if (rex_p2_msr_lock == 1) { rex_p2_held_lock_ = &rex_p2_addr_lock_(a); rex_p2_held_lock_->lock(); } else if (rex_p2_msr_lock == 2) { rex::thread::global_critical_region::mutex().lock(); ppc_global_lock_count_().fetch_add(1); } } while (0)\n'
           '#define REX_LEAVE_GLOBAL_LOCK() do { if (rex_p2_held_global_) { rex_p2_held_global_ = false; rex::thread::global_critical_region::mutex().unlock(); } else if (rex_p2_held_spin_) { rex_p2_held_spin_ = false; rex_p2_spin_.store(0, std::memory_order_release); } else if (rex_p2_held_lock_) { rex_p2_held_lock_->unlock(); rex_p2_held_lock_ = nullptr; } else if (rex_p2_msr_lock == 2) { ppc_global_lock_count_().fetch_sub(1); rex::thread::global_critical_region::mutex().unlock(); } } while (0)\n\n')
    if old.count('#define') < 3:
        sys.exit('Unexpected global lock macros; refusing to patch')
    target.write_text(text.replace(old, new), encoding='utf-8', newline='\n')
    print('Applied lock-free msr regions')

# Per-address lock: insert REX_ENTER_ADDR_LOCK(ea) right after the reservation's address is computed.
pat = re.compile(r'REX_ENTER_GLOBAL_LOCK\(\);(\n\t// l[wd]arx [^\n]*\n\tea = [^;]+;)(?!\n\tREX_ENTER_(?:ADDR|HOT)_LOCK)')
changed = 0
for f in sorted(glob.glob(str(gen / 'prototype2_recomp.*.cpp'))):
    src = Path(f).read_text(encoding='utf-8', errors='replace')
    if src.count('REX_ENTER_GLOBAL_LOCK();') != len(re.findall(r'REX_ENTER_GLOBAL_LOCK\(\);\n\t// l[wd]arx', src)):
        sys.exit(f'{Path(f).name}: a global-lock region does not start with lwarx/ldarx; refusing')
    out, n = pat.subn(lambda m: 'REX_ENTER_GLOBAL_LOCK();' + m.group(1) + '\n\tREX_ENTER_ADDR_LOCK(ea);', src)
    if n:
        Path(f).write_text(out, encoding='utf-8', newline='\n'); changed += n
print(f'Per-address msr locks inserted: {changed}')

# --- Prototype 2: PowerPC barriers (run22 analysis) -----------------------------------------------
# ReXGlue 0.10 emits nothing for lwsync / sync / eieio (73 / 18 / 17 sites), not even a compiler barrier.
# The guest uses them to publish data between threads (store data; lwsync; store flag). On the Cortex-A57
# both the compiler and the CPU may reorder those plain accesses. With the old global mutex the frequent
# lock/unlock barriers hid this; lock-free msr regions exposed it (level-load crash in the job system).
barrier_for = {'lwsync': 'std::atomic_thread_fence(std::memory_order_acq_rel);',
               'sync': 'std::atomic_thread_fence(std::memory_order_seq_cst);',
               'eieio': 'std::atomic_thread_fence(std::memory_order_release);'}
bpat = re.compile(r'\t// (lwsync|sync|eieio) \n(?!\tstd::atomic_thread_fence\(std::memory_order_(?:acq_rel|seq_cst|release)\); /\* P2 barrier \*/)')
nb = 0
for f in sorted(glob.glob(str(gen / 'prototype2_recomp.*.cpp'))):
    src = Path(f).read_text(encoding='utf-8', errors='replace')
    out, n = bpat.subn(lambda m: m.group(0) + '\t' + barrier_for[m.group(1)] + ' /* P2 barrier */\n', src)
    if n:
        Path(f).write_text(out, encoding='utf-8', newline='\n'); nb += n
print(f'PowerPC barriers inserted: {nb}')

# --- Prototype 2: mode 5 = global lock except in the hottest job-system primitives (run28 wait profile) ----
# In these functions the msr regions use REX_ENTER_HOT_LOCK(ea): no lock in mode 5, the normal lock otherwise.
HOT = ['sub_822EC7A0', 'sub_8222FFA0', 'sub_8222F858', 'sub_82293D80', 'sub_82230340', 'sub_82238D00',
       'sub_82CA91D8', 'sub_821D0EC0', 'sub_822A7690']
nh = 0
for f in sorted(glob.glob(str(gen / 'prototype2_recomp.*.cpp'))):
    src = Path(f).read_text(encoding='utf-8', errors='replace')
    changed = False
    for fn in HOT:
        a = src.find('DEFINE_REX_FUNC(' + fn + ')')
        if a < 0:
            continue
        b = src.find('\nDEFINE_REX_FUNC(', a + 10)
        b = len(src) if b < 0 else b
        body = src[a:b]
        nb = body.replace('REX_ENTER_ADDR_LOCK(ea);', 'REX_ENTER_HOT_LOCK(ea);')
        if nb != body:
            nh += body.count('REX_ENTER_ADDR_LOCK(ea);')
            src = src[:a] + nb + src[b:]
            changed = True
    if changed:
        Path(f).write_text(src, encoding='utf-8', newline='\n')
text = target.read_text(encoding='utf-8')
if 'REX_ENTER_HOT_LOCK' not in text:
    text = text.rstrip('\n') + ('\n/* mode 5: the hot job-system functions skip the lock, everything else uses the SDK global mutex. */\n'
        '#define REX_ENTER_HOT_LOCK(a) do { if (rex_p2_msr_lock != 5) { REX_ENTER_ADDR_LOCK(a); } } while (0)\n')
    target.write_text(text, encoding='utf-8', newline='\n')
print(f'Hot msr regions marked: {nh}')

# Canonicalize old build output: earlier versions reinserted ADDR before an
# existing HOT lock, then renamed ADDR to HOT, acquiring a recursive mutex twice
# in mode 2 and releasing only once. Keep exactly one entry per reservation.
dedup = re.compile(r'(\n\tREX_ENTER_(?:ADDR|HOT)_LOCK\(ea\);)(?:\n\tREX_ENTER_(?:ADDR|HOT)_LOCK\(ea\);)+')
removed = 0
for f in sorted(gen.glob('prototype2_recomp.*.cpp')):
    src = f.read_text(encoding='utf-8')
    out, n = dedup.subn(lambda m: m.group(1), src)
    if n:
        f.write_text(out, encoding='utf-8', newline='\n')
        removed += n
    starts = out.count('REX_ENTER_GLOBAL_LOCK();')
    entries = out.count('REX_ENTER_ADDR_LOCK(ea);') + out.count('REX_ENTER_HOT_LOCK(ea);')
    if starts != entries:
        sys.exit(f'{f.name}: {starts} regions but {entries} lock entries; refusing unsafe output')
print(f'Canonicalized duplicate lock entries: {removed} regions')

# Global return-boundary instrumentation, opt-in at compile and runtime.
# Keep normal builds identical at this boundary; never suppress original alignment checks.
stack_marker='// P2 generated stack audit (opt-in diagnostic)'
text=target.read_text(encoding='utf-8')
if stack_marker not in text:
    text += '\n'+stack_marker+'\n'+'''#if defined(__SWITCH__) && defined(P2_GUEST_STACK_AUDIT) && P2_GUEST_STACK_AUDIT
#if defined(REXGLUE_PROFILE_GUEST_FUNCTIONS) && defined(REXGLUE_ENABLE_PROFILING)
#error P2 stack audit requires guest Tracy zones off
#endif
#include "guest_stack_audit.h"
#undef REX_FUNC_PROLOGUE
#define REX_FUNC_PROLOGUE() do { if (((size_t)base & 0x1F) != 0) __builtin_unreachable(); } while (0); P2GuestStackScope p2_guest_stack_scope_{__func__, ctx.r1.u64, ctx.lr, &ctx}
#endif
'''
    target.write_text(text,encoding='utf-8',newline='\n')
print('Generated stack audit: opt-in boundary present')

# Targeted lifecycle history; compile-time disabled in ordinary builds.
sys.path.insert(0, str(root / "tools"))
from prepare_task_lifetime_audit import prepare as prepare_task_lifetime
prepare_task_lifetime()

# The queue drain keeps r31 after a deleting destructor. A wait timeout skips
# dequeue and reuses that dead object. Reset only the consumed local pointer.
fix_marker='/* P2 consumed task timeout fix */'
# Codegen with non_volatile_as_local/ctr_as_local (project/perf_locals.toml) spells r31/ctr as locals.
_task_forms = [('\tctx.lr = 0x822BAB38;\n\tREX_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n', 'ctx.r31.u64 = 0;'),
               ('\tctx.lr = 0x822BAB38;\n\tREX_CALL_INDIRECT_FUNC(ctr.u32);\n', 'r31.u64 = 0;')]
_task_done = False
for f in sorted(glob.glob(str(gen / 'prototype2_recomp.*.cpp'))):
    src = Path(f).read_text(encoding='utf-8')
    for anchor, reset in _task_forms:
        if src.count(anchor) == 1:
            if fix_marker not in src:
                src = src.replace(anchor, anchor + '#if defined(__SWITCH__) || defined(P2_TASK_TIMEOUT_TEST)\n'
                                  '\textern bool p2_task_pump_timeout_fix_enabled;\n\tif (p2_task_pump_timeout_fix_enabled) '
                                  + reset + ' ' + fix_marker + '\n#endif\n')
                Path(f).write_text(src, encoding='utf-8', newline='\n')
            _task_done = True
            break
    if _task_done:
        break
if not _task_done:
    sys.exit('Task-pump virtual call changed; inspect before applying timeout fix')
print('Consumed task timeout fix: opt-in reset after destructor')

# Run last: earlier semantic/instrumentation passes inspect original call shapes.
from prepare_direct_calls import prepare as prepare_direct_calls
prepare_direct_calls()

from prepare_arm_cpu import prepare as prepare_arm_cpu
prepare_arm_cpu()

# --- Prototype 2: hook the game's main simulation loop to raise its thread priority (switch_sim_priority.cpp) ---
sim = 'sub_821A84F8'
for f in sorted(glob.glob(str(gen / 'prototype2_recomp.*.cpp'))):
    src = Path(f).read_text(encoding='utf-8', errors='replace')
    a = src.find('DEFINE_REX_FUNC(' + sim + ') {\n\tREX_FUNC_PROLOGUE();\n')
    if a < 0:
        continue
    head = 'DEFINE_REX_FUNC(' + sim + ') {\n\tREX_FUNC_PROLOGUE();\n'
    if 'REX_P2_SIM_HOOK();' not in src[a:a + 300]:
        src = src[:a] + head + '\tREX_P2_SIM_HOOK();\n' + src[a + len(head):]
        Path(f).write_text(src, encoding='utf-8', newline='\n')
        print('Simulation thread hook inserted')
    break
text = target.read_text(encoding='utf-8')
if 'REX_P2_SIM_HOOK' not in text:
    text = text.rstrip('\n') + ('\n#if defined(__SWITCH__)\nextern "C" void p2_on_sim_thread_start();\n'
        '#define REX_P2_SIM_HOOK() p2_on_sim_thread_start()\n#else\n#define REX_P2_SIM_HOOK() do { } while (0)\n#endif\n')
    target.write_text(text, encoding='utf-8', newline='\n')

# --- Prototype 2: guest D3D flip scheduling (sub_82C746D8) -> immediate flips (switch_sim_priority.cpp) ---
# The game's swap handler queues the flip for a later vblank (present interval / "late in the period" check);
# with frames of 35-60 ms of work this rounds every frame up to 50/67/83 ms. The hook may replace the target.
flip = 'sub_82C746D8'
_flip_forms = [
    ('loc_82C747F0:\n\t// lwz r10,16952(r31)\n\tctx.r10.u64 = REX_LOAD_U32(ctx.r31.u32 + 16952);\n',
     '\tctx.r11.u64 = p2_flip_target(ctx.r11.u32, ctx.r10.u32, ctx.r29.u32, ctx.r27.u32, REX_LOAD_U32(ctx.r31.u32 + 16960));\n'),
    ('loc_82C747F0:\n\t// lwz r10,16952(r31)\n\tctx.r10.u64 = REX_LOAD_U32(r31.u32 + 16952);\n',
     '\tctx.r11.u64 = p2_flip_target(ctx.r11.u32, ctx.r10.u32, r29.u32, r27.u32, REX_LOAD_U32(r31.u32 + 16960));\n')]
for f in sorted(glob.glob(str(gen / 'prototype2_recomp.*.cpp'))):
    src = Path(f).read_text(encoding='utf-8', errors='replace')
    if 'DEFINE_REX_FUNC(' + flip + ') {' not in src:
        continue
    if 'p2_flip_target(' not in src:
        for flip_anchor, line in _flip_forms:
            if src.count(flip_anchor) == 1:
                src = src.replace(flip_anchor, flip_anchor + line)
                break
        else:
            raise SystemExit('flip hook anchor not found exactly once')
        decl = 'extern "C" uint32_t p2_flip_target(uint32_t target, uint32_t now, uint32_t interval, uint32_t threshold, uint32_t last_flip);\n'
        b = src.find('DEFINE_REX_FUNC(')
        src = src[:b] + decl + src[b:]
        Path(f).write_text(src, encoding='utf-8', newline='\n')
        print('Flip hook inserted in', Path(f).name)
    break

# --- Prototype 2: internal render size (WORKLOG #107). sub_82258D70 fills the renderer's init parameters with a
# hard-coded 1200x672 (scene targets; half/quarter-size effect targets are derived from it by the game). The size
# comes from p2_render_width/p2_render_height (switch_quality.cpp, cvars p2_render_width/_height, default
# 1200/672); the presenter scales the smaller frontbuffer to the screen.
_rs = 'sub_82258D70'
for f in sorted(glob.glob(str(gen / 'prototype2_recomp.*.cpp'))):
    src = Path(f).read_text(encoding='utf-8', errors='replace')
    head = 'DEFINE_REX_FUNC(' + _rs + ') {'
    if head not in src:
        continue
    if 'p2_render_width()' not in src:
        b = src.index(head)
        e = src.index('\nDEFINE_REX_FUNC(', b + 1) if '\nDEFINE_REX_FUNC(' in src[b + 1:] else len(src)
        body = src[b:e]
        if body.count('\tctx.r9.s64 = 1200;\n') != 1 or body.count('\tctx.r8.s64 = 672;\n') != 1:
            raise SystemExit('render size constants not found exactly once in ' + _rs)
        body = body.replace('\tctx.r9.s64 = 1200;\n', '\tctx.r9.s64 = p2_render_width();\n')
        body = body.replace('\tctx.r8.s64 = 672;\n', '\tctx.r8.s64 = p2_render_height();\n')
        decl = 'extern "C" int64_t p2_render_width();\nextern "C" int64_t p2_render_height();\n'
        src = src[:b] + decl + body + src[e:]
        Path(f).write_text(src, encoding='utf-8', newline='\n')
        print('Render size hook inserted in', Path(f).name)
    break

# --- Prototype 2: dcbt -> host prefetch (WORKLOG #82). ReXGlue drops the guest's cache-touch hints; the
# hot loops (skinning reads 256 bytes ahead, copies, audio) were written around them. A prefetch never faults.
import re as _re
_dcbt = _re.compile(r'^(\t// dcbt (r\d+),(r\d+)\n)(?!\t__builtin_prefetch)', _re.M)
def _dcbt_sub(m):
    ra, rb = m.group(2), m.group(3)
    ea = f'ctx.{rb}.u32' if ra == 'r0' else f'ctx.{ra}.u32 + ctx.{rb}.u32'
    return m.group(1) + f'\t__builtin_prefetch(REX_RAW_ADDR((uint32_t)({ea})));\n'
_n = 0
for f in sorted(glob.glob(str(gen / 'prototype2_recomp.*.cpp'))):
    src = Path(f).read_text(encoding='utf-8', errors='replace')
    new, k = _dcbt.subn(_dcbt_sub, src)
    if k:
        parts = _re.split(r'(?=^DEFINE_REX_FUNC\()', new, flags=_re.M)
        for i, part in enumerate(parts):
            if '__builtin_prefetch(REX_RAW_ADDR' in part and ('p2_leaf_r' in part or 'PPCRegister r' in part):
                def _leaf(m, part=part):
                    t = m.group(0)
                    for r in _re.findall(r'ctx\.(r\d+)\.u32', t):
                        if 'p2_leaf_' + r in part:
                            t = t.replace('ctx.' + r + '.u32', 'p2_leaf_' + r + '.u32')
                        elif '\tPPCRegister ' + r + '{};' in part:  # codegen register locals (perf_locals.toml)
                            t = t.replace('ctx.' + r + '.u32', r + '.u32')
                    return t
                parts[i] = _re.sub(r'__builtin_prefetch\(REX_RAW_ADDR\(\(uint32_t\)\([^;]*\)\)\);', _leaf, part)
        new = ''.join(parts)
    if k:
        Path(f).write_text(new, encoding='utf-8', newline='\n')
        _n += k
print('dcbt prefetches inserted:', _n)
