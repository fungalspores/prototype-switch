// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2: the game's vertex skinning routine (sub_829F5978 with its constant helper sub_829F4C58) rewritten
// per vertex (WORKLOG #83). run56: p2_skinning_local was 42 % of a core. The generated kernel keeps the 77 guest
// vector registers of a hand-pipelined PowerPC loop (two vertices in flight) and spills most of them; this keeps
// only one vertex's values live.
//
// Exactness: the same simde operations in the same order on the same lane layout as the generated code (every
// accumulator's chain of mul/add is the generated one, vmaddfp = mul then add, flush mode on), so the memory it
// writes (24-byte vertices written as two 16-byte stores each, the bounding box, the stack words) is the
// original's byte for byte. Guest registers are not reproduced: the only caller (sub_829F5FF8 region) reads the
// bounding box from its stack and nothing else; ctx.fpscr is left in flush mode as the original leaves it.
// tests/skinning_diff.cpp compares memory against the generated reference on random inputs.
#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <vector>

namespace p2skin {

inline simde__m128i LoadBE(const uint8_t* p) {
  return simde_mm_shuffle_epi8(simde_mm_loadu_si128((const simde__m128i*)p),
                               simde_mm_load_si128((const simde__m128i*)VectorMaskL));
}
inline void StoreBE(uint8_t* p, simde__m128i v) {
  simde_mm_storeu_si128((simde__m128i*)p,
                        simde_mm_shuffle_epi8(v, simde_mm_load_si128((const simde__m128i*)VectorMaskL)));
}
// The first 8 bytes of StoreBE.
inline void StoreBE8(uint8_t* p, simde__m128i v) {
  alignas(16) uint8_t bytes[16];
  simde_mm_store_si128((simde__m128i*)bytes, simde_mm_shuffle_epi8(v, simde_mm_load_si128((const simde__m128i*)VectorMaskL)));
  std::memcpy(p, bytes, 8);
}
inline simde__m128i ConstBE(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
  uint32_t words[4] = {__builtin_bswap32(w0), __builtin_bswap32(w1), __builtin_bswap32(w2), __builtin_bswap32(w3)};
  return LoadBE(reinterpret_cast<const uint8_t*>(words));
}
inline void Store32BE(uint8_t* p, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(p, &v, 4); }
inline uint16_t Load16BE(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }

// vpkd3d128 type 2 (D3DCOLOR 2:10:10:10 from biased floats) into one 32-bit lane (lane = x86 index), as the
// generated scalar code: each lane clamped (x < min ? min : x > max ? max : x; NaN passes), low bits taken.
inline simde__m128i Pack2101010(simde__m128 v, int lane, simde__m128i into) {
  const simde__m128 lo = simde_mm_setr_ps(kPack2101010_Min2, kPack2101010_Min10, kPack2101010_Min10, kPack2101010_Min10);
  const simde__m128 hi = simde_mm_setr_ps(kPack2101010_Max2, kPack2101010_Max10, kPack2101010_Max10, kPack2101010_Max10);
  simde__m128 c = simde_mm_blendv_ps(v, hi, simde_mm_cmpgt_ps(v, hi));
  c = simde_mm_blendv_ps(c, lo, simde_mm_cmplt_ps(v, lo));
  simde__m128i bits = simde_mm_and_si128(simde_mm_castps_si128(c), simde_mm_setr_epi32(0x3, 0x3FF, 0x3FF, 0x3FF));
  bits = simde_mm_sllv_epi32(bits, simde_mm_setr_epi32(30, 20, 10, 0));
  bits = simde_mm_or_si128(bits, simde_mm_shuffle_epi32(bits, 0x4E));
  bits = simde_mm_or_si128(bits, simde_mm_shuffle_epi32(bits, 0xB1));
  const simde__m128i sel = lane == 3 ? simde_mm_setr_epi32(0, 0, 0, -1) : simde_mm_setr_epi32(0, 0, -1, 0);
  return simde_mm_or_si128(simde_mm_andnot_si128(sel, into), simde_mm_and_si128(sel, bits));
}

struct Consts {
  simde__m128i v0i, v2, v7, v13, v14;
  simde__m128 v0, v1, v3, v4, v8, v9;
};
inline Consts MakeConsts() {
  Consts c;
  c.v0i = ConstBE(0, 0, 0, 0x3F800000u);
  c.v0 = simde_mm_castsi128_ps(c.v0i);
  c.v1 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(c.v0i, 0x0));
  c.v2 = ConstBE(0x00010203u, 0x04050607u, 0x08090A0Bu, 0x1C1D1E1Fu);
  const uint32_t c3 = uint32_t(8200 + 14976 * 65536);
  c.v3 = simde_mm_castsi128_ps(ConstBE(c3, c3, c3, c3));
  c.v4 = simde_mm_add_ps(c.v3, c.v3);
  c.v7 = ConstBE(1023, 1023, 1023, 1023);
  const uint32_t c8 = uint32_t(16448) * 65536u;
  c.v8 = simde_mm_castsi128_ps(ConstBE(c8, c8, c8, c8));
  const uint32_t c9 = uint32_t(int32_t(-32768) + int32_t(14591) * 65536), c9w = uint32_t(13440) * 65536u;
  c.v9 = simde_mm_castsi128_ps(ConstBE(c9, c9, c9, c9w));
  c.v13 = ConstBE(0, 0xC0000000u, 0, 0);
  c.v14 = ConstBE(0, 0xFFFFFFFFu, 0, 0);
  return c;
}
inline constexpr uint32_t kBig = uint32_t(int32_t(-11099) + int32_t(21352) * 65536);

