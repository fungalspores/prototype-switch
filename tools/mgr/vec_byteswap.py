#!/usr/bin/env python3
"""Guest vector loads and stores without a table lookup on AArch64 (post-codegen step of the "optimised" codegen
flavour; docs/enhancements.md, "Build-time"). Works on any ReXGlue port's generated code.

The context keeps each VMX register byte-reversed, so every lvx / stvx (and the aligned forms of the others) reverses
16 bytes. The generator writes that as the x86 idiom

    simde_mm_shuffle_epi8(<vector>, simde_mm_load_si128((simde__m128i*)VectorMaskL))

a pshufb with the mask 15, 14, ... 0: one cheap instruction on x86. On AArch64, simde turns pshufb into an AND of
the mask with 0x8F (pshufb's "high bit clears the byte" rule) and a TBL, and the mask is a load from a table the
compiler cannot fold (VectorMaskL is a writable array). So every guest vector load and store costs a mask load, an
AND and a table lookup (TBL with one table register: 5 cycles and 2 micro-ops on the Cortex-A57), and each function
keeps two vector registers for the mask and the 0x8F constant. This game's ELF has 38,399 TBL instructions in
recompiled functions against 121 REV64 and 462 EXT.

The same permutation is REV64 (reverse the bytes of each 64-bit half) followed by EXT #8 (swap the halves), and
written with those two intrinsics it is a permutation the compiler KNOWS. This step rewrites the expression into
REX_BSWAP128(<vector>), which the generated pch defines as
  - AArch64: vextq_u8(r, r, 8) with r = vrev64q_u8(v);
  - elsewhere, or with REX_BSWAP128_TABLE defined: the original expression (the PC build is unchanged).
What GCC makes of it (one recompiled file, the Switch flags, tools/one_file_ab.py): it folds the pair into a single
TBL with a constant mask from the literal pool, so the AND and the 0x8F constant are gone (vector ANDs 288 -> 52,
MOVI 170 -> 63), a reversal that meets another reversal or a constant shuffle is merged with it (a vector copied
from memory to memory is no longer reversed twice: TBL 446 -> 356), and the file has 1.7 % fewer instructions, 2.4 %
fewer loads and 1.3 % fewer stores.
sdk/tests/vec_byteswap_test.cpp checks the identity with simde's model of the two NEON instructions.

The unaligned forms (lvlx, lvrx, stvlx ...) index the mask table by the address, so they keep a table lookup:
    simde_mm_shuffle_epi8(<vector>, simde_mm_load_si128((simde__m128i*)&VectorMaskL[<row>]))
becomes REX_SHUFFLE_TABLE(<vector>, &VectorMaskL[<row>]): on AArch64 the TBL alone, without the AND with 0x8F. TBL
returns 0 for an index of 16 or more and pshufb for one with bit 7 set, and every byte of VectorMaskL and
VectorMaskR is 0..15 or 0xFF (sdk/tests/vmx_small_helpers_test.cpp checks the tables), so the two agree.

  python tools/vec_byteswap.py --gen app/generated/default [--check] [--undo]
Idempotent.
"""
import argparse
import os
import re
import sys

HEAD = 'simde_mm_shuffle_epi8('
TAIL = ', simde_mm_load_si128((simde__m128i*)VectorMaskL))'
NEW = 'REX_BSWAP128('
MARKER = '// rexglue ports: 16-byte reversal of guest vectors without a table lookup (tools/vec_byteswap.py)'
MACROS = MARKER + '''
#ifndef REX_BSWAP128
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(REX_BSWAP128_TABLE)
[[gnu::always_inline]] static inline simde__m128i RexByteReverse128(simde__m128i v) noexcept {
  const uint8x16_t r = vrev64q_u8(simde__m128i_to_neon_u8(v));
  return simde__m128i_from_neon_u8(vextq_u8(r, r, 8));
}
#define REX_BSWAP128(v) RexByteReverse128(v)
#else
#define REX_BSWAP128(v) simde_mm_shuffle_epi8((v), simde_mm_load_si128((simde__m128i*)VectorMaskL))
#endif
#endif
'''


