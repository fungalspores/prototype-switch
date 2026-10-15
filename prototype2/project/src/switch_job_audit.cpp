// SPDX-License-Identifier: GPL-3.0-or-later
#include "generated/default/prototype2_pch.h"
#include "job_register_audit.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <atomic>

REXCVAR_DEFINE_BOOL(p2_job_register_audit, false, "Prototype2", "Observe task-pump ABI and parent object changes without changing guest execution");
REX_EXTERN(__imp__sub_822BAA78);
REX_EXTERN(__imp__sub_821D2D20);
namespace {
struct SceneRecord { SceneRecord* parent; uint32_t object, sp, member; };
thread_local SceneRecord* scene=nullptr;
std::atomic<uint64_t> scene_calls{0}, pump_calls{0}, abi_failures{0};
std::atomic<uint32_t> details{0};
bool Detail() { return details.fetch_add(1, std::memory_order_relaxed)<32; }
}

extern "C" REX_FUNC(sub_821D2D20) {
  if (!REXCVAR_GET(p2_job_register_audit)) { __imp__sub_821D2D20(ctx,base); return; }
  const uint32_t object=ctx.r3.u32;
  if (object<0x10000u || object>0xfffff000u) {
    REXLOG_ERROR("[p2-job] invalid scene argument={:08X} sp={:08X} lr={:08X}",object,ctx.r1.u32,uint32_t(ctx.lr));
    __imp__sub_821D2D20(ctx,base); return;
  }
  SceneRecord record{scene,object,ctx.r1.u32,REX_LOAD_U32(object+16)};
  scene=&record;
  const auto n=scene_calls.fetch_add(1,std::memory_order_relaxed)+1;
  if (n<=4 || (record.member<0x10000u && Detail()))
    REXLOG_INFO("[p2-job] scene entry={} ctx={} object={:08X} member16={:08X} sp={:08X} lr={:08X}",n,static_cast<void*>(&ctx),record.object,record.member,record.sp,uint32_t(ctx.lr));
  __imp__sub_821D2D20(ctx,base);
  scene=record.parent;
}

extern "C" REX_FUNC(sub_822BAA78) {
  if (!REXCVAR_GET(p2_job_register_audit)) { __imp__sub_822BAA78(ctx,base); return; }
  const uint32_t stack=ctx.r1.u32;
  const auto before=P2JobRegisters::Capture(ctx,REX_LOAD_U32(stack));
  const uint32_t member=scene?REX_LOAD_U32(scene->object+16):0;
  __imp__sub_822BAA78(ctx,base);
  // Read the original caller's frame, even if the returned r1 is corrupt.
  const auto after=P2JobRegisters::Capture(ctx,REX_LOAD_U32(stack));
  const auto mask=before.Difference(after);
  const auto n=pump_calls.fetch_add(1,std::memory_order_relaxed)+1;
  if (mask) {
    const auto failures=abi_failures.fetch_add(1,std::memory_order_relaxed)+1;
    if (failures<=32) {
      REXLOG_ERROR("[p2-job] ABI MISMATCH call={} mask={:03X} ctx={} sp={:08X}->{:08X} lr={:08X}->{:08X} chain={:08X}->{:08X}",n,mask,static_cast<void*>(&ctx),uint32_t(before.sp),uint32_t(after.sp),before.lr,after.lr,before.backchain,after.backchain);
      for (unsigned i=0;i<6;++i)
        REXLOG_ERROR("[p2-job] r{} expected={:016X} actual={:016X} saved={:016X}",26+i,before.nonvolatile[i],after.nonvolatile[i],REX_LOAD_U64(stack-56+i*8));
    }
  }
  if (scene) {
    const uint32_t now=REX_LOAD_U32(scene->object+16);
    if ((now!=member || mask || now<0x10000u) && Detail())
      REXLOG_ERROR("[p2-job] parent object={:08X} entry_member16={:08X} pre_pump={:08X} post_pump={:08X} entry_sp={:08X} caller_lr={:08X}",scene->object,scene->member,member,now,scene->sp,before.lr);
  }
  if (n<=4 || !(n%4096))
    REXLOG_INFO("[p2-job] pump_calls={} scene_calls={} abi_failures={}",n,scene_calls.load(std::memory_order_relaxed),abi_failures.load(std::memory_order_relaxed));
}
