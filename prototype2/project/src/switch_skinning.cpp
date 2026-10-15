// SPDX-License-Identifier: GPL-3.0-or-later
// Exact register-local specialization of the game's skinning routine.
#include "generated/default/prototype2_pch.h"
#include <rex/cvar.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <vector>
#include <cstring>
#include <atomic>

REXCVAR_DEFINE_BOOL(p2_skinning_local, false, "Prototype2", "Exact skinning specialization with local registers (rollback: false)");
REXCVAR_DEFINE_UINT32(p2_skinning_verify_calls, 32, "Prototype2", "Differential checks against original skinning; compares output, AABB, stack and registers");
REX_EXTERN(__imp__sub_829F5978);
#include "p2_skinning_kernel.inc"
#include "skinning_input_guard.h"
#include "p2_skinning_fast.h"
#include "p2_skinning_parallel.h"
#include "p2_skin_cache.h"
#include <rex/logging.h>
#include "native/skin_write_log.h"

REXCVAR_DEFINE_BOOL(p2_skinning_fast, true, "Prototype2",
    "Per-vertex rewrite of the skinning routine (p2_skinning_fast.h, same operations, memory byte-identical); "
    "false = the register-local generated kernel");
REXCVAR_DEFINE_UINT32(p2_skinning_fast_verify, 64, "Prototype2",
    "First N skinning calls run both the generated kernel and the rewrite and compare every byte written; a "
    "difference keeps the generated result and turns the rewrite off");
REXCVAR_DEFINE_BOOL(p2_skinning_approx, true, "Prototype2",
    "WORKLOG #99: blend the four bone matrices first, then transform once (about half the arithmetic; float "
    "rounding differs from the original in the last bits, so the byte-for-byte verification is skipped). "
    "false = the byte-identical rewrite");
REXCVAR_DEFINE_UINT32(p2_skinning_parallel_min, 384, "Prototype2",
    "Skinning calls with at least this many vertices are shared with a helper thread on cores 1-2 (WORKLOG #92; "
    "0 = never)");
REXCVAR_DEFINE_UINT32(p2_skinning_helper_priority, 0x2C, "Prototype2",
    "Horizon priority of the skinning helper thread (0x2C = the GPU ring thread's)");
REXCVAR_DEFINE_UINT32(p2_skinning_helper_mask, 0x6, "Prototype2",
    "Core mask of the skinning helper thread (WORKLOG #101: 0x8 = core 3 when the process may use it)");
REXCVAR_DEFINE_UINT32(p2_skinning_helper_spin_us, 150, "Prototype2",
    "The skinning helper keeps polling this long after its last chunk before it sleeps (calls come in bursts)");
namespace {
p2skinpar::Parallel ParallelRunner() {
  const uint32_t min = REXCVAR_GET(p2_skinning_parallel_min);
  if (min) p2skinpar::Start(int(REXCVAR_GET(p2_skinning_helper_priority)), REXCVAR_GET(p2_skinning_helper_mask),
                           REXCVAR_GET(p2_skinning_helper_spin_us));
  const uint64_t calls = p2skinpar::g_calls.load(std::memory_order_relaxed);
  if (calls && calls % 400000 == 0)
    REXLOG_INFO("[p2-skin-par] {} calls, {} vertices, {} split, {} vertices by the helper", calls,
                p2skinpar::g_vertices.load(std::memory_order_relaxed), p2skinpar::g_split.load(std::memory_order_relaxed),
                p2skinpar::g_helper_vertices.load(std::memory_order_relaxed));
  return p2skinpar::Parallel{min ? min : UINT32_MAX};
}
std::atomic<uint32_t> checks{0};
std::atomic<uint32_t> fast_checks{0};
std::atomic<bool> fast_failed{false};
std::atomic<bool> failed{false};
struct Region {
  uint32_t address;
  std::vector<uint8_t> before, reference;
  void Save(uint8_t* base, uint32_t a, size_t size) {
    address = a;
    before.assign(base + a, base + a + size);
  }
  void Restore(uint8_t* base) const { std::memcpy(base + address, before.data(), before.size()); }
};
bool Overlaps(uint32_t a, uint64_t n, uint32_t b, uint64_t m) {
  return uint64_t(a) < uint64_t(b)+m && uint64_t(b) < uint64_t(a)+n;
}
}

