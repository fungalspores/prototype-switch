#!/usr/bin/env python3
"""Constant 4-lane shuffles as one permutation on AArch64 (post-codegen step of the "optimised" codegen flavour;
docs/enhancements.md, "Build-time"). Patches the generated pch only. Works on any ReXGlue port.

The generator writes the VMX word shuffles (vspltw, vpermwi128, vrlimi128, the word rotations ...) as
simde_mm_shuffle_epi32(v, imm) and simde_mm_permute_ps(v, imm): one pshufd / vpermilps on x86. simde's NEON versions
build the result lane by lane:
  - simde_mm_shuffle_epi32: vmovq_n(lane a) then three vsetq_lane(vgetq_lane(...)), which GCC compiles to a DUP and
    three element-to-element INS in a dependency chain (3 cycles each on the Cortex-A57: 12 cycles for what x86 does
    in one) unless all four lanes are the same;
  - simde_mm_permute_ps: a C loop over the four lanes.
The compiler does not see these as a permutation, so it cannot merge them with the permutations around them either
(the byte reversal of a vector load, tools/vec_byteswap.py).

This step redefines both, on AArch64 only, with simde's own generic form (the one it uses for targets without a
native shuffle): SIMDE_SHUFFLE_VECTOR_, GCC's __builtin_shuffle with constant indices. GCC then picks the best single
instruction for each constant (DUP, EXT, REV, ZIP / UZP / TRN, or one TBL with a literal mask) and folds neighbouring
constant permutations into one. The lanes are the same by definition (lane i = a[(imm >> 2i) & 3]):
sdk/tests/vec_byteswap_test.cpp's sibling check is not needed for an index formula simde already uses elsewhere.
REX_SIMDE_SHUFFLES defined keeps simde's NEON versions (the off switch). x86 builds are not touched.

  python tools/pch_neon_shuffles.py <generated pch> [--check]
Idempotent.
"""
import sys

MARKER = '// rexglue ports: constant word shuffles as one permutation on AArch64 (tools/pch_neon_shuffles.py)'
BLOCK = MARKER + '''
#if (defined(__aarch64__) || defined(_M_ARM64)) && defined(SIMDE_SHUFFLE_VECTOR_) && !defined(REX_SIMDE_SHUFFLES)
#undef simde_mm_shuffle_epi32
#define simde_mm_shuffle_epi32(a, imm8)                                                        \\
  (__extension__({                                                                             \\
    const simde__m128i_private rex_shuffle_a_ = simde__m128i_to_private(a);                    \\
    simde__m128i_from_private((simde__m128i_private){                                          \\
        .i32 = SIMDE_SHUFFLE_VECTOR_(32, 16, (rex_shuffle_a_).i32, (rex_shuffle_a_).i32,       \\
                                     ((imm8)) & 3, ((imm8) >> 2) & 3, ((imm8) >> 4) & 3,       \\
                                     ((imm8) >> 6) & 3)});                                     \\
  }))
#undef simde_mm_permute_ps
#define simde_mm_permute_ps(a, imm8)                                                           \\
  (__extension__({                                                                             \\
    const simde__m128_private rex_permute_a_ = simde__m128_to_private(a);                      \\
    simde__m128_from_private((simde__m128_private){                                            \\
        .f32 = SIMDE_SHUFFLE_VECTOR_(32, 16, (rex_permute_a_).f32, (rex_permute_a_).f32,       \\
                                     ((imm8)) & 3, ((imm8) >> 2) & 3, ((imm8) >> 4) & 3,       \\
                                     ((imm8) >> 6) & 3)});                                     \\
  }))
#endif
'''
ANCHOR = '#define REX_LOAD_U8(x)'


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        sys.exit(__doc__)
    check = '--check' in sys.argv
    text = open(args[0], encoding='utf-8').read()
    if MARKER in text:
        print('pch_neon_shuffles: already in %s' % args[0])
        return
    if ANCHOR not in text:
        sys.exit('ERROR (post-codegen patch): pch_neon_shuffles: no REX_LOAD_U8 in %s: the generated pch changed'
                 % args[0])
    if not check:
        open(args[0], 'w', encoding='utf-8', newline='\n').write(text.replace(ANCHOR, BLOCK + ANCHOR, 1))
    print('pch_neon_shuffles%s: simde_mm_shuffle_epi32 and simde_mm_permute_ps as constant permutations on AArch64'
          % (' (check only)' if check else ''))


if __name__ == '__main__':
    main()
