// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace p2::native {
// A missing grading LUT blacks out the whole 3D composite, unlike a missing
// ordinary material. Keep this exception bounded to small RGBA cube LUTs.
inline bool IsSmallColorLut(bool volume, bool rgba8, uint32_t width, uint32_t height,
                           uint32_t depth, uint64_t bytes) {
  return volume && rgba8 && width && width==height && height==depth &&
         width<=32 && bytes && bytes<=256*1024;
}

// A long map pause ages scene materials, but it must not destroy them unless
// the cache also exceeds its payload budget. Pending jobs cannot be retired.
inline bool IdleTextureExpired(uint64_t frame, uint64_t last_used, bool streaming) {
  return !streaming && frame > last_used && frame - last_used > 600;
}

// Invalidates CPU references before a retired image may be destroyed or reused.
// View handles alone are insufficient: a memo also owns a raw Texture pointer.
template <typename Memo, typename IsEvicted>
bool InvalidateEvictedTextureMemo(Memo& memo, IsEvicted evicted) {
  if (!memo.texture || !evicted(memo.texture)) return false;
  memo.view = {};
  memo.texture = nullptr;
  memo.valid_until = 0;
  return true;
}
}  // namespace p2::native
