"""Execute the actual SDK bugcheck bodies with its actual fatal macro under NDEBUG.
Logging is mocked; abort and the two entrypoint bodies are not. Control case proves
that an unrelated launch failure cannot count as a passing termination test.
"""
from pathlib import Path
import re,subprocess,sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import ROOT,CLANG,vs_env
sdk=ROOT/'reference/masseffect-nx/sdk'
source=(sdk/'src/kernel/xboxkrnl/xboxkrnl_debug.cpp').read_text()
bodies=[]
for name in ('KeBugCheckEx_entry','KeBugCheck_entry'):
 m=re.search(r'\[\[noreturn\]\] void '+name+r'\([^\n]+\) \{[^}]*\n\}',source)
 # Format strings contain braces: delimit by the function-closing line instead.
 start=source.index('[[noreturn]] void '+name+'(')
 end=source.index('\n}',start)+2
 bodies.append(source[start:end])
assert 'assert_always' not in bodies[0].split('//')[0]
macro=(sdk/'include/rex/logging/assert.h').read_text()
start=macro.index('#define REX_FATAL(fmt, ...)')
end=macro.index('while (0)',start)+len('while (0)')
macro=macro[start:end]
out=ROOT/'build/switch-hot/bugcheck_release_test.cpp'
cpp='''#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <crtdbg.h>
using u32=uint32_t;
namespace rex {struct Logger {void flush(){}}; Logger* GetLogger(){return nullptr;}}
#define REXLOG_CRITICAL(...) ((void)0)
'''+macro+'\n'+'\n'.join(bodies)+'''
int main(int argc,char** argv) {
 _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
 _set_error_mode(_OUT_TO_STDERR);
 if (argc!=2) return 4;
 if (!std::strcmp(argv[1],"control")) {std::puts("CONTROL");return 0;}
 if (!std::strcmp(argv[1],"basic")) KeBugCheck_entry(0);
 else if (!std::strcmp(argv[1],"extended")) KeBugCheckEx_entry(3,1,2,3,4);
 else return 5;
 std::puts("BUGCHECK RETURNED");return 0;
}
'''
out.write_text(cpp,encoding='utf-8')
exe=out.with_suffix('.exe');env=vs_env()
cmd=[str(CLANG),'/nologo','/std:c++latest','/O2','/EHsc','/MD','/DNDEBUG',str(out),'/Fe:'+str(exe),'/Fo:'+str(out.with_suffix('.obj'))]
r=subprocess.run(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);print(r.stdout)
if r.returncode:raise SystemExit(r.returncode)
r=subprocess.run([str(exe),'control'],env=env,capture_output=True,text=True)
assert r.returncode==0 and 'CONTROL' in r.stdout
for case in ('basic','extended'):
 r=subprocess.run([str(exe),case],env=env,capture_output=True,text=True,timeout=10)
 assert r.returncode!=0 and 'BUGCHECK RETURNED' not in r.stdout,(case,r.returncode,r.stdout)
 print('PASS actual SDK',case,'bugcheck terminates under NDEBUG, exit',r.returncode)
