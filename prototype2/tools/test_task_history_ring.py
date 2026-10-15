import sys,subprocess
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import vs_env,CLANG,ROOT
out=ROOT/'build/switch-hot/task_history_ring_test.exe'
subprocess.run([str(CLANG),'/nologo','/std:c++latest','/O2','/EHsc','/MD','/I'+str(ROOT/'project'),str(ROOT/'tests/task_history_ring_test.cpp'),'/Fe:'+str(out),'/Fo:'+str(out.with_suffix('.obj'))],env=vs_env(),check=True)
subprocess.run([str(out)],check=True)
