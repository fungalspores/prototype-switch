// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2 (WORKLOG #93): where a frame's time goes between the game's two frame threads.
// run63/65: the render/simulation thread (pinned core 0) waits in sub_821EC950 (pop from a queue Main XThread
// fills with sub_821DF540, which blocks when it is full) and Main XThread waits on an event in sub_82283888.
// Moving 45 % of the skinning off the render thread (run65) did not change the frame rate, so the frame is set
// by this hand-off. Every 10 s: per frame, the time each side spent in these three calls and how often.
#include "generated/default/prototype2_pch.h"
#include <rex/logging.h>
#include <atomic>
#include <chrono>

REX_EXTERN(__imp__sub_821EC950);
REX_EXTERN(__imp__sub_82283888);
REX_EXTERN(__imp__sub_821DF540);

namespace {
struct Counter {
  std::atomic<uint64_t> ns{0}, calls{0}, max_ns{0};
  void Add(uint64_t d) {
    ns.fetch_add(d, std::memory_order_relaxed);
    calls.fetch_add(1, std::memory_order_relaxed);
    uint64_t m = max_ns.load(std::memory_order_relaxed);
    while (d > m && !max_ns.compare_exchange_weak(m, d, std::memory_order_relaxed)) {}
  }
};
Counter g_pop, g_event, g_push;
std::atomic<uint32_t> g_event_object{0};
inline uint64_t Now() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace

extern "C" REX_FUNC(sub_821EC950) {
  const uint64_t t0 = Now();
  __imp__sub_821EC950(ctx, base);
  g_pop.Add(Now() - t0);
}

extern "C" REX_FUNC(sub_82283888) {
  uint32_t expected = 0;
  if (g_event_object.compare_exchange_strong(expected, ctx.r3.u32))
    REXLOG_INFO("[p2-timeline] Main XThread's frame wait object {:08X}", ctx.r3.u32);
  const uint64_t t0 = Now();
  __imp__sub_82283888(ctx, base);
  g_event.Add(Now() - t0);
}

extern "C" REX_FUNC(sub_821DF540) {
  const uint64_t t0 = Now();
  __imp__sub_821DF540(ctx, base);
  g_push.Add(Now() - t0);
}

// Once per guest frame (flip hook).
extern "C" void p2_timeline_frame() {
  static uint64_t last = 0, frames = 0;
  const uint64_t now = Now();
  if (!last) last = now;
  ++frames;
  if (now - last < 10000000000ull) return;
  auto take = [](Counter& c, uint64_t& ns, uint64_t& calls, uint64_t& max) {
    ns = c.ns.exchange(0, std::memory_order_relaxed);
    calls = c.calls.exchange(0, std::memory_order_relaxed);
    max = c.max_ns.exchange(0, std::memory_order_relaxed);
  };
  uint64_t a, an, am, b, bn, bm, c, cn, cm;
  take(g_pop, a, an, am);
  take(g_event, b, bn, bm);
  take(g_push, c, cn, cm);
  const double f = double(frames), ms = 1e-6;
  REXLOG_INFO("[p2-timeline] {:.1f} ms/frame over {} frames: render thread queue pop {:.1f} ms ({:.1f} calls, "
              "max {:.1f}); Main frame wait {:.1f} ms ({:.1f} calls, max {:.1f}); Main queue push {:.1f} ms "
              "({:.1f} calls, max {:.1f})",
              double(now - last) * ms / f, frames, a * ms / f, an / f, am * ms, b * ms / f, bn / f, bm * ms,
              c * ms / f, cn / f, cm * ms);
  last = now;
  frames = 0;
}
