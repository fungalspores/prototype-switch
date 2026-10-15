from pathlib import Path
import hashlib,json,re,tomllib
root=Path(__file__).resolve().parents[1]
source=root/'build/switch/prototype2.nro'; sha=hashlib.sha256(source.read_bytes()).hexdigest(); tag=f'prototype2-{sha[:8]}-direct-calls'
nm=(root/'build/nro-archive'/f'{tag}.nm.txt').read_text()
report=json.loads((root/'build/switch-hot/direct_calls_report.json').read_text())
strong=set(re.findall(r'^[0-9a-f]+ T (sub_[0-9A-F]{8})$',nm,re.M))
assert strong==set(report['strong_hooks']),strong
assert not strong&set(report['converted_targets'])
assert re.search(r'^0+ T _start$',nm,re.M)
gen=root/'project/generated/default'
assert report['dispatch_hashes']=={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [gen/'prototype2_init.cpp',gen/'prototype2_register.cpp']}
for p in gen.glob('prototype2_recomp.*.cpp'):
    for name in strong: assert 'P2_DIRECT_CALL('+name+')' not in p.read_text()
assert tomllib.loads((root/'sd-configs/27-direct-calls/prototype2.toml').read_text(encoding='utf-8-sig'))==tomllib.loads((root/'sd-configs/26-task-timeout-fix/prototype2.toml').read_text(encoding='utf-8-sig'))
ninja=(root/'build/switch/build.ninja').read_text()
blocks=[b for b in re.split(r'\n(?=build )',ninja) if re.match(r'build CMakeFiles/prototype2_recomp\.dir/[^\n]*prototype2_recomp\.\d+\.cpp\.obj:',b)]
assert len(blocks)==195 and all('-DP2_DIRECT_CALLS=1' in b and '-DP2_GUEST_STACK_AUDIT=0' in b for b in blocks)
lines=(root/'build/direct-call-inlining.0.log').read_text().splitlines()
pairs=[]
for line in lines:
    m=re.search(r'Inlined (?:void )?(__imp__sub_[0-9A-F]{8}).*?into.*?(__imp__sub_[0-9A-F]{8})',line)
    if m and m[1]!=m[2]: pairs.append({'callee':m[1],'caller':m[2]})
assert pairs,'No actual inter-function inlining evidence in TU0'
result={'sha256':sha,'tag':tag,'bytes':source.stat().st_size,'strong_hooks':sorted(strong),'runtime_values_identical_to_successful_baseline':True,'actual_inter_function_inline_records_TU0':len(pairs),'inline_examples':pairs[:5],'baseline_bytes':80955589,'gpu_and_collision_behavior':'unchanged by intended source scope; hardware verification pending'}
(root/'build/switch-hot/direct_call_build_check.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print('PASS '+json.dumps(result))
