import sys,subprocess,re
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import ROOT,CLANG,vs_env
sdk=ROOT/'reference/masseffect-nx/sdk/include/rex'
s=(sdk/'ppc/context.h').read_text()
start=s.index('constexpr uint32_t kRoundNearest')
end=s.index('}  // namespace rex::ppc',start)
body=s[start:end].replace('struct FPSCRRegister {','template<class MockPlatform> struct FPSCRRegister {').replace('using Platform = platform::FPSCRPlatform;','using Platform = MockPlatform;')
platform=(sdk/'platform/fpscr.h').read_text().split('#elif defined(__aarch64__)')[1]
arm=re.search(r'GuestToHost\[\] = \{(.*?)\};',platform,re.S)[1]
assert re.findall(r'(\d+) << RoundShift',arm)==['0','3','1','2']
prefix=r"""
#include <cstdint>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cassert>
template<unsigned A,unsigned B,unsigned C,unsigned D,unsigned Shift>
struct MockFP {
 inline static uint32_t current=0;
 static constexpr size_t RoundShift=Shift, RoundMaskVal=3u<<Shift, FlushMask=1u<<24;
 static constexpr size_t GuestToHost[4]={A<<Shift,B<<Shift,C<<Shift,D<<Shift};
 static uint32_t getcsr() {return current;}
 static void setcsr(uint32_t value) {current=value;}
 static void InitHostExceptions(uint32_t&) {}
};
"""
suffix=r"""
template<class Platform> unsigned Check() {
 unsigned failures=0;
 for(unsigned value=0;value<4;++value) {
  Platform::current=1u<<20;
  FPSCRRegister<Platform> r{};
  r.InitHost(); r.storeFromGuest(value);
  assert((Platform::current & ~Platform::RoundMaskVal)==(1u<<20));
  unsigned result=r.loadFromHost();
  if(result!=value) { ++failures; std::printf("roundtrip guest=%u readback=%u\n",value,result); }
 }
 return failures;
}
int main() {
 static_assert(sizeof(FPSCRRegister<MockFP<0,3,1,2,22>>)==sizeof(uint32_t));
 auto x86=Check<MockFP<0,3,2,1,13>>();
 auto arm=Check<MockFP<0,3,1,2,22>>();
 assert(x86==0 && arm==0);
 std::printf("PASS actual corrected SDK FPSCR bodies: x86 and ARM 4/4 modes; host policy preserved. Native FRINTI requires Switch startup test.\n");
}
"""
out=ROOT/'build/switch-hot'; cpp=out/'fpscr_mapping_fixed_test.cpp'; cpp.write_text(prefix+body+suffix)
exe=out/'fpscr_mapping_fixed_test.exe'
subprocess.run([str(CLANG),'/nologo','/std:c++17','/O2','/EHsc','/MD',str(cpp),'/Fe:'+str(exe),'/Fo:'+str(exe.with_suffix('.obj'))],env=vs_env(),check=True)
subprocess.run([str(exe)],check=True)