// WORKLOG #115 diagnostic: how many skinning calls repeat one already made (same mesh, vertex count and bone
// palette contents) in the same renderer frame or the previous one. Racy table, counts only.
REXCVAR_DEFINE_BOOL(p2_skin_cache, true, "Prototype2",
    "Approximate skinning: an identical call (same input, palette contents, vertex count) replays the bytes the "
    "previous one wrote instead of computing them (WORKLOG #116; false = always compute)");
REXCVAR_DEFINE_BOOL(p2_skin_dup_stats, false, "Prototype2",
    "Log how many skinning calls repeat an identical earlier call (same frame / previous frame)");
namespace dupstats {
struct Slot { std::atomic<uint64_t> key{0}; std::atomic<uint32_t> frame{0}; };
inline Slot g_slots[8192];
inline std::atomic<uint64_t> g_calls{0}, g_vertices{0}, g_same_frame{0}, g_same_frame_v{0}, g_prev_frame{0},
    g_prev_frame_v{0};
}
static void DupStats(uint8_t* base, uint32_t input, uint32_t n, uint32_t palette, uint32_t max_index) {
  const uint8_t* rows = base + (palette & ~15u);
  const size_t bytes = size_t(std::min<uint32_t>(max_index, 255) + 1) * 64;
  uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t(input) << 32) ^ n;
  for (size_t i = 0; i + 8 <= bytes; i += 8) {
    uint64_t w;
    std::memcpy(&w, rows + i, 8);
    h = (h ^ w) * 0xFF51AFD7ED558CCDull;
    h ^= h >> 29;
  }
  const uint32_t frame = p2::skinlog::g_frame.load(std::memory_order_relaxed) + 1;
  auto& slot = dupstats::g_slots[(h >> 51) & 8191];
  const uint64_t old_key = slot.key.load(std::memory_order_relaxed);
  const uint32_t old_frame = slot.frame.load(std::memory_order_relaxed);
  if (old_key == h && old_frame == frame) {
    dupstats::g_same_frame.fetch_add(1, std::memory_order_relaxed);
    dupstats::g_same_frame_v.fetch_add(n, std::memory_order_relaxed);
  } else if (old_key == h && old_frame + 1 == frame) {
    dupstats::g_prev_frame.fetch_add(1, std::memory_order_relaxed);
    dupstats::g_prev_frame_v.fetch_add(n, std::memory_order_relaxed);
  }
  slot.key.store(h, std::memory_order_relaxed);
  slot.frame.store(frame, std::memory_order_relaxed);
  dupstats::g_vertices.fetch_add(n, std::memory_order_relaxed);
  if (dupstats::g_calls.fetch_add(1, std::memory_order_relaxed) % 100000 == 99999)
    REXLOG_INFO("[p2-skin-dup] {} calls / {} vertices: repeated in the same frame {} calls / {} vertices, "
                "unchanged from the previous frame {} / {}", dupstats::g_calls.load(), dupstats::g_vertices.load(),
                dupstats::g_same_frame.load(), dupstats::g_same_frame_v.load(), dupstats::g_prev_frame.load(),
                dupstats::g_prev_frame_v.load());
}

