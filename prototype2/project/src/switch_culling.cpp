// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2: the job workers' object culling routine sub_82A22DD8 (bounding boxes against up to 6 planes plus a
// minimum projected size) in native code (WORKLOG #87). run59: 5.8 % of a core on the saturated cores 1-2.
//
// Same operations as the generated code (simde, same lane layout, same FPCR flush mode): per object of the
// A&B bit masks, corner_j = select(min, max, n_j <= 0), pass = |max-min|^2 >= s^2*c and -d_j >= dot3(corner_j, n_j)
// for the six plane slots (missing planes as the original fills them: n = 0, mask 0, -d = 1.0 from vupkd3d128).
// Output: the C bit masks. The original's red-zone scratch below the stack pointer is not reproduced (dead).
// More than 6 planes (the original's scratch slots then overlap) -> the original.
// The first p2_cull_native_verify calls run both and compare the masks; a difference keeps the original's result
// and turns the native path off.
#include "generated/default/prototype2_pch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <arm_neon.h>
#include <atomic>
#include <cstring>
#include <vector>

REXCVAR_DEFINE_BOOL(p2_cull_native, true, "Prototype2",
                    "Native code for the culling routine sub_82A22DD8 (same results); false = recompiled");
REXCVAR_DEFINE_UINT32(p2_cull_native_verify, 256, "Prototype2",
                      "First N culling calls also run the recompiled routine and compare every mask word");
REX_EXTERN(__imp__sub_82A22DD8);

namespace {
std::atomic<uint32_t> g_checks{0};
std::atomic<bool> g_failed{false};

inline uint32_t Load32(const uint8_t* base, uint32_t a) { uint32_t v; std::memcpy(&v, base + a, 4); return __builtin_bswap32(v); }
inline simde__m128i LoadBE(const uint8_t* p) {
  return simde_mm_shuffle_epi8(simde_mm_loadu_si128((const simde__m128i*)p),
                               simde_mm_load_si128((const simde__m128i*)VectorMaskL));
}
inline simde__m128 Splat0(simde__m128i v) {  // vspltw lane 0 (x86 lane 3 in this layout)
  return simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v, 0xFF));
}

