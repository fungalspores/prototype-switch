from pathlib import Path
import sys,subprocess
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import ROOT,CLANG,vs_env
p=ROOT/'tests/dynamic_buffer_stamp_test.cpp';out=ROOT/'build/switch-hot/dynamic_buffer_stamp_test.exe'
subprocess.run([str(CLANG),'/nologo','/std:c++latest','/O2','/EHsc','/MD','/I'+str(ROOT/'project/src'),str(p),'/Fe:'+str(out),'/Fo:'+str(out.with_suffix('.obj'))],env=vs_env(),check=True)
subprocess.run([str(out)],check=True)