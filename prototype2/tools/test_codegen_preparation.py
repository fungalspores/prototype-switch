"""Prove repeated preparation leaves generated output unchanged and lock entries balanced."""
from pathlib import Path
import subprocess,hashlib,sys,json
root=Path(__file__).resolve().parents[1]
gen=root/'project/generated/default'
def run(): subprocess.run([sys.executable,'-I',str(root/'tools/prepare_switch_codegen.py')],check=True)
def hashes():
    return {p.name:(hashlib.sha256(p.read_bytes()).hexdigest(),p.stat().st_mtime_ns) for p in [gen/'prototype2_pch.h',*sorted(gen.glob('prototype2_recomp.*.cpp'))]}
run()
first=hashes()
run()
second=hashes()
changed=[name for name in first if first[name]!=second[name]]
if changed: raise SystemExit(f'FAIL preparation is not idempotent: {changed}')
regions=entries=0
for p in gen.glob('prototype2_recomp.*.cpp'):
    s=p.read_text(encoding='utf-8')
    regions+=s.count('REX_ENTER_GLOBAL_LOCK();')
    entries+=s.count('REX_ENTER_ADDR_LOCK(ea);')+s.count('REX_ENTER_HOT_LOCK(ea);')
assert regions==entries and regions>0
print(f'PASS preparation idempotence: {len(first)} file hashes and mtimes unchanged; {regions} atomic regions, {entries} lock entries')
(root/'build/switch-hot/codegen_check.json').write_text(json.dumps({'files':len(first),'regions':regions,'lock_entries':entries,'idempotent':True},indent=2))