// Vertices [k0, k1) of n: writes them, folds their positions into the running min (v32) / max (v33).
// Each vertex is stored as two 16-byte stores at a 24-byte stride: its last 8 bytes spill into the next vertex,
// which overwrites them. When the next vertex belongs to another range (k1 < n) the spill is left out, so ranges
// can be written in any order with the sequential result; the call's last vertex still spills as the original.
// Approx (WORKLOG #99): the four bone matrices are first blended by the weights and the vertex is then
// transformed once (linear algebra: the same result up to float rounding, ~half the operations). Not
// byte-identical to the original (last-bit differences in positions/bounds, the 10-bit normals almost never).
template <bool Approx = false>
inline void SkinRange(uint8_t* base, const simde__m128* rows, const Consts& c, uint32_t in, uint32_t out,
                      uint32_t k0, uint32_t k1, uint32_t n, simde__m128& v32, simde__m128& v33) {
  const simde__m128i v0i = c.v0i, v2 = c.v2, v7 = c.v7, v13 = c.v13, v14 = c.v14;
  const simde__m128 v1 = c.v1, v3 = c.v3, v4 = c.v4, v8 = c.v8, v9 = c.v9;
  uint32_t r4 = in + 32 * k0, r3 = out + 24 * k0;
#pragma GCC unroll 2
  for (uint32_t k = k0; k < k1; ++k, r4 += 32, r3 += 24) {
    __builtin_prefetch(base + uint32_t(r4 + 256));
    uint32_t m[4];
    for (int b = 0; b < 4; ++b) m[b] = Load16BE(base + uint32_t(r4 + 12 + 2 * b));
    const simde__m128 q0 = simde_mm_castsi128_ps(LoadBE(base + (r4 & ~15u)));
    const simde__m128i q1 = LoadBE(base + (uint32_t(r4 + 16) & ~15u));
    const simde__m128i v76 = simde_mm_and_si128(q1, v13);
    simde__m128i v59 = simde_mm_slli_si128(q1, 4);  // vslo by v5 (4 bytes) in this lane layout
    simde__m128i v56 = simde_mm_srli_epi32(v59, 10);
    simde__m128i v53 = simde_mm_and_si128(v59, v7);
    v59 = simde_mm_srli_epi32(v56, 10);
    v56 = simde_mm_and_si128(v56, v7);
    simde__m128 n53 = simde_mm_cvtepi32_ps(v53);
    v59 = simde_mm_and_si128(v59, v7);
    simde__m128 t56 = simde_mm_cvtepi32_ps(v56);
    simde__m128 w59 = simde_mm_cvtepi32_ps(v59);
    n53 = simde_mm_mul_ps(n53, v4);
    t56 = simde_mm_mul_ps(t56, v4);
    w59 = simde_mm_mul_ps(w59, v3);
    n53 = simde_mm_sub_ps(n53, v1);
    t56 = simde_mm_sub_ps(t56, v1);
    auto spl = [](simde__m128 v, int imm) {
      return simde_mm_castsi128_ps(simde_mm_shuffle_epi32(simde_mm_castps_si128(v), imm));
    };
    // PPC lane 0/1/2 = shuffle 0xFF/0xAA/0x55 in this layout.
    const simde__m128 nx = spl(n53, 0xFF), ny = spl(n53, 0xAA), nz = spl(n53, 0x55);
    const simde__m128 tx = spl(t56, 0xFF), ty = spl(t56, 0xAA), tz = spl(t56, 0x55);
    const simde__m128 w0 = spl(w59, 0xFF), w1 = spl(w59, 0xAA), w2 = spl(w59, 0x55);
    const simde__m128 px = spl(q0, 0xFF), py = spl(q0, 0xAA), pz = spl(q0, 0x55);
    simde__m128 w3 = simde_mm_add_ps(simde_mm_add_ps(simde_mm_add_ps(simde_mm_castsi128_ps(simde_mm_setzero_si128()), w0), w1), w2);
    w3 = simde_mm_sub_ps(v1, w3);
    simde__m128 nrm, tan, pos;
    if constexpr (Approx) {
      const simde__m128* R0 = &rows[m[0] * 4];
      const simde__m128* R1 = &rows[m[1] * 4];
      const simde__m128* R2 = &rows[m[2] * 4];
      const simde__m128* R3 = &rows[m[3] * 4];
      simde__m128 M[4];
      for (int r = 0; r < 4; ++r)
        M[r] = simde_mm_add_ps(simde_mm_add_ps(simde_mm_mul_ps(R0[r], w0), simde_mm_mul_ps(R1[r], w1)),
                               simde_mm_add_ps(simde_mm_mul_ps(R2[r], w2), simde_mm_mul_ps(R3[r], w3)));
      nrm = simde_mm_add_ps(simde_mm_add_ps(simde_mm_mul_ps(nx, M[0]), simde_mm_mul_ps(ny, M[1])), simde_mm_mul_ps(nz, M[2]));
      tan = simde_mm_add_ps(simde_mm_add_ps(simde_mm_mul_ps(tx, M[0]), simde_mm_mul_ps(ty, M[1])), simde_mm_mul_ps(tz, M[2]));
      pos = simde_mm_add_ps(simde_mm_add_ps(simde_mm_mul_ps(px, M[0]), simde_mm_mul_ps(py, M[1])),
                            simde_mm_add_ps(simde_mm_mul_ps(pz, M[2]), M[3]));
    } else {
    simde__m128 a[4], t[4], p[4];
    for (int b = 0; b < 4; ++b) {
      const simde__m128* row = &rows[m[b] * 4];
      const simde__m128 r0 = row[0], r1 = row[1], r2 = row[2], r3row = row[3];
      a[b] = simde_mm_mul_ps(nx, r0);
      a[b] = simde_mm_add_ps(simde_mm_mul_ps(ny, r1), a[b]);
      a[b] = simde_mm_add_ps(simde_mm_mul_ps(nz, r2), a[b]);
      t[b] = simde_mm_mul_ps(tx, r0);
      t[b] = simde_mm_add_ps(simde_mm_mul_ps(ty, r1), t[b]);
      t[b] = simde_mm_add_ps(simde_mm_mul_ps(tz, r2), t[b]);
      p[b] = simde_mm_add_ps(simde_mm_mul_ps(px, r0), r3row);
      p[b] = simde_mm_add_ps(simde_mm_mul_ps(py, r1), p[b]);
      p[b] = simde_mm_add_ps(simde_mm_mul_ps(pz, r2), p[b]);
    }
    simde__m128 v34 = simde_mm_mul_ps(a[0], w0);
    simde__m128 v38 = simde_mm_mul_ps(t[0], w0);
    simde__m128 v35 = simde_mm_mul_ps(a[1], w1);
    simde__m128 v39 = simde_mm_mul_ps(t[1], w1);
    simde__m128 v60 = simde_mm_mul_ps(p[0], w0);
    simde__m128 v64 = simde_mm_mul_ps(p[1], w1);
    v34 = simde_mm_add_ps(simde_mm_mul_ps(a[2], w2), v34);
    v38 = simde_mm_add_ps(simde_mm_mul_ps(t[2], w2), v38);
    v35 = simde_mm_add_ps(simde_mm_mul_ps(a[3], w3), v35);
    v39 = simde_mm_add_ps(simde_mm_mul_ps(t[3], w3), v39);
    v60 = simde_mm_add_ps(simde_mm_mul_ps(p[2], w2), v60);
    v64 = simde_mm_add_ps(simde_mm_mul_ps(p[3], w3), v64);
    nrm = simde_mm_add_ps(v34, v35);
    tan = simde_mm_add_ps(v38, v39);
    pos = simde_mm_add_ps(v60, v64);
    }
    // Output of this vertex.
    simde__m128 v10 = simde_mm_dp_ps(nrm, nrm, 0xEF);
    const simde__m128i v17 = P2_VMX_PERMUTE(simde_mm_castps_si128(pos), v0i, v2);
    v32 = simde_mm_min_ps(v32, pos);
    v33 = simde_mm_max_ps(v33, pos);
    // All four lanes of the dot product are equal: one scalar sqrt and divide, the same IEEE results.
    v10 = simde_mm_set1_ps(1.0f / __builtin_sqrtf(simde_mm_cvtss_f32(v10)));
    simde__m128 v11 = simde_mm_mul_ps(nrm, v10);
    const simde__m128 v12 = simde_mm_mul_ps(tan, v10);
    v10 = simde_mm_add_ps(simde_mm_mul_ps(v11, v9), v8);
    v11 = simde_mm_add_ps(simde_mm_mul_ps(v12, v9), v8);
    simde__m128i p10 = Pack2101010(v10, 3, simde_mm_castps_si128(v10));
    const simde__m128i p11 = Pack2101010(v11, 2, simde_mm_castps_si128(v11));
    p10 = simde_mm_or_si128(simde_mm_andnot_si128(v14, p10), simde_mm_and_si128(v14, p11));
    p10 = simde_mm_or_si128(p10, v76);
    StoreBE(base + r3, v17);
    if (k + 1 == k1 && k1 != n) StoreBE8(base + uint32_t(r3 + 16), p10);
    else StoreBE(base + uint32_t(r3 + 16), p10);
  }
}

