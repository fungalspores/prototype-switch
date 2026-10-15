// SPDX-License-Identifier: GPL-3.0-or-later
#include "generated/default/prototype2_pch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/gpu_wait_diagnostics.h>
#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdio>
using namespace p2::gpuwait;
REXCVAR_DEFINE_BOOL(p2_gpu_wait_diagnostics,false,"Prototype2","GPU/WSI/query wait attribution, INFO once every10s; requires restart")
 .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
namespace {
uint64_t Now(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
struct alignas(64) Counter {std::atomic<uint64_t> calls{0},ns{0},exclusive{0},maximum{0};};
std::array<Counter,4*Count*Count> counters;
std::atomic<uint64_t> frames{0},invalid{0};
struct Node {uint32_t category=0,role=0,parent=0;uint64_t start=0,children=0;};
struct WaitThreadState {uint32_t role=Other,depth=0;std::array<Node,32> nodes{};};
thread_local WaitThreadState tls;
void Max(std::atomic<uint64_t>& a,uint64_t n){uint64_t v=a.load(std::memory_order_relaxed);while(v<n&&!a.compare_exchange_weak(v,n,std::memory_order_relaxed)){};}
void Record(uint32_t c,uint32_t role,uint32_t parent,uint64_t ns,uint64_t exclusive){
 auto& a=counters[(role*Count+c)*Count+parent];a.ns.fetch_add(ns,std::memory_order_relaxed);a.exclusive.fetch_add(exclusive,std::memory_order_relaxed);Max(a.maximum,ns);a.calls.fetch_add(1,std::memory_order_relaxed);
}
const char* names[]={"ring.swap","native.swap","resolve","native.submit","slot.wait","shutdown.wait","readback.wait","clear.fence","pacing","WAIT_REG_MEM.total","WAIT_REG_MEM.blocked","WAIT_REG_MEM.occlusion","GetData.call","GetData.pending-latency","guest.refresh","refresh.callback","mailbox.wait","paint","paint.old-submission","refresh.resize","refresh.seal","tracker.await","fence.blocking-call","fence.poll","acquire.image","queue.lock","queue.submit","queue.present","wsi.present","bq.queue-buffer","recorder.drain","texture.uploads"};
const char* roles[]={"other","ring","guest-query","presenter"};
struct Site {std::atomic<int64_t> key{0};Counter c;};
std::array<Site,32> sites;
}
extern "C" bool P2GpuWaitEnabled(){return REXCVAR_GET(p2_gpu_wait_diagnostics);}
extern "C" uint32_t P2GpuWaitRole(uint32_t role){const auto old=tls.role;if(role<4)tls.role=role;return old;}
extern "C" uint64_t P2GpuWaitBegin(uint32_t c){
 if(!P2GpuWaitEnabled())return 0;
 if(c>=Count || tls.depth==tls.nodes.size()){invalid.fetch_add(1);return 0;}
 const uint64_t t=Now();const uint32_t parent=tls.depth?tls.nodes[tls.depth-1].category:c;
 tls.nodes[tls.depth++]={c,tls.role,parent,t,0};return t;
}
extern "C" void P2GpuWaitEnd(uint32_t c,uint64_t start){
 if(!start)return;
 if(!tls.depth || tls.nodes[tls.depth-1].category!=c || tls.nodes[tls.depth-1].start!=start){invalid.fetch_add(1);return;}
 const uint64_t ns=Now()-start;const auto n=tls.nodes[--tls.depth];
 Record(c,n.role,n.parent,ns,ns-std::min(ns,n.children));
 if(tls.depth)tls.nodes[tls.depth-1].children+=ns;
}
extern "C" void P2GpuWaitFrame(){if(P2GpuWaitEnabled())frames.fetch_add(1,std::memory_order_relaxed);}
extern "C" void P2GpuWaitReport(){
 if(!P2GpuWaitEnabled())return;
 static uint64_t previous_ns=Now(),previous_frame=frames.load();
 const uint64_t now=Now(),f=frames.load();if(now-previous_ns<10000000000ull || f==previous_frame)return;
 const auto delta=f-previous_frame;previous_ns=now;previous_frame=f;
 REXLOG_INFO("[gpu-waits] window frames={} invalid={} (inclusive overlaps; exclusive subtracts nested scopes; pending-latency is NOT CPU blocked time)",delta,invalid.exchange(0));
 for(uint32_t role=0;role<4;++role)for(uint32_t c=0;c<Count;++c)for(uint32_t parent=0;parent<Count;++parent){
  auto& a=counters[(role*Count+c)*Count+parent];const auto calls=a.calls.exchange(0),ns=a.ns.exchange(0),ex=a.exclusive.exchange(0),mx=a.maximum.exchange(0);
  if(!calls&&!ns)continue;
  REXLOG_INFO("[gpu-waits] {} {} parent={} {:.3f} ms/frame exclusive={:.3f} calls={} max={:.3f} ms",roles[role],names[c],names[parent],double(ns)/1e6/delta,double(ex)/1e6/delta,calls,double(mx)/1e6);
 }
 for(auto& s:sites){const auto key=s.key.load();if(!key)continue;const auto calls=s.c.calls.exchange(0),ns=s.c.ns.exchange(0),mx=s.c.maximum.exchange(0);if(!calls)continue;
  REXLOG_INFO("[gpu-waits] fence-site delta_from_wrapper={} calls={} {:.3f} ms/frame max={:.3f} ms",key-1,calls,double(ns)/1e6/delta,double(mx)/1e6);
 }
}
// GetData is a poll, not necessarily a wait. Time its actual execution and separately
// report first-S_FALSE -> completion latency, which may include other guest work.
REX_EXTERN(__imp__sub_822C67D0);
extern "C" REX_FUNC(sub_822C67D0){
 if(!P2GpuWaitEnabled()){__imp__sub_822C67D0(ctx,base);return;}
 const uint32_t query=ctx.r3.u32;
 uint32_t type;std::memcpy(&type,base+uint32_t(query+4),4);type=__builtin_bswap32(type);
 if(type!=9){__imp__sub_822C67D0(ctx,base);return;}
 struct Pending {uint32_t address=0;uint64_t first=0;};static thread_local std::array<Pending,16> pending{};
 RoleScope role(Guest);Scope call(GetDataCall);
 __imp__sub_822C67D0(ctx,base);
 const uint64_t now=Now();auto it=std::find_if(pending.begin(),pending.end(),[&](const auto& p){return p.first&&p.address==query;});
 if(ctx.r3.u32==1){if(it==pending.end()){it=std::find_if(pending.begin(),pending.end(),[](const auto& p){return !p.first;});if(it!=pending.end())*it={query,now};}}
 else if(it!=pending.end()){Record(GetDataPendingLatency,Guest,GetDataCall,now-it->first,now-it->first);*it={};}
}
extern "C" void P2GpuWaitFenceSite(int64_t delta,uint64_t start){
 if(!start)return;
 for(auto& s:sites){int64_t key=s.key.load();if(!key){s.key.compare_exchange_strong(key,delta+1);key=s.key.load();}if(key!=delta+1)continue;
  const auto ns=Now()-start;s.c.ns.fetch_add(ns);Max(s.c.maximum,ns);s.c.calls.fetch_add(1);break;
 }
}
#if defined(__SWITCH__)
#include <switch.h>
extern "C" Result __real_bqQueueBuffer(Binder*,s32,const BqBufferInput*,BqBufferOutput*);
extern "C" Result __wrap_bqQueueBuffer(Binder* b,s32 slot,const BqBufferInput* i,BqBufferOutput* o){Scope s(BQQueueBuffer);return __real_bqQueueBuffer(b,slot,i,o);}
#endif
