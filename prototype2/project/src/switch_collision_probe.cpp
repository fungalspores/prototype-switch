// SPDX-License-Identifier: GPL-3.0-or-later
#include "generated/default/prototype2_pch.h"
#include "collision_trace.h"
#ifndef P2_COLLISION_PROBE_TEST
#include <switch.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <cstdio>
#include <sys/stat.h>
REXCVAR_DECLARE(bool,p2_collision_diagnostics);
static uint64_t P2CollisionClock(){return armTicksToNs(armGetSystemTick());}
#else
extern uint64_t P2CollisionClock();
#endif
namespace {
p2collision::Trace trace;
std::array<std::atomic<uint32_t>,4> live{0,0,0,0};
thread_local std::array<uint32_t,4> sample_count{};
thread_local p2collision::Event* active_mesh=nullptr;
bool Begin(PPCContext& ctx,unsigned kind,p2collision::Event& e){
 if(!REXCVAR_GET(p2_collision_diagnostics))return false;
 if(!trace.Sample(kind,sample_count[kind]))return false;
 e.kind=kind;e.begin=P2CollisionClock();e.caller=uint32_t(ctx.lr);
#ifndef P2_COLLISION_PROBE_TEST
 e.thread=threadGetCurHandle();
#else
 e.thread=42;
#endif
 return true;
}
void Segment(uint8_t* base,uint32_t address,p2collision::Event& e){
 if(address<0x10000 || uint64_t(address)+24>0x100000000ull)return;
 for(unsigned i=0;i<6;++i)e.segment[i]=REX_LOAD_U32(address+i*4);
}
void End(p2collision::Event& e){e.end=P2CollisionClock();trace.Store(e);}
}
REX_EXTERN(__imp__sub_82246E70);
extern "C" REX_FUNC(sub_82246E70){
 p2collision::Event e;const bool sampled=Begin(ctx,0,e);
 if(sampled){e.request=ctx.r3.u32;e.object=ctx.r5.u32;e.other=ctx.r4.u32;e.type_a=REX_LOAD_U8(e.object);Segment(base,e.request,e);}
 __imp__sub_82246E70(ctx,base);
 if(sampled){e.result=ctx.r3.u32;End(e);}
}
REX_EXTERN(__imp__sub_822426F0);
extern "C" REX_FUNC(sub_822426F0){
 p2collision::Event e;const bool sampled=Begin(ctx,1,e);
 if(sampled){e.request=ctx.r3.u32;e.object=ctx.r6.u32;e.other=ctx.r7.u32;Segment(base,e.request,e);
  e.query_flags=REX_LOAD_U32(e.request+56);e.transform=REX_LOAD_U8(e.object+148);
  e.tree=REX_LOAD_U32(e.object+160);e.triangles=REX_LOAD_U32(e.object+164);e.vertices=REX_LOAD_U32(e.object+168);
  for(unsigned i=0;i<6;++i)e.canonical[i]=REX_LOAD_U32(e.request+100+i*4);
  for(unsigned i=0;i<3;++i){e.mesh_min_inverse[i]=REX_LOAD_U32(e.object+172+i*4);e.mesh_min_inverse[i+3]=REX_LOAD_U32(e.object+196+i*4);}
 }
 auto* previous=active_mesh;active_mesh=sampled?&e:nullptr;
 __imp__sub_822426F0(ctx,base);
 active_mesh=previous;
 if(sampled){e.result=ctx.r3.u32;for(unsigned i=0;i<6;++i)e.transformed[i]=REX_LOAD_U32(e.request+128+i*4);End(e);}
}
// Aggregate stages only while a sampled mesh query is executing on this thread.
REX_EXTERN(__imp__sub_82241AA8);
extern "C" REX_FUNC(sub_82241AA8){
 auto* e=active_mesh;const bool matched=e && ctx.r3.u32==e->object;
 __imp__sub_82241AA8(ctx,base);
 if(matched){++e->clip_calls;if(ctx.r3.u32&255u)++e->clip_hits;}
}
REX_EXTERN(__imp__sub_822425A8);
extern "C" REX_FUNC(sub_822425A8){
 auto* e=active_mesh;const bool matched=e && ctx.r5.u32==e->object && ctx.r3.u32==e->request;
 uint8_t before=0;
 if(matched){before=REX_LOAD_U8(e->request+60);if(e->triangle_calls<e->triangle_indices.size())e->triangle_indices[e->triangle_calls]=ctx.r6.u32;++e->triangle_calls;}
 __imp__sub_822425A8(ctx,base);
 if(matched && !before && REX_LOAD_U8(e->request+60))++e->contact_flag_sets;
}
REX_EXTERN(__imp__sub_8299F1B0);
extern "C" REX_FUNC(sub_8299F1B0){
 p2collision::Event e;const bool sampled=Begin(ctx,2,e);
 if(sampled){e.object=ctx.r3.u32;e.other=ctx.r4.u32;e.request=ctx.r5.u32;
  if(e.object!=e.other){e.type_a=REX_LOAD_U8(e.object+72);e.type_b=REX_LOAD_U8(e.other+72);}}
 __imp__sub_8299F1B0(ctx,base);
 // This function is void; result is deliberately not labelled hit/miss.
 if(sampled){End(e);}
}
REX_EXTERN(__imp__sub_822833E8);
extern "C" REX_FUNC(sub_822833E8){
 p2collision::Event e;const bool enabled=REXCVAR_GET(p2_collision_diagnostics);
 const bool sampled=Begin(ctx,3,e);const uint32_t manager=ctx.r3.u32;
 if(enabled)live[3].fetch_add(1,std::memory_order_relaxed);
 if(sampled){e.object=manager;e.before=REX_LOAD_U32(manager+972);e.result=0xffffffffu;e.end=e.begin;trace.Store(e);}
 __imp__sub_822833E8(ctx,base);
 if(enabled)live[3].fetch_sub(1,std::memory_order_relaxed);
 if(sampled){e.after=REX_LOAD_U32(manager+972);e.result=0;End(e);}
}
#ifndef P2_COLLISION_MARKER_DIR
#define P2_COLLISION_MARKER_DIR "sdmc:/switch/Prototype2/logs/collision-markers"
#endif
extern "C" void P2CollisionSnapshotRequest(){
 static std::mutex writer;std::unique_lock guard(writer,std::try_to_lock);if(!guard.owns_lock())return;
 const uint64_t mark=P2CollisionClock();static p2collision::Snapshot snapshot;trace.Copy(snapshot);
 mkdir(P2_COLLISION_MARKER_DIR,0777);
 char path[192];std::snprintf(path,sizeof(path),"%s/marker_%llu.txt",P2_COLLISION_MARKER_DIR,(unsigned long long)mark);
 FILE* f=std::fopen(path,"w");if(!f){REXLOG_WARN("[p2-collision-marker] write failed {}",path);return;}
 std::fprintf(f,"marker_ns=%llu enabled=%d events=%zu sampled_every=64 limit_per_kind_per_100ms=16 skipped=%llu jobs_in_flight=%u\n",
  (unsigned long long)mark,int(REXCVAR_GET(p2_collision_diagnostics)),snapshot.count,(unsigned long long)snapshot.skipped,live[3].load());
 std::fprintf(f,"calls ray=%llu mesh=%llu pair=%llu jobs=%llu\n",(unsigned long long)snapshot.calls[0],(unsigned long long)snapshot.calls[1],(unsigned long long)snapshot.calls[2],(unsigned long long)snapshot.calls[3]);
 std::fprintf(f,"# segment words are IEEE754 float bits in guest ray input; not identified as player position. Pair result is not a boolean.\n");
 for(size_t i=0;i<snapshot.count;++i){const auto& e=snapshot.events[i];
  std::fprintf(f,"event begin=%llu end=%llu kind=%u thread=%u caller=%08x object=%08x other=%08x request=%08x result=%08x types=%u,%u unfinished=%u,%u segment=%08x,%08x,%08x,%08x,%08x,%08x\n",
   (unsigned long long)e.begin,(unsigned long long)e.end,e.kind,e.thread,e.caller,e.object,e.other,e.request,e.result,e.type_a,e.type_b,e.before,e.after,e.segment[0],e.segment[1],e.segment[2],e.segment[3],e.segment[4],e.segment[5]);
  if(e.kind==1){
   std::fprintf(f,"mesh_stage begin=%llu request=%08x object=%08x flags=%u transform=%u tree=%08x triangles=%08x vertices=%08x clip_calls=%u clip_hits=%u triangle_calls=%u contact_flag_sets=%u canonical=",
    (unsigned long long)e.begin,e.request,e.object,e.query_flags,e.transform,e.tree,e.triangles,e.vertices,e.clip_calls,e.clip_hits,e.triangle_calls,e.contact_flag_sets);
   for(auto v:e.canonical)std::fprintf(f,"%08x,",v);std::fprintf(f," transformed=");for(auto v:e.transformed)std::fprintf(f,"%08x,",v);
   std::fprintf(f," min_inverse=");for(auto v:e.mesh_min_inverse)std::fprintf(f,"%08x,",v);
   std::fprintf(f," first_triangle_indices=");for(unsigned j=0;j<e.triangle_calls && j<e.triangle_indices.size();++j)std::fprintf(f,"%u,",e.triangle_indices[j]);std::fprintf(f,"\n");
  }
 }
 const bool failed=std::ferror(f)!=0;const int closed=std::fclose(f);
 REXLOG_WARN("[p2-collision-marker] {} enabled={} events={} saved={}",path,REXCVAR_GET(p2_collision_diagnostics),snapshot.count,!failed && closed==0);
}
