from pathlib import Path
import struct,re,json,sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from scan_code_pointers import sections
ROOT=Path(__file__).resolve().parents[1];img=(ROOT/'build/image_dump.bin').read_bytes();base=0x82000000
u32=lambda a:struct.unpack_from('>I',img,a-base)[0]
valid=lambda a,n=4:base<=a<=base+len(img)-n
rows=[]
for name,va,size,exe in sections(img):
 if exe: continue
 for off in range(va,min(va+size,len(img))-3,4):
  if struct.unpack_from('>I',img,off)[0]!=0x82c2cc60: continue
  vtable=base+off-20;locator=u32(vtable-4) if valid(vtable-4) else 0;typename=''
  if valid(locator,20):
   td=u32(locator+12)
   if valid(td,9):
    raw=img[td-base+8:td-base+264].split(b'\0',1)[0]
    if raw.startswith(b'.?A') and all(32<=x<127 for x in raw): typename=raw.decode('ascii')
  rows.append({'slot_address':hex(base+off),'candidate_slot20_vtable':hex(vtable),'locator':hex(locator),'type':typename})
print('image size',len(img),'purecall references',len(rows))
for r in rows[:60]: print(r)
print('code around abort fallthrough',[(hex(a),hex(u32(a))) for a in range(0x82c34204,0x82c34218,4)])
(ROOT/'build/switch-hot/purecall_vtable_candidates.json').write_text(json.dumps(rows,indent=2),encoding='utf-8')
