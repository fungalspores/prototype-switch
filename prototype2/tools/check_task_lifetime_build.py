from pathlib import Path
import json,re,tomllib
root=Path(__file__).resolve().parents[1]
ninja=(root/'build/switch/build.ninja').read_text()
commands=[block for block in re.split(r'\n(?=build )',ninja) if re.match(r'build CMakeFiles/prototype2_recomp\.dir/[^\n]*prototype2_recomp\.\d+\.cpp\.obj:',block)]
assert len(commands)==195,len(commands)
for command in commands:
    assert '-DP2_GUEST_STACK_AUDIT=0' in command
    assert '-DP2_TASK_LIFETIME_AUDIT=1' in command
nm=(root/'build/nro-archive/prototype2-a3bb4b51-task-lifetime-no-stack-audit.nm.txt').read_text()
for fn in ['sub_82C2CC60','sub_822DA288']:
    assert re.search(r'^[0-9a-f]+ T '+fn+r'$',nm,re.M),fn
for fn in ['P2TaskBaseWrite','P2DumpTaskHistory']:
    assert re.search(r'^[0-9a-f]+ T '+fn+r'\(',nm,re.M),fn
cfg=tomllib.loads((root/'sd-configs/25-task-lifetime-history/prototype2.toml').read_text(encoding='utf-8-sig'))
assert cfg['p2_task_lifetime_audit'] and cfg['p2_purecall_audit']
assert cfg['p2_msr_lock_mode']==5 and cfg['p2_native_exact_vertex_check']
for key in ['p2_guest_stack_audit','p2_job_register_audit','p2_skinning_local','p2_native_copy_ab','p2_native_fragment_copies','p2_fiber_context_audit','p2_native_gpu_profile']:
    assert not cfg[key],key
assert (root/'sd-configs/25-task-lifetime-history/stacks_profile.flag').read_bytes()==b'off'
print('PASS actual Ninja compile rules: 195 functions files stack audit OFF, targeted history ON; strong hooks + record/dump linked; profile25 validated')