// out = r3, in = r4, n = r5 (>= 2), palette = r6, bbox = r7, sp = r1, lr = ctx.lr. Guest memory at `base`.
// `run(n, range)` runs range(k0, k1, v32, v33) over [0, n) and returns the folded bounds (sequential by default;
// switch_skinning.cpp splits big meshes with a helper thread: same per-vertex operations, min/max fold order
// does not change the result for non-NaN positions).
struct Sequential {
  template <class F>
  void operator()(uint32_t n, F&& range, simde__m128& v32, simde__m128& v33) const { range(0u, n, v32, v33); }
};
template <class Run = Sequential>
inline void Skin(uint8_t* base, uint32_t out, uint32_t in, uint32_t n, uint32_t palette, uint32_t bbox,
                 uint32_t sp, uint32_t lr, uint32_t max_index = UINT32_MAX, const Run& run = Run(), bool approx = false) {
  // Prologue and constant helper: the stack words it leaves.
  Store32BE(base + uint32_t(sp - 16), bbox);
  Store32BE(base + uint32_t(sp - 8), lr);
  const uint32_t big = kBig;
  for (int i = 0; i < 4; ++i) Store32BE(base + uint32_t(sp - 32 + 4 * i), big);
  const Consts c = MakeConsts();
  simde__m128 v32 = simde_mm_castsi128_ps(ConstBE(big, big, big, big));
  simde__m128 v33 = simde_mm_sub_ps(c.v0, v32);

  // The bone rows the vertices use, loaded (as the original's aligned lvx) and byte-swapped once per call
  // instead of 16 times per vertex. On the stack for up to 128 bones (no thread_local: TLS is a call here).
  if (max_index == UINT32_MAX) {
    max_index = 0;
    for (uint32_t k = 0; k < n; ++k)
      for (int b = 0; b < 4; ++b)
        max_index = std::max<uint32_t>(max_index, Load16BE(base + uint32_t(in + 32 * k + 12 + 2 * b)));
  }
  simde__m128 stack_rows[128 * 4];
  std::vector<simde__m128> heap_rows;
  simde__m128* rows = stack_rows;
  if (max_index >= 128) {
    heap_rows.resize(size_t(max_index + 1) * 4);
    rows = heap_rows.data();
  }
  for (uint32_t i = 0; i <= max_index; ++i) {
    const uint32_t mb = __builtin_rotateleft32(i, 6) + palette;
    for (uint32_t r = 0; r < 4; ++r)
      rows[i * 4 + r] = simde_mm_castsi128_ps(LoadBE(base + (uint32_t(mb + 16 * r) & ~15u)));
  }
  run(n, [&](uint32_t k0, uint32_t k1, simde__m128& lo, simde__m128& hi) {
    if (approx) SkinRange<true>(base, rows, c, in, out, k0, k1, n, lo, hi);
    else SkinRange<false>(base, rows, c, in, out, k0, k1, n, lo, hi);
  }, v32, v33);
  StoreBE(base + (bbox & ~15u), simde_mm_castps_si128(v32));
  StoreBE(base + (uint32_t(bbox + 16) & ~15u), simde_mm_castps_si128(v33));
}

}  // namespace p2skin
