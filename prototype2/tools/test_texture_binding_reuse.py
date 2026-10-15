import sys,subprocess
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import ROOT,CLANG,vs_env
exe=ROOT/'build/switch-hot/texture_binding_reuse_test.exe'
subprocess.run([str(CLANG),'/nologo','/std:c++17','/O2','/EHsc','/MD','/I'+str(ROOT/'project/src'),str(ROOT/'tests/texture_binding_reuse_test.cpp'),'/Fe:'+str(exe),'/Fo:'+str(exe.with_suffix('.obj'))],env=vs_env(),check=True)
subprocess.run([str(exe)],check=True)