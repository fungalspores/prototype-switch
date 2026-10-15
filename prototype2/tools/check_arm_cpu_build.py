from pathlib import Path
import re,json,tomllib,hashlib,struct,sys
root=Path(__file__).resolve().parents[1]
r=json.loads((root/'build/switch-hot/guest_lto_build_check.json').read_text())
assert r['tag'].endswith('-'+(sys.argv[1] if len(sys.argv)>1 else 'arm-cpu-lowering'))
ninja=(root/'build/switch/build.ninja').read_text(); blocks=re.split(r'\n(?=build )',ninja)
guests=[b for b in blocks if re.match(r'build CMakeFiles/prototype2_recomp\.dir/[^\n]*prototype2_recomp\.\d+\.cpp\.obj:',b)]
assert len(guests)==195
assert all('-DP2_LEAF_LOCALS=1' in b and '-DP2_ARM_VMX=1' in b for b in guests)
app=[b for b in blocks if 'switch_arm_cpu_selftest.cpp.obj:' in b]
assert len(app)==1 and '-DP2_LEAF_LOCALS=1' in app[0] and '-DP2_ARM_VMX=1' in app[0]
settings=lambda n:tomllib.loads((root/'sd-configs'/n/'prototype2.toml').read_text(encoding='utf-8-sig'))
assert settings('35-arm-cpu-lowering')==settings('34-history-off')
assert (root/'sd-configs/35-arm-cpu-lowering/stacks_profile.flag').read_bytes()==b'off'
report=json.loads((root/'build/switch-hot/arm_cpu_lowering.json').read_text())
assert report['leaf_functions']>=1700 and report['left_sites']==561 and report['right_sites']==141 and report['permute_sites']==300
assert not any(x['name'] in r['strong_hooks'] and x['leaf'] for x in report['functions'])
elf=(root/'build/nro-archive'/(r['tag']+'.elf')).read_bytes()
for message in [b'[p2-arm-cpu-selftest] PASS',b'ARM CPU lowering selftest failed before guest launch',b'[p2-fpscr-selftest] PASS 4 ARM64 modes',b'[p2-skin] DIFFERENTIAL MISMATCH']:
 assert message in elf,message
asm=(root/'build/switch-hot/arm_cpu_selftest.asm').read_text()
assert re.search(r'\btbl\s+v\d+\.16b,\s*\{v\d+\.16b(?:,\s*v\d+\.16b|-v\d+\.16b)\}',asm),'Native two-table TBL not found'
stream=(root/'build/switch-hot/arm_cpu_stream.asm').read_text()
def elf_bytes(address,length):
 shoff=struct.unpack_from('<Q',elf,40)[0];shsize,shcount=struct.unpack_from('<HH',elf,58)
 for i in range(shcount):
  _,kind,_,start,offset,size,*_=struct.unpack_from('<IIQQQQIIQQ',elf,shoff+i*shsize)
  if kind!=8 and start<=address and address+length<=start+size:return elf[offset+address-start:offset+address-start+length]
 raise AssertionError('Address not backed by ELF section')
if not re.search(r'\brev64\s+v\d+\.16b',stream):
 page=re.search(r'adrp\s+x(\d+),\s*([0-9a-f]+)',stream)
 load=re.search(r'ldr\s+q(\d+),\s*\[x'+page[1]+r',\s*#(\d+)\]',stream)
 assert load and re.search(r'tbl\s+v\d+\.16b,\s*\{v\d+\.16b\},\s*v'+load[1]+r'\.16b',stream)
 assert elf_bytes(int(page[2],16)+int(load[2]),16)==bytes(range(15,-1,-1)), 'Compiled reversal mask differs'
assert re.search(r'\bstr\s+q\d+,\s*\[x\d+\]',stream), 'Aligned guest vector store missing'
assert 'inverse[Platform::GuestToHost[guest] >> RoundShift] = guest;' in (root/'reference/masseffect-nx/sdk/include/rex/ppc/context.h').read_text()
r.update({'runtime_settings_identical_to':'34-history-off','leaf_functions':report['leaf_functions'],'partial_store_sites':702,'permute_sites':300,'native_TBL2_and_partial_store_disassembly_verified':True,'limits':'Host fixtures passed, ARM execution mandatory startup selftest pending; scene FPS benefit and collisions unresolved'})
(root/'build/switch-hot/arm_cpu_build_check.json').write_text(json.dumps(r,indent=2))
print('PASS native ARM lowering flags/opcodes, protected hooks/dispatch, unchanged runtime35/34 and startup fail-closed selftest',r['tag'])
nm=(root/'build/nro-archive'/(r['tag']+'.nm.txt')).read_text()
match=re.search(r'^([0-9a-f]+) R rex::ppc::FPSCRRegister::HostToGuest$',nm,re.M)
assert match,'Compiled FPSCR inverse table missing'
address=int(match[1],16); shoff=struct.unpack_from('<Q',elf,40)[0];shsize,shcount=struct.unpack_from('<HH',elf,58)
modes=None
for i in range(shcount):
 _,kind,_,start,offset,size,*_=struct.unpack_from('<IIQQQQIIQQ',elf,shoff+i*shsize)
 if kind!=8 and start<=address and address+32<=start+size:
  modes=struct.unpack_from('<4Q',elf,offset+address-start);break
assert modes==(0,2,3,1),modes
r['ARM_compiled_host_to_guest_inverse']=list(modes)
sites=json.loads((root/'build/switch-hot/task_base_write_sites.json').read_text())
r['task_base_write_sites_preserved']=len(sites)
assert len(sites)==123
(root/'build/switch-hot/arm_cpu_build_check.json').write_text(json.dumps(r,indent=2))
print('PASS linked ARM FPSCR inverse and all123 targeted task write sites preserved')