void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t P = ctx.r3.u32;
  const uint32_t count = Load32(base, P + 20);
  const uint32_t planes = Load32(base, P + 168);
  const uint32_t a_base = Load32(base, P + 4), b_base = Load32(base, P + 8), c_base = Load32(base, P + 12);
  ctx.fpscr.enableFlushMode();
  const simde__m128 s = Splat0(LoadBE(base + uint32_t(P + 172)));
  const simde__m128 c = Splat0(LoadBE(base + 0x82000000u + 3088u));
  const simde__m128 s2c = simde_mm_mul_ps(simde_mm_mul_ps(s, s), c);
  const simde__m128i sign = LoadBE(base + ((0x82000000u + 4448u) & ~15u));
  const simde__m128 zero = simde_mm_castsi128_ps(simde_mm_setzero_si128());
  simde__m128 normal[6], negd[6];
  simde__m128i mask[6];
  for (uint32_t j = 0; j < 6; ++j) {
    if (j < planes) {
      const uint32_t a = P + 24 + 16 * j;
      simde__m128 n = simde_mm_castsi128_ps(LoadBE(base + a));
      n = simde_mm_blend_ps(n, simde_mm_permute_ps(zero, 228), 1);  // vrlimi128 n, 0, 1, 0
      normal[j] = n;
      negd[j] = simde_mm_castsi128_ps(simde_mm_xor_si128(sign, simde_mm_castps_si128(Splat0(LoadBE(base + uint32_t(a + 12))))));
      mask[j] = simde_mm_castps_si128(simde_mm_cmpge_ps(zero, n));
    } else {
      normal[j] = zero;
      mask[j] = simde_mm_setzero_si128();
      negd[j] = simde_mm_castsi128_ps(simde_mm_set1_epi32(0x3F800000));  // vupkd3d128(0, 0)
    }
  }
  const uint32_t words = (((((count + 7u) >> 3) & 0x1FFFFFFFu) + 3u) >> 2) & 0x3FFFFFFFu;
  const uint32_t boxes = Load32(base, P + 16);
  // WORKLOG #94: four objects per pass (one per NEON lane) instead of one object per vector. Exactness: the
  // generated code's dot3 is simde_mm_dp_ps(a, b, 0xEF), on AArch64 vmulq, lane 0 masked to +0, vaddvq =
  // (p0 + p1) + (p2 + p3) with p0 = +0; reproduced per lane as (0 + x1*y1) + (x2*y2 + x3*y3) (FPCR flush mode
  // as set above, no fused multiply-add). The corner per plane picks min or max per component by the plane's
  // sign mask, the same for every object.
  float nrm[6][4], nd[6][4], s2cf[4];
  uint32_t msk[6][4];
  {
    alignas(16) uint32_t tmpu[4];
    simde_mm_store_ps(s2cf, s2c);
    for (int j = 0; j < 6; ++j) {
      simde_mm_store_ps(nrm[j], normal[j]);
      simde_mm_store_ps(nd[j], negd[j]);
      simde_mm_store_si128((simde__m128i*)tmpu, mask[j]);
      for (int q = 0; q < 4; ++q) msk[j][q] = tmpu[q];
    }
  }
  const float32x4_t zero4 = vdupq_n_f32(0.0f);
  auto dot = [&](float32x4_t x1, float32x4_t x2, float32x4_t x3, float32x4_t y1, float32x4_t y2,
                 float32x4_t y3) {
    return vaddq_f32(vaddq_f32(zero4, vmulq_f32(x1, y1)), vaddq_f32(vmulq_f32(x2, y2), vmulq_f32(x3, y3)));
  };
  for (uint32_t i = 0; i < words; ++i) {
    uint32_t live = Load32(base, a_base + 4 * i) & Load32(base, b_base + 4 * i);
    if (i * 32 + 32 > count) live &= count > i * 32 ? (1u << (count - i * 32)) - 1u : 0u;
    uint32_t result = 0;
    while (live) {
      uint32_t ks[4];
      int g = 0;
      while (live && g < 4) {
        ks[g++] = uint32_t(__builtin_ctz(live));
        live &= live - 1;
      }
      alignas(16) float mn[4][4], mx[4][4];  // [object][x86 lane]
      for (int q = 0; q < 4; ++q) {
        const uint32_t box = boxes + (i << 10) + 32 * ks[q < g ? q : 0];
        simde_mm_store_si128((simde__m128i*)mn[q], LoadBE(base + (box & ~15u)));
        simde_mm_store_si128((simde__m128i*)mx[q], LoadBE(base + (uint32_t(box + 16) & ~15u)));
      }
      const float32x4x4_t MN = vld4q_f32(&mn[0][0]), MX = vld4q_f32(&mx[0][0]);  // .val[lane] = 4 objects
      const float32x4_t e1 = vsubq_f32(MX.val[1], MN.val[1]), e2 = vsubq_f32(MX.val[2], MN.val[2]),
                        e3 = vsubq_f32(MX.val[3], MN.val[3]);
      // Each test passes only if all four lanes of the original's compare do (dot3 is broadcast; the other
      // operand's lanes are compared one by one).
      const float32x4_t size = dot(e1, e2, e3, e1, e2, e3);
      uint32x4_t pass = vcgeq_f32(size, vdupq_n_f32(s2cf[0]));
      for (int q = 1; q < 4; ++q) pass = vandq_u32(pass, vcgeq_f32(size, vdupq_n_f32(s2cf[q])));
      for (int j = 0; j < 6; ++j) {
        const float32x4_t c1 = msk[j][1] ? MX.val[1] : MN.val[1], c2 = msk[j][2] ? MX.val[2] : MN.val[2],
                          c3 = msk[j][3] ? MX.val[3] : MN.val[3];
        const float32x4_t d = dot(c1, c2, c3, vdupq_n_f32(nrm[j][1]), vdupq_n_f32(nrm[j][2]),
                                  vdupq_n_f32(nrm[j][3]));
        for (int q = 0; q < 4; ++q) pass = vandq_u32(pass, vcgeq_f32(vdupq_n_f32(nd[j][q]), d));
      }
      alignas(16) uint32_t ok[4];
      vst1q_u32(ok, pass);
      for (int q = 0; q < g; ++q)
        if (ok[q]) result |= 1u << ks[q];
    }
    const uint32_t be = __builtin_bswap32(result);
    std::memcpy(base + uint32_t(c_base + 4 * i), &be, 4);
  }
}
}  // namespace

