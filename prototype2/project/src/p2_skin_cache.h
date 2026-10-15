// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2 (WORKLOG #116/#117): skinning results reused for identical calls. run84: 10 % of the skinned vertices
// repeated a call of the same frame and 28 % one of the previous frame (same mesh data, vertex count and bone
// palette contents). The key hashes every byte the rewrite reads (input vertices, palette rows) plus n; a hit
// writes back the exact bytes the rewrite wrote last time (output vertices, bounds, stack words), so the result is
// byte-identical to running it.
// v2 (run86: v1's map allocations were 9 % of a core in malloc/free): direct-mapped slots whose buffers are reused,
// a spin lock per slot, and a result is stored only for a key already seen once (one-off calls cost no copy).
#pragma once
#include <xxhash.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace p2skincache {

constexpr uint32_t kSlots = 1024;
constexpr uint32_t kMaxBytes = 48 * 1024;  // bigger meshes (> 2000 vertices) are not cached (memory <= 48 MB)

struct Slot {
  std::atomic<bool> busy{false};
  uint64_t key = 0;
  uint32_t n = 0;
  std::vector<uint8_t> out;
  uint8_t bounds[32];
};
inline Slot g_slots[kSlots];
inline std::atomic<uint64_t> g_seen[4096];
inline std::atomic<uint64_t> g_calls{0}, g_hits{0}, g_hit_vertices{0}, g_vertices{0}, g_stores{0};

inline uint64_t Key(const uint8_t* base, uint32_t in, uint32_t n, uint32_t palette, uint32_t max_index) {
  uint64_t h = XXH3_64bits_withSeed(base + in, size_t(n) * 32, 0x5C1A ^ n);
  return XXH3_64bits_withSeed(base + (palette & ~15u), size_t(max_index + 1) * 64 + 16, h) | 1;
}
inline uint32_t OutBytes(uint32_t n) { return (n - 1) * 24 + 32; }
inline bool Cacheable(uint32_t n) { return OutBytes(n) <= kMaxBytes; }

// The prologue's stack words (as p2skin::Skin writes them).
inline void StackWords(uint8_t* base, uint32_t bbox, uint32_t sp, uint32_t lr, uint32_t big) {
  auto store = [&](uint32_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(base + a, &v, 4); };
  store(sp - 16, bbox);
  store(sp - 8, lr);
  for (int i = 0; i < 4; ++i) store(sp - 32 + 4 * i, big);
}

struct Lock {
  Slot& s;
  bool held;
  explicit Lock(Slot& slot) : s(slot), held(!slot.busy.exchange(true, std::memory_order_acquire)) {}
  ~Lock() { if (held) s.busy.store(false, std::memory_order_release); }
};

inline bool Replay(uint8_t* base, uint64_t key, uint32_t n, uint32_t out, uint32_t bbox) {
  Slot& s = g_slots[(key >> 20) % kSlots];
  Lock lock(s);
  if (!lock.held || s.key != key || s.n != n) return false;  // busy: just compute
  std::memcpy(base + out, s.out.data(), OutBytes(n));
  std::memcpy(base + (bbox & ~15u), s.bounds, 16);
  std::memcpy(base + (uint32_t(bbox + 16) & ~15u), s.bounds + 16, 16);
  return true;
}

inline void Store(const uint8_t* base, uint64_t key, uint32_t n, uint32_t out, uint32_t bbox) {
  // Second chance: only keys seen before are worth a copy.
  std::atomic<uint64_t>& seen = g_seen[(key >> 40) % 4096];
  if (seen.exchange(key, std::memory_order_relaxed) != key) return;
  Slot& s = g_slots[(key >> 20) % kSlots];
  Lock lock(s);
  if (!lock.held) return;
  const uint32_t bytes = OutBytes(n);
  if (s.out.size() < bytes) s.out.resize(bytes);  // grows only; reused afterwards
  std::memcpy(s.out.data(), base + out, bytes);
  std::memcpy(s.bounds, base + (bbox & ~15u), 16);
  std::memcpy(s.bounds + 16, base + (uint32_t(bbox + 16) & ~15u), 16);
  s.key = key;
  s.n = n;
  g_stores.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace p2skincache
