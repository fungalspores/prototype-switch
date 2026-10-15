from pathlib import Path
import json,tomllib,hashlib,re,struct
root=Path(__file__).resolve().parents[1]
cfg=lambda name:tomllib.loads((root/'sd-configs'/name/'prototype2.toml').read_text(encoding='utf-8-sig'))
assert cfg('33-fpscr-roundtrip')==cfg('32-skinning-checked')
assert (root/'sd-configs/33-fpscr-roundtrip/stacks_profile.flag').read_bytes()==b'off'
r=json.loads((root/'build/switch-hot/guest_lto_build_check.json').read_text())
assert r['tag'].endswith('-fpscr-roundtrip')
elf=(root/'build/nro-archive'/(r['tag']+'.elf')).read_bytes()
assert b'[p2-fpscr-selftest] PASS 4 ARM64 modes' in elf
assert b'ARM FPSCR selftest failed before guest launch' in elf
nm=(root/'build/nro-archive'/(r['tag']+'.nm.txt')).read_text()
assert re.search(r'^[0-9a-f]+ T prototype2_RunFPSCRSelfTest\(\)$',nm,re.M)
assembly=(root/'build/switch-hot/fpscr_selftest.asm').read_text()
assert 'prototype2_RunFPSCRSelfTest' in assembly
assert re.search(r'frinti\s+d\d+,\s*d\d+',assembly)
assert re.search(r'msr\s+fpcr,',assembly) and re.search(r'mrs\s+x\d+,\s*fpcr',assembly)
source=(root/'reference/masseffect-nx/sdk/include/rex/ppc/context.h').read_text()
assert 'inverse[Platform::GuestToHost[guest] >> RoundShift] = guest;' in source
table=re.search(r'^([0-9a-f]+) R rex::ppc::FPSCRRegister::HostToGuest$',nm,re.M)
assert table, 'Compiled inverse table missing'
table_address=int(table[1],16)
shoff=struct.unpack_from('<Q',elf,40)[0]
shsize,shcount=struct.unpack_from('<HH',elf,58)
modes=None
for i in range(shcount):
 section=struct.unpack_from('<IIQQQQIIQQ',elf,shoff+i*shsize)
 _,kind,_,address,offset,size,*_=section
 if kind!=8 and address<=table_address and table_address+32<=address+size:
  modes=struct.unpack_from('<4Q',elf,offset+table_address-address)
  break
assert modes==(0,2,3,1), modes
r['ARM_compiled_host_to_guest_inverse']=list(modes)
r['runtime_settings_identical']=True
r['profile_change']='No runtime value changes versus32; SDK inverse map corrected and mandatory ARM startup test added'
r['fpscr_header_sha256']=hashlib.sha256((root/'reference/masseffect-nx/sdk/include/rex/ppc/context.h').read_bytes()).hexdigest()
r['ARM_FPCR_selftest_linked']=True
r['limits']='Actual Switch startup PASS4, skin128 differential results, stability/visuals/FPS pending; collision cause not established'
(root/'build/switch-hot/fpscr_build_check.json').write_text(json.dumps(r,indent=2),encoding='utf-8')
print('PASS inverse source, linked ARM FRINTI/MSR/MRS selftest, profile33==32/offflag, archive '+r['tag'])