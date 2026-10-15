// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace p2::native_probe {
// Bounded diagnostic-only histogram. Never controls rendering or discards work.
// Keys: path,width,height,source_format,dest_format,sample_x,sample_y,host_msaa,
// scale_bits,clamp,guest_destination,clear_flags.
using Key = std::array<uint64_t, 12>;
struct Bucket { Key key{}; uint64_t count = 0, pixels = 0; };
template <size_t Capacity = 128> struct Histogram {
  std::array<Bucket, Capacity> buckets{};
  size_t used = 0;
  uint64_t overflow_count = 0, overflow_pixels = 0;
  void Record(const Key& key, uint64_t pixels) {
    for (size_t i = 0; i < used; ++i) {
      if (buckets[i].key == key) {
        ++buckets[i].count;
        buckets[i].pixels += pixels;
        return;
      }
    }
    if (used < Capacity) buckets[used++] = {key, 1, pixels};
    else { ++overflow_count; overflow_pixels += pixels; }
  }
  void Reset() { used = 0; overflow_count = overflow_pixels = 0; }
};
}  // namespace p2::native_probe