extern "C" REX_FUNC(sub_82A22DD8) {
  if (!REXCVAR_GET(p2_cull_native) || g_failed.load(std::memory_order_relaxed) ||
      Load32(base, ctx.r3.u32 + 168) > 6) {
    __imp__sub_82A22DD8(ctx, base);
    return;
  }
  const uint32_t verify = REXCVAR_GET(p2_cull_native_verify);
  if (g_checks.load(std::memory_order_relaxed) >= verify || g_checks.fetch_add(1, std::memory_order_relaxed) >= verify) {
    Native(ctx, base);
    return;
  }
  const uint32_t P = ctx.r3.u32;
  const uint32_t count = Load32(base, P + 20);
  const uint32_t words = (((((count + 7u) >> 3) & 0x1FFFFFFFu) + 3u) >> 2) & 0x3FFFFFFFu;
  const uint32_t c_base = Load32(base, P + 12);
  std::vector<uint8_t> before(base + c_base, base + c_base + 4 * size_t(words));
  PPCContext trial = ctx;
  __imp__sub_82A22DD8(ctx, base);
  std::vector<uint8_t> reference(base + c_base, base + c_base + 4 * size_t(words));
  std::memcpy(base + c_base, before.data(), before.size());
  Native(trial, base);
  if (std::memcmp(base + c_base, reference.data(), reference.size()) != 0) {
    std::memcpy(base + c_base, reference.data(), reference.size());
    g_failed.store(true, std::memory_order_relaxed);
    REXLOG_ERROR("[p2-cull] MISMATCH objects={} planes={} - recompiled result kept, native culling off", count,
                 Load32(base, P + 168));
    return;
  }
  const uint32_t n = g_checks.load(std::memory_order_relaxed);
  if (n <= 4 || n == verify) REXLOG_INFO("[p2-cull] verify {} OK objects={} words={}", n, count, words);
}

// ---------------------------------------------------------------------------------------------------------------
// WORKLOG #96: sub_82A234B8, the job workers' occluder culling (run66: 5.2 % of a core). Inputs: r3 = cull job
// (+24 boxes, 32 bytes each, 1024 bytes per 32-object word; +40/+44 object array begin/end -> n = words*32),
// r4 = [begin, end) of occluder volumes (148 bytes: u32 plane count, then planes x,y,z,d at +4 + 16 p, unaligned),
// r5 = visibility mask words, r6 = optional second mask words. An object (bit set in r5[i]) is cleared from both
// masks when, for some volume, no plane has dot3(corner, n) > -d on any lane (it is inside that volume: occluded);
// a volume without planes clears every object it is reached for. corner = n_lane <= 0 ? min : max per lane.
// Order of volumes/planes does not change the result (pure compares), so four objects go through the tests at
// once. dot3 as in the routine above: (0 + x1*y1) + (x2*y2 + x3*y3) per lane (simde_mm_dp_ps(0xEF) on AArch64).
// More than 16 volumes or 10 planes (the original's stack tables would overlap), or no objects -> original.
REXCVAR_DEFINE_BOOL(p2_occlusion_native, true, "Prototype2",
                    "Native code for the occluder culling routine sub_82A234B8 (same results); false = recompiled");
REXCVAR_DEFINE_UINT32(p2_occlusion_native_verify, 256, "Prototype2",
                      "First N occluder culling calls also run the recompiled routine and compare the masks");
REX_EXTERN(__imp__sub_82A234B8);

