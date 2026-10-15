// Counters of the game's frame pacing waits (guest_swap_wait.h), read by the
// native renderer's slow-frame report.
#pragma once

#include <atomic>
#include <cstdint>

namespace p2::pacing {
// Time the render thread slept in the presentation-interval wait.
inline std::atomic<uint64_t> g_vblank_wait_ns{0};
// Late frames let through without waiting for the next vblank.
inline std::atomic<uint64_t> g_late_skips{0};
}  // namespace p2::pacing
