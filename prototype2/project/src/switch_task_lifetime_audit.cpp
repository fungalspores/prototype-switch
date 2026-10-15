// SPDX-License-Identifier: GPL-3.0-or-later
#include "generated/default/prototype2_pch.h"
#include "task_history_ring.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <chrono>
#include <mutex>
#include <thread>
#if defined(P2_TASK_LIFETIME_AUDIT) && P2_TASK_LIFETIME_AUDIT
REXCVAR_DEFINE_BOOL(p2_task_lifetime_audit, false, "Prototype2", "Record task base-vtable writes and queue returns in bounded in-memory histories");
REX_EXTERN(__imp__sub_822DA288);
namespace {
struct Event {
  uint64_t sequence=0, us=0, thread=0;
  const char* function=nullptr;
  void* context=nullptr;
  uint32_t object=0, vtable=0, lr=0, sp=0, queue=0;
};
std::mutex history_mutex;
P2HistoryRing<Event,65536> writes;
P2HistoryRing<Event,4096> pops;
Event Snapshot(PPCContext& ctx,uint32_t object,uint32_t vt,const char* function,uint32_t queue=0) {
  return {0,uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()),
    uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id())),function,&ctx,object,vt,uint32_t(ctx.lr),ctx.r1.u32,queue};
}
}
void P2TaskBaseWrite(PPCContext& ctx,uint8_t* base,uint32_t object,const char* function) {
  if (!REXCVAR_GET(p2_task_lifetime_audit)) return;
  const auto event=Snapshot(ctx,object,REX_LOAD_U32(object),function);
  std::lock_guard lock(history_mutex); writes.Push(event);
}
void P2DumpTaskHistory(uint32_t object) {
  std::lock_guard lock(history_mutex);
  REXLOG_ERROR("[p2-history] bounded history base_writes={} capacity=65536 queue_returns={} capacity=4096 object={:08X}",writes.Count(),pops.Count(),object);
  auto show=[](const Event& e) {
    REXLOG_ERROR("[p2-history] seq={} us={} thread={} ctx={} function={} object={:08X} observed_vtable={:08X} lr={:08X} sp={:08X} queue={:08X}",e.sequence,e.us,e.thread,e.context,e.function,e.object,e.vtable,e.lr,e.sp,e.queue);
  };
  const auto nw=writes.Visit(object,show), np=pops.Visit(object,show);
  REXLOG_ERROR("[p2-history] matching writes={} returns={}; missing history may have been evicted; sequence numbers are per history",nw,np);
}
extern "C" REX_FUNC(sub_822DA288) {
  if (!REXCVAR_GET(p2_task_lifetime_audit)) { __imp__sub_822DA288(ctx,base); return; }
  const uint32_t queue=ctx.r3.u32, output=ctx.r4.u32, caller=uint32_t(ctx.lr);
  __imp__sub_822DA288(ctx,base);
  const uint32_t object=REX_LOAD_U32(output);
  if (object>=0x10000 && object<=0xfffff000) {
    auto event=Snapshot(ctx,object,REX_LOAD_U32(object),"queue_return_822DA288",queue);
    event.lr=caller;
    std::lock_guard lock(history_mutex); pops.Push(event);
  }
}
#endif
