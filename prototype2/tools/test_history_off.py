from pathlib import Path
import sys, subprocess, tomllib, hashlib, json
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import vs_env, CLANG, ROOT
source=(ROOT/'project/src/switch_task_lifetime_audit.cpp').read_text()
def body(signature):
 start=source.index(signature); opening=source.index('{',start); depth=1; i=opening+1
 while depth:
  depth += (source[i]=='{')-(source[i]=='}'); i+=1
 return source[opening:i]
base=tomllib.loads((ROOT/'sd-configs/33-fpscr-roundtrip/prototype2.toml').read_text(encoding='utf-8-sig'))
trial=tomllib.loads((ROOT/'sd-configs/34-history-off/prototype2.toml').read_text(encoding='utf-8-sig'))
assert base.pop('p2_task_lifetime_audit') is True
assert trial.pop('p2_task_lifetime_audit') is False and trial==base
assert trial['p2_task_pump_timeout_fix'] and trial['p2_purecall_audit']
assert (ROOT/'sd-configs/34-history-off/stacks_profile.flag').read_bytes()==b'off'
archive=ROOT/'build/nro-archive/prototype2-52b5a4e2-fpscr-roundtrip.nro'
assert hashlib.sha256(archive.read_bytes()).hexdigest()=='52b5a4e2a7c1699a5d58777c41efa4b5cff2aac600b20c638a1051ad77a72832'
report=json.loads((ROOT/'build/hw-runs/run46-52b5a4e2-fpscr-roundtrip/run46_findings.json').read_text())
assert report['fpscr_hardware_pass_count']==1 and report['skinning_real_verified_calls']==128
preamble=r'''
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <cstdio>
#include <cstdlib>
struct Reg { uint32_t u32=0; };
struct PPCContext { Reg r3,r4; uint64_t lr=0; std::array<uint64_t,64> registers{}; };
std::array<uint32_t,128> memory{}, expected_memory{};
bool enabled=false;
uint64_t snapshots=0, locks=0, loads=0, originals=0, recorded=0;
#define REXCVAR_GET(x) enabled
#define REX_LOAD_U32(x) Load(x)
uint32_t Load(uint32_t address) { ++loads; if(address/4>=memory.size()) std::abort(); return memory[address/4]; }
struct Mutex { void lock(){++locks;} void unlock(){} } history_mutex;
struct Event { uint32_t lr=0; };
Event Snapshot(PPCContext&,uint32_t,uint32_t,const char*,uint32_t=0){++snapshots;return {};}
struct Ring { void Push(Event){++recorded;} } writes,pops;
void __imp__sub_822DA288(PPCContext& ctx,uint8_t*) {
 ++originals;
 for(size_t i=0;i<ctx.registers.size();++i) ctx.registers[i] ^= 0xfeed00000000ull+i;
 for(size_t i=0;i<memory.size();++i) memory[i]^=uint32_t(i*17+3);
 ctx.lr ^= 0xabc;
}
'''
code=preamble+'\nvoid P2TaskBaseWrite(PPCContext& ctx,uint8_t* base,uint32_t object,const char* function) '+body('void P2TaskBaseWrite(')+'\nvoid sub_822DA288(PPCContext& ctx,uint8_t* base) '+body('extern "C" REX_FUNC(sub_822DA288)')+r'''
int main() {
 for(uint32_t trial=0;trial<10000;++trial) {
  PPCContext ctx{}; ctx.r3.u32=trial*13; ctx.r4.u32=0xfffffffcu;ctx.lr=trial*37;
  for(size_t i=0;i<ctx.registers.size();++i) ctx.registers[i]=uint64_t(trial)*971+i;
  for(size_t i=0;i<memory.size();++i) memory[i]=trial+uint32_t(i*23);
  auto expected=ctx; expected.lr^=0xabc;
  for(size_t i=0;i<expected.registers.size();++i) expected.registers[i]^=0xfeed00000000ull+i;
  expected_memory=memory;for(size_t i=0;i<memory.size();++i) expected_memory[i]^=uint32_t(i*17+3);
  P2TaskBaseWrite(ctx,nullptr,0xfffffffcu,"fixture"); sub_822DA288(ctx,nullptr);
  if(ctx.r3.u32!=expected.r3.u32 || ctx.r4.u32!=expected.r4.u32 || ctx.lr!=expected.lr || ctx.registers!=expected.registers || memory!=expected_memory) return 1;
 }
 if(originals!=10000 || snapshots || locks || loads || recorded) return 2;
 // Positive control: enabled base writes really run the load, timestamp snapshot and lock.
 enabled=true;PPCContext ctx{};P2TaskBaseWrite(ctx,nullptr,0,"control");
 if(loads!=1 || snapshots!=1 || locks!=1 || recorded!=1) return 3;
 std::puts("PASS 10000 actual history-disabled hook calls: original once, registers/memory equal, no history loads/snapshots/mutex; enabled positive control PASS");
}
'''
out=ROOT/'build/switch-hot/history_off_test.cpp'; out.write_text(code)
exe=out.with_suffix('.exe')
subprocess.run([str(CLANG),'/nologo','/std:c++latest','/O2','/EHsc','/MD',str(out),'/Fe:'+str(exe),'/Fo:'+str(out.with_suffix('.obj'))],env=vs_env(),check=True)
subprocess.run([str(exe)],check=True)
print('PASS same NRO, only history runtime flag changed; timeout fix/purecall/skin/geometry/rounding preserved; sampling off')