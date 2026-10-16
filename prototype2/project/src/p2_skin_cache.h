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

inline uint64_t InputHash(const uint8_t* base, uint32_t in, uint32_t n) {
  return XXH3_64bits_withSeed(base + in, size_t(n) * 32, 0x5C1A ^ n);
}
inline uint64_t Key(const uint8_t* base, uint64_t input_hash, uint32_t palette, uint32_t max_index) {
  return XXH3_64bits_withSeed(base + (palette & ~15u), size_t(max_index + 1) * 64 + 16, input_hash) | 1;
}

// WORKLOG #122: the per-call input scan (finite positions, largest bone index) remembered per input contents.
// One word per slot: 48-bit tag of (input hash, address) | 16-bit largest bone index; only finite inputs stored.
inline std::atomic<uint64_t> g_scan[4096];
inline bool ScanLookup(uint64_t input_hash, uint32_t in, uint32_t* max_index) {
  const uint64_t tag = (input_hash ^ (uint64_t(in) * 0x9E3779B97F4A7C15ull)) & ~uint64_t(0xFFFF);
  const uint64_t w = g_scan[(tag >> 16) % 4096].load(std::memory_order_relaxed);
  if ((w & ~uint64_t(0xFFFF)) != tag || tag == 0) return false;
  *max_index = uint32_t(w & 0xFFFF);
  return true;
}
inline void ScanStore(uint64_t input_hash, uint32_t in, uint32_t max_index) {
  const uint64_t tag = (input_hash ^ (uint64_t(in) * 0x9E3779B97F4A7C15ull)) & ~uint64_t(0xFFFF);
  if (tag == 0 || max_index > 0xFFFF) return;
  g_scan[(tag >> 16) % 4096].store(tag | max_index, std::memory_order_relaxed);
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

// WORKLOG #125: crowd characters animated at half rate. run95 trace: palette addresses are stable per character
// across frames, and ped models (input address + n) are shared by several palettes; the player and unique characters
// use their own models. For a mesh of a model seen with two or more palettes recently ("crowd"), every other call
// writes back the bytes its previous call wrote (one frame old pose) instead of skinning.
namespace p2halfrate {

constexpr uint32_t kSlots = 2048;
constexpr uint32_t kMaxBytes = 96 * 1024;

struct Slot {
  std::atomic<bool> busy{false};
  uint64_t key = 0;
  uint32_t frame = 0;   // renderer frame of the last call
  bool skipped = false;  // the last call replayed
  bool valid = false;
  uint32_t bytes = 0;
  std::vector<uint8_t> out;
  uint8_t bounds[32];
};
inline Slot g_slots[kSlots];

struct Model { std::atomic<uint64_t> word{0}; };  // input address << 32 | palette (low bits) of the last caller
inline Model g_models[4096];
inline std::atomic<uint64_t> g_crowd[4096];  // input address << 32 | frame until which it counts as crowd

inline std::atomic<uint64_t> g_calls{0}, g_replayed{0}, g_replayed_vertices{0}, g_vertices{0};

inline uint64_t Identity(uint32_t in, uint32_t n, uint32_t palette) {
  uint64_t h = (uint64_t(in) << 32 | palette) * 0x9E3779B97F4A7C15ull;
  h ^= uint64_t(n) * 0xC2B2AE3D27D4EB4Full;
  return (h ^ (h >> 31)) | 1;
}

// A model drawn with another palette within 30 frames is a crowd model for the next 300 frames.
inline bool Crowd(uint32_t in, uint32_t n, uint32_t palette, uint32_t frame) {
  const uint32_t slot = ((in * 2654435761u) ^ n) % 4096;
  const uint64_t word = g_models[slot].word.exchange(uint64_t(in) << 32 | palette, std::memory_order_relaxed);
  if (uint32_t(word >> 32) == in && uint32_t(word) != palette)
    g_crowd[slot].store(uint64_t(in) << 32 | (frame + 300), std::memory_order_relaxed);
  const uint64_t crowd = g_crowd[slot].load(std::memory_order_relaxed);
  return uint32_t(crowd >> 32) == in && int32_t(uint32_t(crowd) - frame) > 0;
}

struct Lock {
  Slot& s;
  bool held;
  explicit Lock(Slot& slot) : s(slot), held(!slot.busy.exchange(true, std::memory_order_acquire)) {}
  ~Lock() { if (held) s.busy.store(false, std::memory_order_release); }
};

// Every other call of an identity whose previous call was within 3 frames and computed: write those bytes back.
inline bool TryReplay(uint8_t* base, uint64_t id, uint32_t n, uint32_t out, uint32_t bbox, uint32_t frame) {
  Slot& s = g_slots[(id >> 20) % kSlots];
  Lock lock(s);
  if (!lock.held || !s.valid || s.key != id || s.skipped) return false;
  if (uint32_t(frame - s.frame) > 3) return false;
  const uint32_t bytes = (n - 1) * 24 + 32;
  if (bytes != s.bytes) return false;
  std::memcpy(base + out, s.out.data(), bytes);
  std::memcpy(base + (bbox & ~15u), s.bounds, 16);
  std::memcpy(base + (uint32_t(bbox + 16) & ~15u), s.bounds + 16, 16);
  s.skipped = true;
  s.frame = frame;
  return true;
}

// After a computed call: remember its bytes for the next call.
inline void Remember(const uint8_t* base, uint64_t id, uint32_t n, uint32_t out, uint32_t bbox, uint32_t frame) {
  const uint32_t bytes = (n - 1) * 24 + 32;
  if (bytes > kMaxBytes) return;
  Slot& s = g_slots[(id >> 20) % kSlots];
  Lock lock(s);
  if (!lock.held) return;
  if (s.out.size() < bytes) s.out.resize(bytes);
  std::memcpy(s.out.data(), base + out, bytes);
  std::memcpy(s.bounds, base + (bbox & ~15u), 16);
  std::memcpy(s.bounds + 16, base + (uint32_t(bbox + 16) & ~15u), 16);
  s.key = id;
  s.bytes = bytes;
  s.frame = frame;
  s.skipped = false;
  s.valid = true;
}

}  // namespace p2halfrate
