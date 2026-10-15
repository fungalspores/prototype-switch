from pathlib import Path
import json,tomllib,hashlib
root=Path(__file__).resolve().parents[1]
cfg=lambda profile:tomllib.loads((root/'sd-configs'/profile/'prototype2.toml').read_text(encoding='utf-8-sig'))
assert cfg('29-binding-cache-probe')==cfg('28-guest-lto')
r=json.loads((root/'build/switch-hot/guest_lto_build_check.json').read_text())
assert r['tag'].endswith('-binding-cache-probe')
elf=(root/'build/nro-archive'/(r['tag']+'.elf')).read_bytes()
assert b'[native] binding cache per frame: hit' in elf
assert hashlib.sha256((root/'build/nro-archive'/(r['tag']+'.nro')).read_bytes()).hexdigest()==r['sha256']
(root/'build/switch-hot/binding_probe_build_check.json').write_text(json.dumps(r,indent=2),encoding='utf-8')
print('PASS linked cache report, profile29 identical28, archived NRO hash '+r['sha256'])