static REX_FUNC(P2SkinningDispatch) {
  if (!REXCVAR_GET(p2_skinning_local) || failed.load(std::memory_order_relaxed)) {
    __imp__sub_829F5978(ctx, base); return;
  }
  const uint32_t limit = REXCVAR_GET(p2_skinning_verify_calls);
  const uint32_t n = ctx.r5.u32;
  // The original pipeline requires two vertices; both odd/even valid tails are tested.
  // Unusual aliases use the original, since running twice would change the inputs.
  const uint32_t out = ctx.r3.u32, input = ctx.r4.u32;
  const uint32_t bbox = ctx.r7.u32 & ~15u, stack = ctx.r1.u32 - 32;
  const uint64_t logical_bytes = uint64_t(n)*24;
  const uint64_t vector_end = n ? ((uint64_t(out)+uint64_t(n-1)*24+16)&~uint64_t(15))+16 : out;
  const uint64_t bytes = std::max(uint64_t(out)+logical_bytes, vector_end)-out;
  if (n < 2 || n > 16384 || uint64_t(out)+bytes > 0x100000000ULL || Overlaps(out, bytes, input, uint64_t(n+2)*32) ||
      Overlaps(out, bytes, bbox, 32) || Overlaps(out, bytes, stack, 32) ||
      Overlaps(bbox, 32, input, uint64_t(n+2)*32)) {
    __imp__sub_829F5978(ctx, base); return;
  }
  // One pass over the input: finite positions (the generated kernels' validated domain) and the largest bone
  // index (the rewrite's palette rows). run59: the two separate scans were 4 % of a core.
  uint32_t max_index = 0;
  {
    bool finite = true;
    const uint8_t* v = base + input;
    for (uint32_t i = 0; i < n; ++i, v += 32) {
      uint32_t xyz[3];
      std::memcpy(xyz, v, 12);
      finite &= ((__builtin_bswap32(xyz[0]) & 0x7F800000u) != 0x7F800000u) &
                ((__builtin_bswap32(xyz[1]) & 0x7F800000u) != 0x7F800000u) &
                ((__builtin_bswap32(xyz[2]) & 0x7F800000u) != 0x7F800000u);
      uint16_t idx[4];
      std::memcpy(idx, v + 12, 8);
      const uint32_t a = std::max(__builtin_bswap16(idx[0]), __builtin_bswap16(idx[1]));
      const uint32_t b = std::max(__builtin_bswap16(idx[2]), __builtin_bswap16(idx[3]));
      max_index = std::max(max_index, std::max(a, b));
    }
    if (!finite) {
      __imp__sub_829F5978(ctx, base); return;
    }
  }
  if (REXCVAR_GET(p2_skin_dup_stats)) DupStats(base, input, n, ctx.r6.u32, max_index);
  if (REXCVAR_GET(p2_skinning_fast) && !fast_failed.load(std::memory_order_relaxed)) {
    // The rewrite reads the palette per vertex and writes each vertex at once; the original read a vertex's
    // matrices before writing the previous vertex. Same result unless the output overlaps the palette.
    const uint32_t palette = ctx.r6.u32;
    if (!Overlaps(out, bytes, palette & ~15u, uint64_t(max_index + 2) * 64)) {
      const bool approx = REXCVAR_GET(p2_skinning_approx);
      if (approx) {
        ctx.fpscr.enableFlushMode();
        if (REXCVAR_GET(p2_skin_cache) && p2skincache::Cacheable(n)) {
          // WORKLOG #116: identical calls (same input bytes, palette bytes and n) replay the bytes written last time.
          const uint64_t key = p2skincache::Key(base, input, n, palette, max_index);
          const uint64_t calls = p2skincache::g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
          p2skincache::g_vertices.fetch_add(n, std::memory_order_relaxed);
          if (calls % 200000 == 0)
            REXLOG_INFO("[p2-skin-cache] {} calls / {} vertices: {} hits / {} vertices replayed, {} stores",
                        calls, p2skincache::g_vertices.load(), p2skincache::g_hits.load(),
                        p2skincache::g_hit_vertices.load(), p2skincache::g_stores.load());
          if (p2skincache::Replay(base, key, n, out, ctx.r7.u32)) {
            p2skincache::StackWords(base, ctx.r7.u32, ctx.r1.u32, uint32_t(ctx.lr), p2skin::kBig);
            p2skincache::g_hits.fetch_add(1, std::memory_order_relaxed);
            p2skincache::g_hit_vertices.fetch_add(n, std::memory_order_relaxed);
            return;
          }
          p2skin::Skin(base, out, input, n, palette, ctx.r7.u32, ctx.r1.u32, uint32_t(ctx.lr), max_index,
                       ParallelRunner(), true);
          p2skincache::Store(base, key, n, out, ctx.r7.u32);
          return;
        }
        p2skin::Skin(base, out, input, n, palette, ctx.r7.u32, ctx.r1.u32, uint32_t(ctx.lr), max_index,
                     ParallelRunner(), true);
        return;
      }
      const uint32_t verify = REXCVAR_GET(p2_skinning_fast_verify);
      const uint32_t ticket = fast_checks.load(std::memory_order_relaxed) < verify
                                  ? fast_checks.fetch_add(1, std::memory_order_relaxed)
                                  : verify;
      if (ticket >= verify) {
        ctx.fpscr.enableFlushMode();
        p2skin::Skin(base, out, input, n, palette, ctx.r7.u32, ctx.r1.u32, uint32_t(ctx.lr), max_index, ParallelRunner());
        return;
      }
      std::array<Region, 3> regions;
      regions[0].Save(base, out, size_t(bytes));
      regions[1].Save(base, bbox, 32);
      regions[2].Save(base, stack, 32);
      PPCContext trial = ctx;
      p2_skinning_local(trial, base);
      for (auto& region : regions) {
        region.reference.assign(base + region.address, base + region.address + region.before.size());
        region.Restore(base);
      }
      ctx.fpscr.enableFlushMode();
      p2skin::Skin(base, out, input, n, palette, ctx.r7.u32, ctx.r1.u32, uint32_t(ctx.lr), max_index, ParallelRunner());
      bool equal = true;
      size_t first = 0;
      for (const auto& region : regions) {
        if (std::memcmp(base + region.address, region.reference.data(), region.reference.size()) != 0) {
          if (equal) {
            first = region.address;
            for (size_t j = 0; j < region.reference.size(); ++j)
              if (base[region.address + j] != region.reference[j]) { first = region.address + j; break; }
          }
          equal = false;
        }
      }
      if (!equal) {
        fast_failed.store(true, std::memory_order_relaxed);
        for (const auto& region : regions)
          std::memcpy(base + region.address, region.reference.data(), region.reference.size());
        ctx = trial;
        REXLOG_ERROR("[p2-skin-fast] MISMATCH call={} vertices={} first byte {:08X} - generated result kept, rewrite off",
                     ticket, n, first);
        return;
      }
      ctx = trial;  // registers as the generated kernel leaves them; memory proven identical
      if (ticket < 4 || ticket + 1 == verify)
        REXLOG_INFO("[p2-skin-fast] verify {} OK vertices={}", ticket, n);
      return;
    }
  }
  if (checks.load(std::memory_order_relaxed) >= limit) {
    p2_skinning_local(ctx, base); return;
  }
  const uint32_t ticket = checks.fetch_add(1, std::memory_order_relaxed);
  if (ticket >= limit) { p2_skinning_local(ctx, base); return; }
  PPCContext trial = ctx;
  const uint32_t host_start = trial.fpscr.getcsr();
  std::array<Region, 3> regions;
  regions[0].Save(base, out, size_t(bytes));
  regions[1].Save(base, bbox, 32);
  regions[2].Save(base, stack, 32);
  auto begin = std::chrono::steady_clock::now();
  __imp__sub_829F5978(ctx, base);
  auto mid = std::chrono::steady_clock::now();
  for (auto& region : regions) {
    region.reference.assign(base+region.address, base+region.address+region.before.size());
    region.Restore(base);
  }
  trial.fpscr.setcsr(host_start);
  auto fast_begin = std::chrono::steady_clock::now();
  p2_skinning_local(trial, base);
  auto end = std::chrono::steady_clock::now();
  bool equal = std::memcmp(&ctx, &trial, sizeof(ctx)) == 0;
  for (const auto& region : regions)
    equal &= std::memcmp(base+region.address, region.reference.data(), region.reference.size()) == 0;
  if (!equal) {
    failed.store(true, std::memory_order_relaxed);
    for (const auto& region : regions)
      std::memcpy(base+region.address, region.reference.data(), region.reference.size());
    ctx.fpscr.restoreGuestBits(ctx.fpscr.csr);
    REXLOG_ERROR("[p2-skin] DIFFERENTIAL MISMATCH call={} vertices={} - original restored, optimization disabled", ticket, n);
    return;
  }
  // Caller retains the original context, which was just proven byte-identical.
  REXLOG_INFO("[p2-skin] verify {} OK vertices={} original={} us local={} us", ticket, n,
      std::chrono::duration_cast<std::chrono::microseconds>(mid-begin).count(),
      std::chrono::duration_cast<std::chrono::microseconds>(end-fast_begin).count());
}


