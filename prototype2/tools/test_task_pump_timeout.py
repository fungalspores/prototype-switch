import sys,subprocess
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import vs_env,CLANG,ROOT
from prepare_switch_hot import extract
out=ROOT/'build/switch-hot'
functions=[extract(n) for n in ['__savegprlr_26','__restgprlr_26','sub_822BAA78']]
(out/'task_pump_under_test.inc').write_text('\n'.join(functions),encoding='utf-8')
exe=out/'task_pump_timeout_test.exe'
subprocess.run([str(CLANG),'/nologo','/std:c++latest','/O2','/EHsc','/MD','/DP2_TASK_TIMEOUT_TEST=1','/DSPDLOG_FMT_EXTERNAL','/DSPDLOG_COMPILED_LIB','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/clang:-msse4.1','/clang:-Wno-ignored-attributes','/I'+str(ROOT/'project'),'/I'+str(ROOT/'tools/bin/rexglue-0.10.0/win-amd64/include'),'/I'+str(out),str(ROOT/'tests/task_pump_timeout_test.cpp'),'/Fe:'+str(exe),'/Fo:'+str(exe.with_suffix('.obj'))],env=vs_env(),check=True)
subprocess.run([str(exe)],check=True)
