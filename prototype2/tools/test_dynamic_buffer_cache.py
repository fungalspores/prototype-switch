from pathlib import Path
import re,sys,subprocess
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import ROOT,CLANG,vs_env
s=(ROOT/'project/src/native/native_renderer.cpp').read_text();assert 'p2_native_dynamic_buffer_upload' in s
for text in ('known.epoch, known.frame_local','k.epoch *','BufferRangeKey','known.detector.CheckExact(data, bytes, vertex_hash_bytes)'):
 assert text in s,text
assert 'p2_native_depth_attachment_only' not in s
methods=[]
for mark in ('  static uint64_t RangeSlot(', '  uint64_t FindRange(', '  void AddRange('):
 a=s.index(mark);opening=s.index('{',a);depth=1;at=opening+1
 while depth:
  depth += (s[at]=='{')-(s[at]=='}');at+=1
 methods.append(s[a:at])
fixture='''#include "native/native_hash.h"
using VertexRangeKey=p2::native::BufferRangeKey;
struct ActualRangeCache {
 struct RangeCopy { VertexRangeKey key{}; uint64_t position=0; };
 p2::native::FlatMap64<RangeCopy> vertex_ranges;
'''+ '\n'.join(methods)+'\n};\n'
out=ROOT/'build/switch-hot';(out/'dynamic_buffer_cache_fixture.inc').write_text(fixture)
cpp=r'''
#include "native/dynamic_buffer_stamp.h"
#include "dynamic_buffer_cache_fixture.inc"
#include <array>
#include <cstdio>
#include <cstdlib>
using namespace p2::native;
void require(bool ok){if(!ok)std::abort();}
uint64_t hash(const std::array<uint8_t,256>& data){return XXH3_64bits(data.data(),data.size());}
int main(){
 uint64_t frames=0,scans=0;uint32_t random=415608;
 auto next=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return random;};
 for(int trial=0;trial<64;++trial){
  ActualRangeCache cache;DynamicBufferStamp state;std::array<uint8_t,256> data{};
  std::vector<std::array<uint8_t,256>> copies;
  for(uint64_t frame=0;frame<4096;++frame){
   if(frame%113<83){data[next()%256]^=uint8_t(next()|1);}
   auto stamp=state.Check(frame,[&](){++scans;return hash(data);});
   VertexRangeKey key{4096,256,stamp.hash,stamp.epoch,stamp.frame_local};
   for(int draw=0;draw<8;++draw){
    auto position=cache.FindRange(key);
    if(position==UINT64_MAX){position=copies.size();copies.push_back(data);cache.AddRange(key,position);}
    require(position<copies.size() && copies[position]==data);
    if(draw==0 && stamp.frame_local){
     require(cache.FindRange(VertexRangeKey{4096,256,0,0,false})==UINT64_MAX);
     require(cache.FindRange(VertexRangeKey{4096,256,0,stamp.epoch+1,true})==UINT64_MAX);
    }
   }
   ++frames;
  }
 }
 require(scans<frames/2);
 std::printf("PASS %llu frames x8draws: actual production RangeSlot/FindRange/AddRange and XXH3 content checks; copied data equal byte-for-byte after partial updates/settling/resume. Hash scans %llu vs %llu baseline.\n",frames,scans,frames);
}
'''
p=out/'dynamic_buffer_cache_test.cpp';p.write_text(cpp);exe=p.with_suffix('.exe')
subprocess.run([str(CLANG),'/nologo','/std:c++latest','/O2','/EHsc','/MD','/DXXH_INLINE_ALL','/I'+str(ROOT/'project/src'),'/I'+str(ROOT/'tools/bin/rexglue-0.10.0/win-amd64/include'),'/I'+str(out),str(p),'/Fe:'+str(exe),'/Fo:'+str(p.with_suffix('.obj'))],env=vs_env(),check=True)
subprocess.run([str(exe)],check=True)
print('Limits: validates CPU cache/copies and real renderer methods on host; not Vulkan/ARM runtime or scene FPS')