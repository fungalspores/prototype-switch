from pathlib import Path
import sys,subprocess
sys.path.insert(0,str(Path(__file__).resolve().parent))
from build_windows import vs_env,CLANG,ROOT
from prepare_arm_cpu import eligible
body='DEFINE_REX_FUNC(sub_12345678) {\n\tREX_FUNC_PROLOGUE();\n'+('\tctx.r3.u64 = ctx.r3.u64+ctx.r4.u64;\n'*20)+'\treturn;\n}\n'
assert eligible('sub_12345678',body)
for reject in ['sub_12345670(ctx, base);','REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);','REX_ENTER_GLOBAL_LOCK();','P2_TASK_BASE_WRITE(ctx, base, 0, "test");','REX_MM_STORE_U32(0, 1);','unknown(&ctx);','ctx.reserved = 0;','if(ctx.r3.u32) return;']:
 assert not eligible('sub_12345678',body.replace('\treturn;',reject+'\n\treturn;')),reject
out=ROOT/'build/switch-hot';p=out/'arm_cpu_host_test.cpp';exe=out/'arm_cpu_disabled_test.exe'
args=[str(CLANG),'/nologo','/std:c++latest','/O2','/EHsc','/MD','/DP2_LEAF_LOCALS=0','/DP2_ARM_VMX=0','/DSPDLOG_FMT_EXTERNAL','/DSPDLOG_COMPILED_LIB','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/clang:-msse4.1','/clang:-ffp-contract=off','/clang:-fno-strict-aliasing','/clang:-Wno-ignored-attributes','/I'+str(ROOT/'project'),'/I'+str(ROOT/'project/src'),'/I'+str(ROOT/'tools/bin/rexglue-0.10.0/win-amd64/include'),'/I'+str(out),str(p),'/Fe:'+str(exe),'/Fo:'+str(exe.with_suffix('.obj'))]
subprocess.run(args,env=vs_env(),check=True);subprocess.run([str(exe)],check=True)
print('PASS compile-OFF baseline, full context/memory fixture comparisons;8 unsafe leaf categories rejected')