// Every call: after the routine (whichever path ran) wrote its vertices, tell the renderer (skin_write_log.h).
REXCVAR_DEFINE_BOOL(p2_skin_write_log, true, "Prototype2",
    "Skinned vertex buffers are known changed/unchanged from the skinning calls instead of hashing them in the "
    "renderer; false = content checks only");
extern "C" REX_FUNC(sub_829F5978) {
  const uint32_t out = ctx.r3.u32;
  const uint64_t n = ctx.r5.u32;
  P2SkinningDispatch(ctx, base);
  if (n && n <= 65536) {
    const uint64_t end = ((uint64_t(out) + (n - 1) * 24 + 16) & ~uint64_t(15)) + 16;
    if (REXCVAR_GET(p2_skin_write_log)) p2::skinlog::Record(out, uint32_t(end - out));
    p2::skinlog::MarkPages(out, uint32_t(end - out));
  }
}

// Runs before launching guest threads, on isolated memory, with the actual ARM64
// recompiled original (including the original constant helper and SIMD implementation).
REXCVAR_DEFINE_BOOL(p2_skinning_selftest, false, "Prototype2", "Run 128 isolated ARM64 differential cases before game boot");
#include <memory>
#include <malloc.h>
void prototype2_RunSkinningSelfTests() {
  if (!REXCVAR_GET(p2_skinning_selftest)) return;
  constexpr size_t size = 0x100000;
  auto release = [](uint8_t* p) { std::free(p); };
  std::unique_ptr<uint8_t, decltype(release)> a(static_cast<uint8_t*>(memalign(64,size)),release);
  std::unique_ptr<uint8_t, decltype(release)> b(static_cast<uint8_t*>(memalign(64,size)),release);
  if (!a || !b) {
    failed.store(true);
    REXLOG_ERROR("[p2-skin-selftest] allocation failed; original skinning retained");
    return;
  }
  PPCFPSCRRegister host{};
  const uint32_t saved_host = host.getcsr();
  uint32_t random_state=415608;
  auto random = [&]() { random_state ^= random_state<<13; random_state ^= random_state>>17; random_state ^= random_state<<5; return random_state; };
  auto put = [](uint8_t* memory, size_t address, uint32_t value) {
    value=__builtin_bswap32(value); std::memcpy(memory+address,&value,4);
  };
  const uint32_t counts[] = {2,3,4,7,16,31,32,129,256};
  uint64_t original_ns=0, local_ns=0;
  for (unsigned test=0;test<128;++test) {
    std::memset(a.get(),0xA5,size);
    PPCContext original{};
    for (size_t j=0;j<sizeof(original);++j) reinterpret_cast<uint8_t*>(&original)[j]=uint8_t(random());
    original.fpscr={}; original.fpscr.InitHost();
    const uint32_t n=counts[test%std::size(counts)];
    original.r1.u64=0xF0000; original.r3.u64=0x70000+(test%16);
    original.r4.u64=0x20000; original.r5.u64=n; original.r6.u64=0x10000;
    original.r7.u64=0xE0000; original.lr=0x82123456;
    for (unsigned i=0;i<128*16;++i) {
      float f=float(int32_t(random()%20001)-10000)/4096.0f;
      uint32_t bits; std::memcpy(&bits,&f,4); put(a.get(),0x10000+i*4,bits);
    }
    for (unsigned i=0;i<n+2;++i) {
      for (unsigned j=0;j<32;++j) a.get()[0x20000+i*32+j]=uint8_t(random());
      for (unsigned j=0;j<3;++j) {
        float f=float(int32_t(random()%20001)-10000)/32.0f;
        uint32_t bits; std::memcpy(&bits,&f,4); put(a.get(),0x20000+i*32+j*4,bits);
      }
      for (unsigned j=0;j<4;++j) {
        a.get()[0x20000+i*32+12+j*2]=0;
        a.get()[0x20000+i*32+13+j*2]=uint8_t(random()%128);
      }
    }
    std::memcpy(b.get(),a.get(),size);
    PPCContext trial=original;
    const uint32_t start_csr=original.fpscr.getcsr();
    auto t0=std::chrono::steady_clock::now();
    __imp__sub_829F5978(original,a.get());
    auto t1=std::chrono::steady_clock::now();
    trial.fpscr.setcsr(start_csr);
    auto t2=std::chrono::steady_clock::now();
    if (!P2SkinningFinitePositions(b.get()+trial.r4.u32,n)) {
      failed.store(true); host.setcsr(saved_host);
      REXLOG_ERROR("[p2-skin-selftest] invalid fixture {}",test); return;
    }
    p2_skinning_local(trial,b.get());
    auto t3=std::chrono::steady_clock::now();
    original_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count();
    local_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(t3-t2).count();
    if (std::memcmp(a.get(),b.get(),size) || std::memcmp(&original,&trial,sizeof(original))) {
      size_t mb=0,cb=0;
      while(mb<size && a.get()[mb]==b.get()[mb]) ++mb;
      while(cb<sizeof(original) && reinterpret_cast<uint8_t*>(&original)[cb]==reinterpret_cast<uint8_t*>(&trial)[cb]) ++cb;
      failed.store(true); host.setcsr(saved_host);
      REXLOG_ERROR("[p2-skin-selftest] FAIL case={} vertices={} memory_byte={} context_byte={}; original retained",test,n,mb,cb);
      return;
    }
  }
  host.setcsr(saved_host);
  REXLOG_INFO("[p2-skin-selftest] PASS 128 ARM64 cases, all memory and registers equal; original={} us local_with_domain_check={} us",
      original_ns/1000,local_ns/1000);
}
