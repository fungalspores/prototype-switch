// SPDX-License-Identifier: GPL-3.0-or-later
#include "generated/default/prototype2_pch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
REXCVAR_DEFINE_BOOL(p2_purecall_audit, false, "Prototype2", "Capture original task/object state before a guest pure virtual call terminates");
REX_EXTERN(__imp__sub_82C2CC60);
extern "C" REX_FUNC(sub_82C2CC60) {
  if (!REXCVAR_GET(p2_purecall_audit)) { __imp__sub_82C2CC60(ctx,base); return; }
  const uint32_t object=ctx.r3.u32;
  REXLOG_ERROR("[p2-purecall] object={:08X} caller_lr={:08X} sp={:08X} r31={:08X} r30={:08X} ctx={}",object,uint32_t(ctx.lr),ctx.r1.u32,ctx.r31.u32,ctx.r30.u32,static_cast<void*>(&ctx));
  if (object>=0x10000u && object<=0xfffff000u) {
    const uint32_t vt=REX_LOAD_U32(object);
    REXLOG_ERROR("[p2-purecall] vtable={:08X}",vt);
    for (unsigned i=0;i<16;++i)
      REXLOG_ERROR("[p2-purecall] object+{:02X}={:08X}",i*4,REX_LOAD_U32(object+i*4));
    // Only inspect vtables inside the loaded static image, not arbitrary guest pointers.
    if (vt>=REX_IMAGE_BASE && uint64_t(vt)+48<=uint64_t(REX_IMAGE_BASE)+REX_IMAGE_SIZE)
      for (unsigned i=0;i<12;++i)
        REXLOG_ERROR("[p2-purecall] vtable+{:02X}={:08X}",i*4,REX_LOAD_U32(vt+i*4));
  }
  P2DumpTaskHistory(object);
  rex::FlushLogging();
  REX_FATAL("Guest pure virtual call at caller {:08X}, object {:08X}; inspect task history for the primary cause",uint32_t(ctx.lr),object);
}
