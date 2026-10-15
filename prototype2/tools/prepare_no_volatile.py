"""Guest loads/stores without `volatile` (WORKLOG #82), as Mass Effect NX tools/pch_no_volatile.py (its t153:
+15-20 % fps, no hang). volatile kept the compiler from keeping guest values in registers, pairing or scheduling
accesses. MMIO macros (REX_MM_*) keep volatile.

Hang guard (Prototype 2 has register-local leaf functions, P2_LEAF_LOCALS): a loop with no guest store, no call
and no fence could poll memory with nothing forcing a reload once loads are not volatile. Every backward-goto
region of a function that contains none of those keeps volatile loads (REX_LOAD_V_*). Idempotent.
"""
import glob, re, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / 'project' / 'generated' / 'default'

def patch_pch():
    p = GEN / 'prototype2_pch.h'
    s = p.read_text(encoding='utf-8')
    if 'REX_GUEST_VOLATILE' in s:
        return False
    out, changed = [], 0
    for line in s.split('\n'):
        if line.startswith(('#define REX_LOAD_U', '#define REX_STORE_U')):
            n = line.replace('(volatile u', '(REX_GUEST_VOLATILE u')
            changed += n != line
            line = n
        out.append(line)
    if changed != 8:
        raise SystemExit(f'pch: expected 8 guest load/store macros, changed {changed}')
    s = '\n'.join(out)
    s = s.replace('#define REX_LOAD_U8(x)', '''// Prototype 2: guest loads/stores without volatile (tools/prepare_no_volatile.py). Define
// REX_GUEST_VOLATILE as volatile to restore the generated behaviour. REX_LOAD_V_* stay volatile: loops
// that poll memory with no store, call or fence in them use those.
#ifndef REX_GUEST_VOLATILE
#define REX_GUEST_VOLATILE
#endif
#define REX_LOAD_V_U8(x) (*(volatile u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_V_U16(x) __builtin_bswap16(*(volatile u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_V_U32(x) __builtin_bswap32(*(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_V_U64(x) __builtin_bswap64(*(volatile u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U8(x)''', 1)
    p.write_text(s, encoding='utf-8', newline='\n')
    return True

SAFE = ('REX_STORE_', 'REX_MM_STORE', '(ctx, base)', 'ctx, base)', '__asm', 'asm volatile', '__sync_',
        '__atomic', 'REX_ENTER_', 'REX_LEAVE_', 'REX_P2_CPU_RELAX', 'P2_VMX_STORE', 'simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR',
        'simde_mm_store_ps((float*)REX_RAW_ADDR', 'memcpy', 'memset', 'std::atomic', 'REX_CALL_INDIRECT')
LABEL = re.compile(r'^(loc_[0-9A-F]+):', re.M)
GOTO = re.compile(r'goto (loc_[0-9A-F]+);')

def patch_function(body):
    lines = body.split('\n')
    where = {}
    for i, l in enumerate(lines):
        m = LABEL.match(l)
        if m: where[m.group(1)] = i
    marks = 0
    for j, l in enumerate(lines):
        for target in GOTO.findall(l):
            i = where.get(target)
            if i is None or i > j:
                continue
            region = lines[i:j + 1]
            if any(any(k in r for k in SAFE) for r in region):
                continue
            for k in range(i, j + 1):
                if 'REX_LOAD_U' in lines[k]:
                    lines[k] = lines[k].replace('REX_LOAD_U', 'REX_LOAD_V_U')
                    marks += 1
    return '\n'.join(lines), marks

def main():
    pch = patch_pch()
    total = files = 0
    for f in sorted(glob.glob(str(GEN / 'prototype2_recomp.*.cpp'))):
        s = Path(f).read_text(encoding='utf-8', errors='replace')
        parts = re.split(r'(?=^DEFINE_REX_FUNC\()', s, flags=re.M)
        n = 0
        for i, p in enumerate(parts):
            if 'goto loc_' in p:
                parts[i], k = patch_function(p)
                n += k
        if n:
            Path(f).write_text(''.join(parts), encoding='utf-8', newline='\n')
            total += n; files += 1
    print(f'no-volatile: pch {"patched" if pch else "already"}; {total} polling-loop loads kept volatile in {files} files')

if __name__ == '__main__':
    main()
