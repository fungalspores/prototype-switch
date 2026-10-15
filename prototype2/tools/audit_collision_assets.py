"""Read-only comparison of extracted world archives against the original GOD stream."""
from pathlib import Path
import sys, importlib.util, hashlib, json
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from god_extract import GodStream
spec=importlib.util.spec_from_file_location('xiso','D:/Mercenaries2-switch/reference/masseffect-nx/tools/extract_iso.py')
x=importlib.util.module_from_spec(spec); spec.loader.exec_module(x)
src=GodStream('E:/domload/Prototype 2_XBOX360/415608A7/00007000/AF99834FBE0D26800B34.data')
base,_=x.find_partition(src,scan_limit=src.size); sector,size=x.read_volume_descriptor(src,base)
entries=[(p,e) for p,e in x.walk(src,base,sector,size) if not e['is_dir']]
report={'partition':hex(base),'files':len(entries),'manifest':[], 'hashes':[]}
for p,e in entries:
 local=ROOT/'assets/game_root'/p
 report['manifest'].append({'path':p,'expected_size':e['size'],'exists':local.exists(),'size_ok':local.exists() and local.stat().st_size==e['size']})
selected=[(p,e) for p,e in entries if p.lower().endswith('.rcf') or p.lower() in ('cementfiles.p3d','default.xex','scripts/init.lua','manifests/default.tod')]
selected.sort(key=lambda pe:(pe[0].lower() not in ('cells.rcf','boot.rcf'),pe[0].lower()))
for p,e in selected:
 src.seek(base+e['sector']*x.SECTOR); h=hashlib.sha256(); left=e['size']
 while left:
  data=src.read(min(left,4*1024*1024))
  if not data: raise RuntimeError('short original '+p)
  h.update(data); left-=len(data)
 row={'path':p,'size':e['size'],'original_sha256':h.hexdigest()}
 for label,folder in [('extracted',ROOT/'assets/game_root'),('sd_staging',ROOT/'sd/switch/Prototype2/game_root')]:
  f=folder/p
  if f.exists():
   with f.open('rb') as fh: row[label+'_sha256']=hashlib.file_digest(fh,'sha256').hexdigest()
   row[label+'_match']=row[label+'_sha256']==row['original_sha256']
  else: row[label+'_match']=None
 report['hashes'].append(row)
 (ROOT/'build/collision_asset_integrity.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
 print(p+' original/extracted='+str(row['extracted_match'])+' staging='+str(row['sd_staging_match']),flush=True)
print('manifest mismatches:',[r for r in report['manifest'] if not r['size_ok']],flush=True)