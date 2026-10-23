// Prototype (4156084E) on Switch: priority and core of the game's render thread.
//
// Found with the all-threads stack sampler (2026-10-19/20): in frames over 50 ms the game's main thread
// waits ~30% of the time for its render thread (tid 23, entry sub_8226DD70 -> sub_8226CA48, the consumer
// of the 16384-entry command ring), which works ~78% of those frames. Every guest thread runs at the same
// Horizon priority (REX_SWITCH_PRIO_GUEST, 0x3B), so the 12 job workers and the other guest threads
// preempt it. This wrapper raises the render thread's priority once, when the thread starts, and can
// pin it to a core mask.
//
// sub_8226DD70 is the thread's entry (the start routine passed to ExCreateThread through sub_82236A90);
// it is entered only by that thread, once, and does not return while the game runs.
#include "generated/default/prototype_pch.h"

#include <atomic>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(proto_render_thread_priority, 0x3A, "Prototype",
                      "Horizon priority of the game's render thread (0x2C-0x3F; lower = more urgent; guest threads "
                      "run at 0x3B). 0 = unchanged")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(proto_render_thread_mask, 0, "Prototype",
                      "Core mask of the game's render thread (e.g. 0x8 = core 3). 0 = unchanged")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" void RexSwitchSetCurrentThreadPriority(int priority);
extern "C" bool RexSwitchSetCurrentThreadMask(uint32_t mask);

REX_EXTERN(__imp__sub_8226DD70);

namespace {
std::atomic<bool> g_set{false};
}

extern "C" REX_FUNC(sub_8226DD70) {
  if (!g_set.exchange(true)) {
    const uint32_t priority = REXCVAR_GET(proto_render_thread_priority);
    const uint32_t mask = REXCVAR_GET(proto_render_thread_mask) & 0xFu;
    if (priority >= 0x2C && priority <= 0x3F) RexSwitchSetCurrentThreadPriority(int(priority));
    const bool mask_ok = mask ? RexSwitchSetCurrentThreadMask(mask) : true;
    REXLOG_INFO("[proto-render-thread] priority {:#x}, core mask {:#x}{}", priority, mask,
                mask_ok ? "" : " (refused)");
  }
  __imp__sub_8226DD70(ctx, base);
}
