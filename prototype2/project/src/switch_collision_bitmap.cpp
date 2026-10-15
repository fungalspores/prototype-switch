// SPDX-License-Identifier: GPL-3.0-or-later
// Xenon dcbz clears128bytes. The generated82994D18 clears32 with128byte strides,
// leaving6144bytes of the collision traversal's8192byte visited bitmap stale.
#include "generated/default/prototype2_pch.h"
#include <cstring>
#include <atomic>
#ifndef P2_COLLISION_BITMAP_TEST
#include <rex/cvar.h>
#include <rex/logging.h>
REXCVAR_DEFINE_BOOL(p2_collision_bitmap_fix,true,"Prototype2",
                   "Restore128byte Xenon cache-line clearing of the collision visited bitmap");
#endif
REX_EXTERN(__imp__sub_82994D18);
extern "C" REX_FUNC(sub_82994D18){
 const uint32_t start=ctx.r3.u32 & ~uint32_t(127);
 __imp__sub_82994D18(ctx,base);
 if(!REXCVAR_GET(p2_collision_bitmap_fix))return;
 if(uint64_t(start)+8192<=0x100000000ull){std::memset((void*)REX_RAW_ADDR(start),0,8192);}
 else for(unsigned i=0;i<64;++i){const uint32_t address=start+i*128u;std::memset((void*)REX_RAW_ADDR(address),0,128);}
#ifndef P2_COLLISION_BITMAP_TEST
 static std::atomic<bool> reported{false};
 if(!reported.exchange(true,std::memory_order_relaxed))REXLOG_INFO("[p2-collision-bitmap] Xenon128byte clear enabled:8192bytes at{:08X}",start);
#endif
}
