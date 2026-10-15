// Prototype 2 (Switch): priority of the game's simulation thread.
//
// run49/run52 profiles: the three cores are ~93 % busy (281 % of 300 %) and the thread that runs the game's main
// loop (sub_821A84F8 -> sub_8231E4B8 every frame) is the critical path, yet it got only ~70 % of a core: it ran at
// the same Horizon priority (0x3B) as the ~16 job workers that compete with it. The patched generated code calls
// p2_on_sim_thread_start() once when that loop starts; this raises that one thread above the other guest threads
// (still below the host present/audio threads at 0x2C/0x2B), so the frame's serial work is not time-sliced away.
#include "generated/default/prototype2_pch.h"
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdint>
#include <rex/cvar.h>
#include <rex/logging.h>

extern "C" bool RexSwitchSetCurrentThreadPriorityOk(int priority);  // SDK threading_switch
extern "C" bool RexSwitchPinCurrentThreadToCore(int core);           // SDK threading_switch

REXCVAR_DEFINE_UINT32(p2_sim_thread_priority, 0x3A, "Prototype2",
                      "Horizon priority for the game's main simulation thread (lower = higher priority; guest "
                      "threads run at 0x3B). 0 = leave it at 0x3B")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(p2_sim_thread_core, 0, "Prototype2",
    "Core the game's main simulation thread is pinned to (the host service threads keep off it, "
    "p2_host_core_mask). -1 = not pinned")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" void p2_on_sim_thread_start() {
  static thread_local bool done = false;
  if (done) return;
  done = true;
  const int32_t core = REXCVAR_GET(p2_sim_thread_core);
  if (core >= 0 && core <= 2) {
    const bool pinned = RexSwitchPinCurrentThreadToCore(core);
    REXLOG_INFO("[sched] simulation thread pinned to core {}: {}", core, pinned ? "yes" : "REFUSED");
  }
  const uint32_t priority = REXCVAR_GET(p2_sim_thread_priority);
  if (!priority) {
    REXLOG_INFO("[sched] simulation thread priority unchanged (0x3B)");
    return;
  }
  const bool ok = RexSwitchSetCurrentThreadPriorityOk(int(priority));
  REXLOG_INFO("[sched] simulation thread priority 0x{:02X}: {}", priority, ok ? "set" : "REFUSED");
}

// Guest D3D flip scheduling (sub_82C746D8, hook inserted by tools/prepare_switch_codegen.py). At each swap the
// game's D3D picks the vblank its flip happens at: last flip + present interval, or the next vblank when the
// swap arrives "late" in the current one. Queued flips make the next frame wait for that vblank, so 35-60 ms of
// work became 50/67/83 ms frames. The real display is paced by the host presenter (Vulkan FIFO), so the guest
// flip can happen at once: target = the current vblank count, which takes D3D's immediate-flip path.
REXCVAR_DEFINE_BOOL(p2_immediate_flip, true, "Prototype2",
    "Flip the guest's frontbuffer at the swap instead of at a later vblank (the screen is paced by the host "
    "presenter). false = the game's own present interval / late-swap wait")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_UINT32(p2_min_present_interval, 2, "Prototype2",
    "With p2_immediate_flip: a frame faster than this many vblanks still waits for that vblank (2 = at most "
    "30 fps, 1 = 60); only slower frames flip at once instead of at the next vblank")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" void p2_quality_frame();  // switch_quality.cpp
extern "C" void p2_timeline_frame();  // switch_timeline.cpp

// WORKLOG #91: D3D's wait for ring-buffer space (sub_822A11A0 loops on sub_82C73C08: 32 db16cyc pauses, a GPU
// hang check, returns 1 = keep waiting) spun the simulation thread (run62: 15.6 % of a core, on its own core 0).
// A real sleep instead lets the job workers (guest threads, cores 0-2, lower priority) run the skinning jobs on
// core 0 meanwhile (nanosleep -> svcSleepThread); a yield would only yield to equal priority.
REXCVAR_DEFINE_UINT32(p2_ring_wait_sleep_us, 50, "Prototype2",
                      "Sleep per step of the game's wait for GPU ring space, in microseconds (0 = spin as on "
                      "the Xbox 360)");
REX_EXTERN(__imp__sub_82C73C08);
extern "C" REX_FUNC(sub_82C73C08) {
  __imp__sub_82C73C08(ctx, base);
  const uint32_t us = REXCVAR_GET(p2_ring_wait_sleep_us);
  if (ctx.r3.u32 == 1 && us) std::this_thread::sleep_for(std::chrono::microseconds(us));
}

extern "C" uint32_t p2_flip_target(uint32_t target, uint32_t now, uint32_t interval, uint32_t threshold,
                                   uint32_t last_flip) {
  static uint32_t swaps = 0, queued = 0, capped = 0, last_interval = 0xFFFFFFFFu, last_threshold = 0xFFFFFFFFu;
  ++swaps;
  if (target != now) ++queued;
  if (interval != last_interval || threshold != last_threshold) {
    last_interval = interval;
    last_threshold = threshold;
    REXLOG_INFO("[flip] present interval {} late threshold {}% (target {} now {} last {})", interval,
                threshold, target, now, last_flip);
  }
  uint32_t result = target;
  if (REXCVAR_GET(p2_immediate_flip)) {
    const uint32_t cap = last_flip + std::max<uint32_t>(interval, REXCVAR_GET(p2_min_present_interval));
    if (int32_t(cap - now) > 0) {
      result = cap;  // faster than the cap: flip at that vblank (frame rate limit, even pacing)
      ++capped;
    } else {
      result = now;  // slower: at once, not rounded up to the next vblank
    }
  }
  if (swaps % 600 == 0) {
    REXLOG_INFO("[flip] last 600 swaps: {} the game queued for a later vblank, {} held by the frame cap{}",
                queued, capped, REXCVAR_GET(p2_immediate_flip) ? "" : " (immediate flip off)");
    queued = 0;
    capped = 0;
  }
  p2_quality_frame();
  p2_timeline_frame();
  return result;
}
