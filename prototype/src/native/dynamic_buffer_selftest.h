#pragma once
#include "dynamic_buffer_stamp.h"
#include <array>
#include <cstring>
#include <limits>
namespace p2::native {
// Shared host/ARM startup fixture. It compares every visible byte, not hashes.
inline bool ValidateDynamicBufferStamp(unsigned steps=8192) {
  for(unsigned session=0;session<2;++session) {
    DynamicBufferStamp policy;
    std::array<uint8_t,256> data{},copied{};
    BufferRangeKey cached{};bool cache_valid=false;
    uint64_t scans=0;uint32_t random=415608;
    const uint64_t start=session?std::numeric_limits<uint64_t>::max()-800:0;
    for(unsigned i=0;i<steps;++i) {
      const uint64_t frame=start+i;
      random^=random<<13;random^=random>>17;random^=random<<5;
      if(i%113<83) data[random%256]^=uint8_t(random|1);
      unsigned called=0;
      auto exact=[&](){++called;++scans;uint64_t h=1469598103934665603ull;for(auto b:data){h^=b;h*=1099511628211ull;}return h;};
      const auto stamp=policy.Check(frame,exact);
      const auto again=policy.Check(frame,exact);
      if(called>1||stamp.hash!=again.hash||stamp.epoch!=again.epoch||stamp.frame_local!=again.frame_local)return false;
      const BufferRangeKey key{4096,256,stamp.hash,stamp.epoch,stamp.frame_local};
      if(!cache_valid||!(cached==key)){copied=data;cached=key;cache_valid=true;}
      if(std::memcmp(data.data(),copied.data(),data.size())!=0)return false;
      if(stamp.frame_local && (called!=0 || key==BufferRangeKey{4096,256,0,0,false}))return false;
    }
    if(scans>=steps/2)return false;
  }
  DynamicBufferStamp sparse;
  for(uint64_t n=0;n<160;++n) {
    const auto stamp=sparse.Check(3+n*16,[&](){return n<32?n+1:32ull;});
    if(n>64 && stamp.frame_local)return false;
  }
  return true;
}
}