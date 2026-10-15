// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2: which vertex ranges the skinning routine (sub_829F5978 hook, switch_skinning.cpp) just wrote
// (WORKLOG #85). The renderer then knows a skinned vertex buffer changed (or did not) without hashing it:
// run58 XXH3 was 11 % of a core, mostly re-checking skinned meshes. Keyed by the output's physical address;
// an entry not refreshed for 2 renderer frames is ignored (the memory may hold something else by then) and
// the renderer falls back to its content checks.
#pragma once
#include <atomic>
#include <cstdint>

namespace p2::skinlog {

struct Entry {
  std::atomic<uint64_t> key{0};  // physical address << 32 | write sequence
  std::atomic<uint32_t> bytes{0};
  std::atomic<uint32_t> frame{0};
};
inline Entry g_table[8192];
inline std::atomic<uint32_t> g_sequence{1};
inline std::atomic<uint32_t> g_frame{0};  // the renderer's frame, published at frame start

inline uint32_t Slot(uint32_t physical) { return (physical * 2654435761u) >> 19; }

// After the routine wrote [guest_address, guest_address + bytes).
inline void Record(uint32_t guest_address, uint32_t bytes) {
  const uint32_t physical = guest_address & 0x1FFFFFFFu;
  const uint32_t sequence = g_sequence.fetch_add(1, std::memory_order_relaxed);
  Entry& e = g_table[Slot(physical)];
  e.bytes.store(bytes, std::memory_order_relaxed);
  e.frame.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
  e.key.store((uint64_t(physical) << 32) | sequence, std::memory_order_release);
}

// A range the renderer reads: the write sequence of a recent skinning write covering it from its start.
inline bool Lookup(uint32_t physical, uint32_t bytes, uint32_t frame, uint32_t* sequence) {
  const Entry& e = g_table[Slot(physical)];
  const uint64_t key = e.key.load(std::memory_order_acquire);
  if (uint32_t(key >> 32) != physical) return false;
  if (e.bytes.load(std::memory_order_relaxed) < bytes) return false;
  if (frame - e.frame.load(std::memory_order_relaxed) > 2) return false;
  *sequence = uint32_t(key);
  return true;
}

// WORKLOG #95: 4 KB guest physical pages the skinning routine wrote, stamped with the renderer frame (+1).
// Whatever the exact range layout, a vertex range on these pages holds animated data, so the renderer
// never trusts sampled or skipped checks for it (cutscene twitch, run78).
inline std::atomic<uint32_t> g_page_frame[0x20000];
inline void MarkPages(uint32_t guest_address, uint32_t bytes) {
  if (!bytes) return;
  // 0xE0000000+ views sit 4 KB above their physical pages.
  const uint32_t physical = ((guest_address & 0x1FFFFFFFu) + (guest_address >= 0xE0000000u ? 0x1000u : 0u)) & 0x1FFFFFFFu;
  const uint32_t stamp = g_frame.load(std::memory_order_relaxed) + 1;
  const uint32_t last = (physical + bytes - 1) & 0x1FFFFFFFu;
  for (uint32_t page = physical >> 12; page <= (last >> 12); ++page)
    g_page_frame[page].store(stamp, std::memory_order_relaxed);
}
// Any page of the range skinned within the last `window` frames.
inline bool PagesSkinned(uint32_t physical, uint32_t bytes, uint32_t frame, uint32_t window) {
  physical &= 0x1FFFFFFFu;
  const uint32_t last = (physical + (bytes ? bytes - 1 : 0)) & 0x1FFFFFFFu;
  for (uint32_t page = physical >> 12; page <= (last >> 12); ++page) {
    const uint32_t stamp = g_page_frame[page].load(std::memory_order_relaxed);
    // The game may skin a frame before the renderer's counter reaches it: stamps slightly ahead count too.
    if (stamp && int32_t(frame + 1 - stamp) <= int32_t(window) && int32_t(frame + 1 - stamp) >= -4) return true;
  }
  return false;
}

}  // namespace p2::skinlog
