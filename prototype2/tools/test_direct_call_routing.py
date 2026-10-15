import sys,subprocess
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import ROOT,CLANG,vs_env
from prepare_direct_calls import convert
source='static REX_FUNC(transformed_caller) { sub_82000100(ctx, base); sub_82000200(ctx, base); }'
result,targets=convert(source,{'sub_82000200'})
assert targets=={'sub_82000100'}
out=ROOT/'build/switch-hot'; (out/'direct_routing_under_test.inc').write_text(result,encoding='utf-8')
exe=out/'direct_call_routing_test.exe'
subprocess.run([str(CLANG),'/nologo','/std:c++latest','/O2','/EHsc','/MD','/DSPDLOG_FMT_EXTERNAL','/DSPDLOG_COMPILED_LIB','/clang:-msse4.1','/clang:-Wno-ignored-attributes','/I'+str(ROOT/'project'),'/I'+str(ROOT/'tools/bin/rexglue-0.10.0/win-amd64/include'),'/I'+str(out),str(ROOT/'tests/direct_call_routing_test.cpp'),'/Fe:'+str(exe),'/Fo:'+str(exe.with_suffix('.obj'))],env=vs_env(),check=True)
subprocess.run([str(exe)],check=True)
