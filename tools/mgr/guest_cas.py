#!/usr/bin/env python3
"""(Prototype: from tuancapoo/mgr-nx-deko, GPL-3.0; the run-time barrier switch removed: no branch, so the
functions keep their control flow and the PGO profile still fits.)
Guest compare-and-swap without the legacy builtin's trailing barrier (post-codegen step of the "optimised" codegen
flavour; docs/enhancements.md, "Build-time"). Works on any ReXGlue port's generated code.

The generator writes a `stwcx.` / `stdcx.` as `__sync_bool_compare_and_swap(...)`. On AArch64 without LSE (the
Switch's Cortex-A57) GCC compiles that legacy builtin as a load-exclusive / store-exclusive loop FOLLOWED BY
`dmb ish`, because the __sync builtins promise a full barrier (GCC PR 65697). A `dmb` drains the store buffer: tens of
cycles on an A57, after every interlocked increment, decrement and exchange the game does (reference counts, lock
words, lock-free lists).

The guest never had that barrier: on PowerPC lwarx/stwcx. order nothing, and where the game needs ordering it has its
own lwsync / sync / eieio, which the generator translates separately (std::atomic_thread_fence). So this step
rewrites the call into REX_GUEST_CAS(...), which the generated pch defines as __atomic_compare_exchange_n with
sequentially consistent ordering: the same loop with acquire/release exclusives (ldaxr / stlxr) and no trailing
barrier. That is still stronger than what the PowerPC code had.

  - REX_GUEST_CAS_LEGACY defined: the old builtin (build-time off switch).
  - On AArch64 the byte g_rex_atomic_legacy_barrier (defined by the runtime, sdk/src/core/atomic_posix.cpp; the app
    sets it from its key) puts the `dmb ish` back at run time: the A/B on the console without a second build.
  - On x86 both forms are `lock cmpxchg`: the PC build tests the rewritten expression, not the barrier.

  python tools/guest_cas.py --gen app/generated/default [--check] [--undo]
Idempotent.
"""
import argparse
import os
import re
import sys

OLD = '__sync_bool_compare_and_swap('
NEW = 'REX_GUEST_CAS('
MARKER = '// rexglue ports: guest compare-and-swap without the trailing barrier (tools/guest_cas.py)'
MACROS = MARKER + '''
#ifndef REX_GUEST_CAS
#if defined(REX_GUEST_CAS_LEGACY)
#define REX_GUEST_CAS(p, e, d) __sync_bool_compare_and_swap((p), (e), (d))
#else
template <typename T, typename E, typename D>
[[gnu::always_inline]] static inline bool RexGuestCas(T* p, E e, D d) noexcept {
  T expected = static_cast<T>(e);
  const bool swapped = __atomic_compare_exchange_n(p, &expected, static_cast<T>(d), false, __ATOMIC_SEQ_CST,
                                                   __ATOMIC_SEQ_CST);
  return swapped;
}
#define REX_GUEST_CAS(p, e, d) RexGuestCas((p), (e), (d))
#endif
#endif
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--undo', action='store_true')
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not files or len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): guest_cas: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    old, new = (NEW, OLD) if a.undo else (OLD, NEW)
    sites = present = changed = 0
    for f in files:
        path = os.path.join(a.gen, f)
        text = open(path, encoding='utf-8').read()
        n = text.count(old)
        present += text.count(new)
        if n:
            sites += n
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(text.replace(old, new))
    pch = os.path.join(a.gen, pchs[0])
    text = open(pch, encoding='utf-8').read()
    if not a.undo and MARKER not in text:
        anchor = '#define REX_LOAD_U8(x)'
        if anchor not in text:
            sys.exit('ERROR (post-codegen patch): guest_cas: no REX_LOAD_U8 in %s: the generated pch changed' % pch)
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS + anchor, 1))
    print('guest_cas%s: %d compare-and-swap sites %s in %d files (%d already in that form)' %
          (' (check only)' if a.check else '', sites, 'restored' if a.undo else 'rewritten', changed, present))


if __name__ == '__main__':
    main()
