from pathlib import Path
import hashlib,json,tomllib
root=Path(__file__).resolve().parents[1]
cfg=lambda name:tomllib.loads((root/'sd-configs'/name/'prototype2.toml').read_text(encoding='utf-8-sig'))
before=cfg('29-binding-cache-probe'); after=cfg('30-binding-refresh')
assert after.pop('p2_native_binding_revalidate') is True and after==before
r=json.loads((root/'build/switch-hot/guest_lto_build_check.json').read_text())
assert r['tag'].endswith('-binding-refresh')
elf=(root/'build/nro-archive'/(r['tag']+'.elf')).read_bytes()
for text in [b'[native] binding revalidation={}',b'[native] binding refresh per frame:',b'p2_native_binding_revalidate']:
 assert text in elf, text
assert hashlib.sha256((root/'build/nro-archive'/(r['tag']+'.nro')).read_bytes()).hexdigest()==r['sha256']
r['runtime_settings_identical']=False
r['baseline_settings_identical_except_revalidation_control']=True
r['profile_change']='p2_native_binding_revalidate=true only'
r['slot_source_epoch_check']='Correctness fix active regardless of reuse flag; old SlotView predicate no longer byte-identical'
(root/'build/switch-hot/binding_refresh_build_check.json').write_text(json.dumps(r,indent=2),encoding='utf-8')
print('PASS linked refresh control/counters, profile difference only reuse flag, archive hash '+r['sha256'])