namespace {
std::atomic<uint32_t> g_occ_checks{0};
std::atomic<bool> g_occ_failed{false};

struct OccPlane { float n1, n2, n3; float nd[4]; bool min1, min2, min3; };

// false: shape the native code does not handle (use the original).
bool OccluderNative(PPCContext& ctx, uint8_t* base) {
  const uint32_t job = ctx.r3.u32, vols = ctx.r4.u32, m5 = ctx.r5.u32, m6 = ctx.r6.u32;
  const uint32_t v_begin = Load32(base, vols), v_end = Load32(base, vols + 4);
  const int32_t vbytes = int32_t(v_end - v_begin);
  const uint32_t nvol = vbytes > 0 ? uint32_t(vbytes / 148) : 0;
  const int32_t obytes = int32_t(Load32(base, job + 44) - Load32(base, job + 40));
  if (vbytes < 0 || nvol > 16 || obytes <= 0) return false;
  const uint32_t n_objects = uint32_t(obytes >> 2);
  const uint32_t words = ((n_objects - 1) >> 5) + 1;
  ctx.fpscr.enableFlushMode();
  alignas(16) float sign_lanes[4];
  {
    const simde__m128i sign = LoadBE(base + ((0x82000000u + 4448u) & ~15u));
    simde_mm_store_si128((simde__m128i*)sign_lanes, sign);
  }
  uint32_t sign_bits[4];
  std::memcpy(sign_bits, sign_lanes, 16);
  OccPlane planes[16][10];
  uint32_t counts[16];
  bool empty_volume = false;
  for (uint32_t v = 0; v < nvol; ++v) {
    const uint32_t vb = v_begin + 148 * v;
    counts[v] = Load32(base, vb);
    if (counts[v] > 10) return false;
    if (counts[v] == 0) empty_volume = true;
    for (uint32_t p = 0; p < counts[v]; ++p) {
      alignas(16) float n[4];  // x86 lanes: 3 = x, 2 = y, 1 = z, 0 = d (zeroed for the normal)
      simde_mm_store_si128((simde__m128i*)n, LoadBE(base + vb + 4 + 16 * p));
      uint32_t dbits;
      std::memcpy(&dbits, &n[0], 4);
      OccPlane& pl = planes[v][p];
      pl.n1 = n[1]; pl.n2 = n[2]; pl.n3 = n[3];
      pl.min1 = 0.0f >= n[1];  // cmpge(0, n): NaN -> false -> max
      pl.min2 = 0.0f >= n[2];
      pl.min3 = 0.0f >= n[3];
      for (int q = 0; q < 4; ++q) {
        const uint32_t b = sign_bits[q] ^ dbits;
        std::memcpy(&pl.nd[q], &b, 4);
      }
    }
  }
  const uint32_t boxes = Load32(base, job + 24);
  const float32x4_t zero4 = vdupq_n_f32(0.0f);
  for (uint32_t i = 0; i < words; ++i) {
    uint32_t r28 = Load32(base, m5 + 4 * i);
    uint32_t r26 = m6 ? Load32(base, m6 + 4 * i) : 0;
    uint32_t live = r28;
    uint32_t clear = 0;
    if (nvol && empty_volume) {
      clear = live;  // every tested object is "inside" the plane-less volume
      live = 0;
    }
    while (live) {
      uint32_t ks[4];
      int g = 0;
      while (live && g < 4) {
        ks[g++] = uint32_t(__builtin_ctz(live));
        live &= live - 1;
      }
      alignas(16) float mn[4][4], mx[4][4];
      for (int q = 0; q < 4; ++q) {
        const uint32_t box = boxes + 1024 * i + 32 * ks[q < g ? q : 0];
        simde_mm_store_si128((simde__m128i*)mn[q], LoadBE(base + (box & ~15u)));
        simde_mm_store_si128((simde__m128i*)mx[q], LoadBE(base + (uint32_t(box + 16) & ~15u)));
      }
      const float32x4x4_t MN = vld4q_f32(&mn[0][0]), MX = vld4q_f32(&mx[0][0]);
      uint32x4_t occluded = vdupq_n_u32(0);
      for (uint32_t v = 0; v < nvol; ++v) {
        uint32x4_t outside = vdupq_n_u32(0);
        for (uint32_t p = 0; p < counts[v]; ++p) {
          const OccPlane& pl = planes[v][p];
          const float32x4_t c1 = pl.min1 ? MN.val[1] : MX.val[1], c2 = pl.min2 ? MN.val[2] : MX.val[2],
                            c3 = pl.min3 ? MN.val[3] : MX.val[3];
          const float32x4_t d = vaddq_f32(vaddq_f32(zero4, vmulq_f32(c1, vdupq_n_f32(pl.n1))),
                                          vaddq_f32(vmulq_f32(c2, vdupq_n_f32(pl.n2)), vmulq_f32(c3, vdupq_n_f32(pl.n3))));
          for (int q = 0; q < 4; ++q) outside = vorrq_u32(outside, vcgtq_f32(d, vdupq_n_f32(pl.nd[q])));
        }
        occluded = vorrq_u32(occluded, vmvnq_u32(outside));
      }
      alignas(16) uint32_t occ[4];
      vst1q_u32(occ, occluded);
      for (int q = 0; q < g; ++q)
        if (occ[q]) clear |= 1u << ks[q];
    }
    r28 &= ~clear;
    r26 &= ~clear;
    const uint32_t be28 = __builtin_bswap32(r28);
    std::memcpy(base + uint32_t(m5 + 4 * i), &be28, 4);
    if (m6) {
      const uint32_t be26 = __builtin_bswap32(r26);
      std::memcpy(base + uint32_t(m6 + 4 * i), &be26, 4);
    }
  }
  return true;
}
}  // namespace

