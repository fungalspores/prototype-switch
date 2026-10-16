// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace p2::native {
class OcclusionCommandState {
 public:
  static constexpr uint32_t invalid = UINT32_MAX;
  template <typename End, typename Begin>
  void Set(uint32_t next, End end, Begin begin) {
    if (next == current_) return;
    Close(end);
    if (next != invalid) { begin(next); current_ = next; }
  }
  template <typename End> void Close(End end) {
    if (current_ != invalid) { end(current_); current_ = invalid; }
  }
 private:
  uint32_t current_ = invalid;
};
}  // namespace p2::native
