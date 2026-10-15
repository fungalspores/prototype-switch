// Prototype (4156084E) on Switch: deadlock between the game's main thread and its render thread.
//
// Found with the wait registry (runs 16-18, ХОД_РАБОТЫ.md):
//  - the main thread pushes render commands into a 16384-entry ring (0x82D8FEA0, sub_822E9760). When the ring
//    is full it asks the render thread to drain it (sub_8226D648 -> sub_8226D038(queue, 4)) and sleeps until
//    the render thread answers;
//  - while doing so it owns critical sections the render thread needs to get there: the scene lock CS_A
//    0x82D84264 (run 17, render thread in sub_822EB9A8) and others, e.g. 0x82DC99D0 after the pause menu
//    (run 18, render thread in sub_827E3040 while executing ring commands via sub_822E9A40).
// On the Xbox 360 the render thread keeps up and the ring is never full at those points. Here it is several
// times slower than the main thread.
//
// Fix: while the main thread sleeps for the drain, it lets go of every critical section it owns (all recursion
// levels, the SDK keeps a per-thread list) and takes them back afterwards, in the original order. It is blocked
// anyway, so nothing of its own runs meanwhile; the render thread can take what it needs, drain and wake it.
#include "generated/default/prototype_pch.h"

#include <atomic>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_BOOL(proto_drain_release_cs, true, "Prototype",
                    "While the main thread waits for the render thread to drain the command ring, release the "
                    "critical sections it holds (fixes the cutscene/transition/pause hangs)");

extern "C" uint32_t RexCsOwnedSnapshot(uint32_t* out, uint32_t max);

namespace {
uint32_t LoadBE32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}
std::atomic<uint32_t> g_releases{0};
}  // namespace

REX_EXTERN(__imp__sub_8226D648);
REX_EXTERN(__imp__sub_822E9A40);

// The ring consumer (render thread, sub_8226CA48 -> sub_822E9A40) executes commands that push new commands
// into the same ring (run 20: sub_82715C40 -> sub_822C6E08 -> sub_822E9890). If the main thread filled the ring
// meanwhile, the render thread asks the render thread - itself - to drain and waits forever. Whoever is inside
// the drain loop does the drain itself instead: the nested sub_822E9A40 executes the queued commands in order,
// exactly what the drain message would make this thread do.
namespace {
thread_local int t_drain_depth = 0;
std::atomic<uint32_t> g_self_drains{0};
}
extern "C" REX_FUNC(sub_822E9A40) {
  ++t_drain_depth;
  __imp__sub_822E9A40(ctx, base);
  --t_drain_depth;
}
REX_EXTERN(__imp__RtlEnterCriticalSection);
REX_EXTERN(__imp__RtlLeaveCriticalSection);

extern "C" REX_FUNC(sub_8226D648) {
  if (t_drain_depth > 0 && REXCVAR_GET(proto_drain_release_cs)) {
    const uint32_t k = g_self_drains.fetch_add(1, std::memory_order_relaxed) + 1;
    if (k <= 8 || k % 100 == 0)
      REXLOG_INFO("[proto-deadlock] ring full inside the render thread's own drain: draining in place ({} time(s))", k);
    const uint64_t saved_lr = ctx.lr;
    if (t_drain_depth < 8) sub_822E9A40(ctx, base);
    ctx.lr = saved_lr;
    return;
  }
  uint32_t cs[32], levels[32], n = 0;
  if (REXCVAR_GET(proto_drain_release_cs)) {
    auto* self = rex::system::XThread::GetCurrentThread();
    const uint32_t me = self ? self->guest_object() : 0;
    const uint32_t owned = RexCsOwnedSnapshot(cs, 32);
    for (uint32_t i = 0; i < owned && me; ++i) {
      // X_RTL_CRITICAL_SECTION: +0x14 recursion count, +0x18 owner KTHREAD.
      if (LoadBE32(base, cs[i] + 0x18) != me) continue;
      const uint32_t r = LoadBE32(base, cs[i] + 0x14);
      if (r == 0 || r > 64) continue;
      cs[n] = cs[i];
      levels[n] = r;
      ++n;
    }
  }
  if (!n) {
    __imp__sub_8226D648(ctx, base);
    return;
  }
  const uint64_t saved_lr = ctx.lr;
  for (uint32_t i = n; i-- > 0;) {  // newest first
    for (uint32_t k = 0; k < levels[i]; ++k) {
      ctx.r3.u64 = cs[i];
      __imp__RtlLeaveCriticalSection(ctx, base);
    }
  }
  const uint32_t count = g_releases.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 8 || count % 100 == 0) {
    REXLOG_INFO("[proto-deadlock] ring full: main thread released {} critical section(s) (first {:08X}) while the "
                "render thread drains; {} time(s) so far", n, cs[0], count);
  }
  ctx.lr = saved_lr;
  __imp__sub_8226D648(ctx, base);
  for (uint32_t i = 0; i < n; ++i) {  // original order
    for (uint32_t k = 0; k < levels[i]; ++k) {
      ctx.r3.u64 = cs[i];
      __imp__RtlEnterCriticalSection(ctx, base);
    }
  }
  ctx.lr = saved_lr;
}
