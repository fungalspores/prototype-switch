#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
namespace p2collision {
struct Event {
 uint64_t begin=0,end=0; uint32_t thread=0,caller=0,kind=0,object=0,other=0,request=0,result=0;
 std::array<uint32_t,6> segment{};uint32_t type_a=0,type_b=0,before=0,after=0;
 std::array<uint32_t,6> canonical{},transformed{},mesh_min_inverse{};
 std::array<uint32_t,16> triangle_indices{};
 uint32_t query_flags=0,transform=0,tree=0,triangles=0,vertices=0,clip_calls=0,clip_hits=0,triangle_calls=0,contact_flag_sets=0;
};
struct Snapshot {std::array<Event,4096> events{};size_t count=0;std::array<uint64_t,4> calls{};uint64_t skipped=0;};
class Trace {
 std::mutex mutex_;std::array<Event,4096> events_{};uint64_t next_=0;
 std::array<std::atomic<uint64_t>,4> calls_{};std::atomic<uint64_t> skipped_{0};
 std::array<uint64_t,4> bucket_{};std::array<uint32_t,4> quota_{};
public:
 bool Sample(unsigned kind,uint32_t& local){calls_[kind].fetch_add(1,std::memory_order_relaxed);return ((local++)&63u)==0;}
 void Store(const Event& event){
  std::unique_lock lock(mutex_,std::try_to_lock);
  if(!lock.owns_lock()){skipped_.fetch_add(1,std::memory_order_relaxed);return;}
  const auto k=event.kind;const uint64_t bucket=event.end/100000000ull;
  if(bucket_[k]!=bucket){bucket_[k]=bucket;quota_[k]=0;}
  if(quota_[k]++>=16){skipped_.fetch_add(1,std::memory_order_relaxed);return;}
  events_[next_++%events_.size()]=event;
 }
 void Copy(Snapshot& out){
  std::lock_guard lock(mutex_);out.count=size_t(next_<events_.size()?next_:events_.size());
  const uint64_t first=next_-out.count;for(size_t i=0;i<out.count;++i)out.events[i]=events_[(first+i)%events_.size()];
  for(size_t i=0;i<4;++i)out.calls[i]=calls_[i].load(std::memory_order_relaxed);
  out.skipped=skipped_.load(std::memory_order_relaxed);
 }
};
}