from pathlib import Path
import tomllib,json,re,hashlib
root=Path(__file__).resolve().parents[1]
cfg=lambda p:tomllib.loads((root/'sd-configs'/p/'prototype2.toml').read_text(encoding='utf-8-sig'))
base=cfg('30-binding-refresh'); trial=cfg('32-skinning-checked')
assert trial['p2_skinning_selftest'] is True
assert trial.pop('p2_skinning_local') is True
assert trial.pop('p2_skinning_verify_calls')==128
base.pop('p2_skinning_local');base.pop('p2_skinning_verify_calls')
assert trial==base
assert (root/'sd-configs/32-skinning-checked/stacks_profile.flag').read_bytes()==b'off'
archive=root/'build/nro-archive'
tag='prototype2-4d8f61b7-binding-refresh'
assert hashlib.sha256((archive/(tag+'.nro')).read_bytes()).hexdigest()=='4d8f61b7163e3acf5a82ae392f1989c2fed8b171bfd28cdcc2203d1385b6e77a'
nm=(archive/(tag+'.nm.txt')).read_text()
symbol=lambda n:int(re.search(r'^([0-9a-f]+) T '+n+r'$',nm,re.M)[1],16)
assert symbol('sub_829F5978')!=symbol('__imp__sub_829F5978')
report=json.loads((root/'build/switch-hot/direct_calls_report.json').read_text())
assert 'sub_829F5978' in report['strong_hooks'] and 'sub_829F5978' not in report['converted_targets']
log=(root/'build/hw-runs/run44-4d8f61b7-guest-cpu-samples/prototype2_055.log').read_text(encoding='utf-8-sig')
assert '[p2-skin-selftest] PASS 128 ARM64 cases' in log
elf=(archive/(tag+'.elf')).read_bytes()
assert b'[p2-skin] DIFFERENTIAL MISMATCH call=' in elf and b'p2_skinning_local' in elf
s=(root/'project/src/switch_skinning.cpp').read_text()
for text in ['failed.store(true','region.reference.data()','ctx.fpscr.restoreGuestBits','checks.fetch_add','P2SkinningFinitePositions']:
 assert text in s
print('PASS same exact NRO, protected strong skin hook, latest128ARM selftests,128 real checks/automatic original restore, all other runtime values identical, samplingoff')