extern "C" REX_FUNC(sub_82A234B8) {
  if (!REXCVAR_GET(p2_occlusion_native) || g_occ_failed.load(std::memory_order_relaxed)) {
    __imp__sub_82A234B8(ctx, base);
    return;
  }
  const uint32_t verify = REXCVAR_GET(p2_occlusion_native_verify);
  if (g_occ_checks.load(std::memory_order_relaxed) >= verify ||
      g_occ_checks.fetch_add(1, std::memory_order_relaxed) >= verify) {
    if (!OccluderNative(ctx, base)) __imp__sub_82A234B8(ctx, base);
    return;
  }
  // Inputs saved before the recompiled routine runs: it leaves r4 = 16 (run85/86 startup crashes: the log line
  // read the volume count through the clobbered r4 -> guest address 0x14).
  const uint32_t job = ctx.r3.u32, vols = ctx.r4.u32, m5 = ctx.r5.u32, m6 = ctx.r6.u32;
  const uint32_t volumes = (Load32(base, vols + 4) - Load32(base, vols)) / 148;
  const int32_t obytes = int32_t(Load32(base, job + 44) - Load32(base, job + 40));
  if (obytes <= 0) {
    __imp__sub_82A234B8(ctx, base);
    return;
  }
  const uint32_t words = (((uint32_t(obytes) >> 2) - 1) >> 5) + 1;
  std::vector<uint8_t> b5(base + m5, base + m5 + 4 * size_t(words)), b6;
  if (m6) b6.assign(base + m6, base + m6 + 4 * size_t(words));
  PPCContext trial = ctx;
  __imp__sub_82A234B8(ctx, base);
  std::vector<uint8_t> r5(base + m5, base + m5 + 4 * size_t(words)), r6;
  if (m6) r6.assign(base + m6, base + m6 + 4 * size_t(words));
  std::memcpy(base + m5, b5.data(), b5.size());
  if (m6) std::memcpy(base + m6, b6.data(), b6.size());
  const bool handled = OccluderNative(trial, base);
  const bool same = handled && std::memcmp(base + m5, r5.data(), r5.size()) == 0 &&
                    (!m6 || std::memcmp(base + m6, r6.data(), r6.size()) == 0);
  if (!same) {
    std::memcpy(base + m5, r5.data(), r5.size());
    if (m6) std::memcpy(base + m6, r6.data(), r6.size());
    if (handled) {
      g_occ_failed.store(true, std::memory_order_relaxed);
      REXLOG_ERROR("[p2-occl] MISMATCH words={} volumes={} - recompiled result kept, native occluder culling off",
                   words, volumes);
    }
    return;
  }
  const uint32_t n = g_occ_checks.load(std::memory_order_relaxed);
  if (n <= 4 || n == verify)
    REXLOG_INFO("[p2-occl] verify {} OK words={} volumes={}", n, words,
                volumes);
}
