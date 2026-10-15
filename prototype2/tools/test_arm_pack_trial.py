from pathlib import Path
import subprocess,sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import ROOT,CLANG,vs_env
source=ROOT/'tests/arm_pack_trial.cpp'; out=ROOT/'build/switch-hot/arm_pack_trial.exe'
args=[str(CLANG),'/nologo','/std:c++latest','/O2','/EHsc','/MD','/clang:-msse4.1','/clang:-ffp-contract=off','/clang:-fno-fast-math','/I'+str(ROOT/'tools/bin/rexglue-0.10.0/win-amd64/include'),str(source),'/Fe:'+str(out),'/Fo:'+str(out.with_suffix('.obj'))]
subprocess.run(args,env=vs_env(),check=True); subprocess.run([str(out)],check=True)