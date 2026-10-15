import hashlib,json,re,tomllib
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=root/'build/switch/prototype2.nro'
sha=hashlib.sha256(source.read_bytes()).hexdigest(); tag=f'prototype2-{sha[:8]}-task-timeout-fix'
nm=(root/'build/nro-archive'/f'{tag}.nm.txt').read_text()
for fn in ['sub_82C2CC60','sub_822DA288']:
    assert re.search(r'^[0-9a-f]+ T '+fn+r'$',nm,re.M)
assert 'prototype2_EnableTaskPumpTimeoutFix()' in nm
assert re.search(r'^[0-9a-f]+ [BD] p2_task_pump_timeout_fix_enabled$',nm,re.M)
cfg=tomllib.loads((root/'sd-configs/26-task-timeout-fix/prototype2.toml').read_text(encoding='utf-8-sig'))
assert cfg['p2_task_pump_timeout_fix'] and cfg['p2_task_lifetime_audit'] and cfg['p2_native_exact_vertex_check']
assert not cfg['p2_skinning_local'] and not cfg['p2_guest_stack_audit']
s=(root/'project/generated/default/prototype2_recomp.86.cpp').read_text()
assert '#if defined(__SWITCH__) || defined(P2_TASK_TIMEOUT_TEST)' in s
assert s.count('/* P2 consumed task timeout fix */')==1
ninja=(root/'build/switch/build.ninja').read_text()
blocks=[b for b in re.split(r'\n(?=build )',ninja) if re.match(r'build CMakeFiles/prototype2_recomp\.dir/[^\n]*prototype2_recomp\.\d+\.cpp\.obj:',b)]
assert len(blocks)==195
assert all('-DP2_GUEST_STACK_AUDIT=0' in b and '-DP2_TASK_LIFETIME_AUDIT=1' in b for b in blocks)
result={'sha256':sha,'tag':tag,'bytes':source.stat().st_size,'profile':'26-task-timeout-fix','checks':'source reset and platform guard; actual Ninja flags; linked initializer/enable bool/strong hooks; config enabled'}
(root/'build/switch-hot/task_timeout_build_check.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print('PASS '+json.dumps(result))