TABLE_ROW = re.compile(r'^ simde_mm_load_si128\(\(simde__m128i\*\)(&VectorMask[LR]\[.*\])\)$')
NEW_TABLE = 'REX_SHUFFLE_TABLE('
MARKER_TABLE = '// rexglue ports: mask-table shuffles as a bare table lookup on AArch64 (tools/vec_byteswap.py)'
MACROS_TABLE = MARKER_TABLE + '''
#ifndef REX_SHUFFLE_TABLE
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(REX_BSWAP128_TABLE)
[[gnu::always_inline]] static inline simde__m128i RexShuffleTable(simde__m128i v, const uint8_t* row) noexcept {
  return simde__m128i_from_neon_u8(vqtbl1q_u8(simde__m128i_to_neon_u8(v), vld1q_u8(row)));
}
#define REX_SHUFFLE_TABLE(v, row) RexShuffleTable((v), (row))
#else
#define REX_SHUFFLE_TABLE(v, row) simde_mm_shuffle_epi8((v), simde_mm_load_si128((simde__m128i*)(row)))
#endif
#endif
'''


def rewrite_table(line):
    """Every `simde_mm_shuffle_epi8(X, simde_mm_load_si128((simde__m128i*)&VectorMaskL[row]))` becomes
    REX_SHUFFLE_TABLE(X, &VectorMaskL[row]) (VectorMaskR too)."""
    count = 0
    start = 0
    while True:
        i = line.find(HEAD, start)
        if i < 0:
            return line, count
        depth = 0
        j = i + len(HEAD) - 1
        comma = -1
        while j < len(line):
            c = line[j]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    break
            elif c == ',' and depth == 1:
                comma = j
            j += 1
        if j >= len(line) or comma < 0:
            return line, count
        m = TABLE_ROW.match(line[comma + 1:j])
        if m:
            line = line[:i] + NEW_TABLE + line[i + len(HEAD):comma] + ', ' + m.group(1) + line[j:]
            count += 1
            start = i + len(NEW_TABLE)
        else:
            start = i + len(HEAD)


def rewrite(line):
    """Every `simde_mm_shuffle_epi8(X, <the full-reversal mask>)` of the line becomes REX_BSWAP128(X)."""
    count = 0
    start = 0
    while True:
        i = line.find(HEAD, start)
        if i < 0:
            return line, count
        # The matching close parenthesis of this call.
        depth = 0
        j = i + len(HEAD) - 1
        while j < len(line):
            c = line[j]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if j >= len(line):
            return line, count  # a call that continues on the next line: left alone
        inner = line[i + len(HEAD):j + 1]  # the arguments and the close parenthesis
        if inner.endswith(TAIL):
            line = line[:i] + NEW + inner[:-len(TAIL)] + ')' + line[j + 1:]
            count += 1
            start = i + len(NEW)  # the argument may hold another shuffle
        else:
            start = i + len(HEAD)


def undo(line):
    count = 0
    start = 0
    while True:
        i = line.find(NEW, start)
        if i < 0:
            return line, count
        depth = 0
        j = i + len(NEW) - 1
        while j < len(line):
            c = line[j]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if j >= len(line):
            return line, count
        line = line[:i] + HEAD + line[i + len(NEW):j] + TAIL + line[j + 1:]
        count += 1
        start = i + len(HEAD)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--undo', action='store_true')
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not files or len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): vec_byteswap: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    sites = present = changed = tables = 0
    work = undo if a.undo else rewrite
    needle = NEW if a.undo else TAIL
    for f in files:
        path = os.path.join(a.gen, f)
        lines = open(path, encoding='utf-8').read().split('\n')
        n = 0
        for k, line in enumerate(lines):
            if not a.undo:
                present += line.count(NEW)
            if needle in line:
                lines[k], c = work(line)
                n += c
            if not a.undo and '&VectorMask' in lines[k] and HEAD in lines[k]:
                lines[k], c = rewrite_table(lines[k])
                tables += c
                n += c
        if n:
            sites += n
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    pch = os.path.join(a.gen, pchs[0])
    text = open(pch, encoding='utf-8').read()
    if not a.undo and MARKER not in text:
        anchor = '#define REX_LOAD_U8(x)'
        if anchor not in text:
            sys.exit('ERROR (post-codegen patch): vec_byteswap: no REX_LOAD_U8 in %s: the generated pch changed' % pch)
        text = text.replace(anchor, MACROS + anchor, 1)
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text)
    if not a.undo and MARKER_TABLE not in text:
        anchor = '#define REX_LOAD_U8(x)'
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS_TABLE + anchor, 1))
    print('vec_byteswap%s: %d 16-byte reversals %s in %d files (%d already in that form); %d mask-table shuffles as '
          'REX_SHUFFLE_TABLE' %
          (' (check only)' if a.check else '', sites - tables, 'restored' if a.undo else 'rewritten', changed, present,
           tables))


if __name__ == '__main__':
    main()
