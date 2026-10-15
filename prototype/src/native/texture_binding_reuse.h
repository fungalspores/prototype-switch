// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <array>
#include <algorithm>
namespace p2::native {
enum class BindingReuse : uint32_t {
  Hit, NewEntry, Identity, DescriptorGeneration, AnswersEpoch, DeadViews, Expired, Count
};
// Keep the old cache predicate's order, including its lazy view validation.
// This classifies misses without changing invalidation or image contents.
template <typename ValidateViews>
BindingReuse CheckBindingReuse(bool valid, bool identity, bool generation,
                              bool answers, ValidateViews&& views,
                              bool same_frame, bool same_submission,
                              uint64_t valid_until, uint64_t frame) {
  if (!valid) return BindingReuse::NewEntry;
  if (!identity) return BindingReuse::Identity;
  if (!generation) return BindingReuse::DescriptorGeneration;
  if (!answers) return BindingReuse::AnswersEpoch;
  if (!views()) return BindingReuse::DeadViews;
  if (!((same_frame && same_submission) || valid_until >= frame))
    return BindingReuse::Expired;
  return BindingReuse::Hit;
}
// Candidate metadata is committed only after every view has been refreshed.
// The sets may stay only when handles AND never-reused live view IDs match.
struct BindingRefresh {
  static constexpr uint32_t Capacity = 24;
  bool valid = true;
  uint32_t view_count = 0, texture_count = 0;
  uint64_t valid_until = UINT64_MAX;
  std::array<void*, Capacity> textures{};
  bool Observe(bool same_handle, uint64_t old_id, uint64_t new_id,
               void* texture, uint64_t until) {
    if (!valid || view_count >= Capacity || !same_handle || !new_id || old_id != new_id)
      return valid = false;
    ++view_count;
    valid_until = std::min(valid_until, until);
    if (texture) {
      if (texture_count >= Capacity) return valid = false;
      textures[texture_count++] = texture;
    }
    return true;
  }
  bool Complete(uint32_t previous_count) const {
    return valid && view_count == previous_count && previous_count <= Capacity;
  }
};
}