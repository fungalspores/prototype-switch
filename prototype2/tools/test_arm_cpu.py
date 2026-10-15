from pathlib import Path
import re,sys,subprocess,json
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import vs_env,CLANG,ROOT
from prepare_arm_cpu import canonical,transform,FUN,eligible
out=ROOT/'build/switch-hot';out.mkdir(parents=True,exist_ok=True)
fixtures={};count=0
for p in (ROOT/'project/generated/default').glob('prototype2_recomp.*.cpp'):
 for m in FUN.finditer(p.read_text()):
  name=re.match(r'DEFINE_REX_FUNC\((\w+)\)',m[0])[1];old=canonical(m[0]);new,r=transform(old)
  assert canonical(new)==old and transform(new)[0]==new,name
  if r['leaf']:
   count+=1;assert '(ctx, base)' not in old and 'REX_CALL' not in old and '_LOCK' not in old
  if name in ['sub_8222AA30','sub_829FF6A8']:fixtures[name]=(old,new)
assert len(fixtures)==2 and eligible('sub_8222AA30',fixtures['sub_8222AA30'][0])
parts=[]
for name,tag in [('sub_8222AA30','matrix'),('sub_829FF6A8','stream')]:
 for source,version in zip(fixtures[name],['reference','optimized']):
  parts.append(source.replace('DEFINE_REX_FUNC('+name+')','static REX_FUNC(p2_'+version+'_'+tag+')'))
(out/'arm_cpu_fixtures.inc').write_text('\n'.join(parts))
source=r'''
#include "generated/default/prototype2_pch.h"
#include "arm_cpu_fixture_test.h"
#include <cstdio>
int main(){
 if(!P2ArmCpuFixtures(8192))return 1;
 if(!P2VmxIntegerFixtures(4096))return 2;
 std::puts("PASS 8192 actual guest matrix/stream trials: full context, full memory, all alignments, output aliases and4rounding modes;4096vector controls +131072partial-store cases/sentinels");
}
'''
p=out/'arm_cpu_host_test.cpp';p.write_text(source);exe=p.with_suffix('.exe')
args=[str(CLANG),'/nologo','/std:c++latest','/O2','/EHsc','/MD','/DP2_LEAF_LOCALS=1','/DP2_VMX_HOST_TEST=1','/DSPDLOG_FMT_EXTERNAL','/DSPDLOG_COMPILED_LIB','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/clang:-msse4.1','/clang:-ffp-contract=off','/clang:-fno-strict-aliasing','/clang:-Wno-ignored-attributes','/I'+str(ROOT/'project'),'/I'+str(ROOT/'project/src'),'/I'+str(ROOT/'tools/bin/rexglue-0.10.0/win-amd64/include'),'/I'+str(out),str(p),'/Fe:'+str(exe),'/Fo:'+str(p.with_suffix('.obj'))]
subprocess.run(args,env=vs_env(),check=True);subprocess.run([str(exe)],check=True)
print('PASS reversible/idempotent whole-project lowering for',count,'leaf functions; callbacks/locks excluded; ARM native execution remains mandatory startup test')