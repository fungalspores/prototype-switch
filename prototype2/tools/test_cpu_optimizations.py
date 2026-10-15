"""Run isolated CPU tests with the project's existing Windows toolchain."""
from pathlib import Path
import subprocess, sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_windows import vs_env, CLANG, LINKER, ROOT
from prepare_switch_hot import generate

generate()
env=vs_env()
for name in ['skinning_diff', 'composite_tile_index_test', 'job_register_audit_test', 'guest_stack_audit_test']:
    out=ROOT/'build/switch-hot'/f'{name}.exe'
    cmd=[str(CLANG), '/nologo', '/std:c++latest', '/O2', '/EHsc', '/MD',
         '/DSPDLOG_FMT_EXTERNAL', '/DSPDLOG_COMPILED_LIB', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/clang:-msse4.1','/clang:-ffp-contract=off','/clang:-fno-strict-aliasing',
         '/clang:-Wno-ignored-attributes', '/I'+str(ROOT/'project'),
         '/I'+str(ROOT/'tools/bin/rexglue-0.10.0/win-amd64/include'),
         '/I'+str(ROOT/'build/switch-hot'), str(ROOT/'tests'/f'{name}.cpp'),
         '/Fe:'+str(out), '/Fo:'+str(out.with_suffix('.obj')), '/link', '/IGNORE:4217']
    result=subprocess.run(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    print(result.stdout,flush=True)
    if result.returncode: raise SystemExit(result.returncode)
    subprocess.run([str(out)],check=True,env=env)
