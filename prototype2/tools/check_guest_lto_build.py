from pathlib import Path
import hashlib,json,re,struct,tomllib,sys
root=Path(__file__).resolve().parents[1]; source=root/'build/switch/prototype2.nro'
sha=hashlib.sha256(source.read_bytes()).hexdigest(); label=sys.argv[1] if len(sys.argv)>1 else 'guest-lto'; tag=f'prototype2-{sha[:8]}-{label}'
nm=(root/'build/nro-archive'/f'{tag}.nm.txt').read_text(); report=json.loads((root/'build/switch-hot/direct_calls_report.json').read_text())
strong=set(re.findall(r'^[0-9a-f]+ T (sub_[0-9A-F]{8})$',nm,re.M))
symbols={name:(int(addr,16),kind) for addr,kind,name in re.findall(r'^([0-9a-f]+) ([A-Za-z]) (\S+)$',nm,re.M)}
hooks=set(report['strong_hooks'])
for name in hooks:
 assert symbols[name][1]=='T' and symbols[name][0]!=symbols['__imp__'+name][0], name
aliases=0
for name in report['converted_targets']:
 if name in symbols and '__imp__'+name in symbols:
  assert symbols[name][0]==symbols['__imp__'+name][0], 'Unexpected override: '+name
  aliases+=1
assert aliases>0
# LTO aliases are checked by address above.
assert re.search(r'^0+ T _start$',nm,re.M)
assert re.search(r'^[0-9a-f]+ [BD] p2_task_pump_timeout_fix_enabled$',nm,re.M)
assert ' T __imp__sub_822BAA78' in nm
ninja=(root/'build/switch/build.ninja').read_text(); blocks=re.split(r'\n(?=build )',ninja)
guest=[b for b in blocks if re.match(r'build CMakeFiles/prototype2_recomp\.dir/[^\n]*prototype2_recomp\.\d+\.cpp\.obj:',b)]
assert len(guest)==195
for b in guest:
 assert '-flto' in b and '-fno-fat-lto-objects' in b and '-frounding-math' in b
 assert '-ffp-contract=off' in b and '-fno-strict-aliasing' in b and '-DP2_DIRECT_CALLS=1' in b
native=[b for b in blocks if re.match(r'build CMakeFiles/prototype2\.dir/[^\n]*src/native/[^\n]+\.cpp\.obj:',b)]
assert len(native)>0 and all('-flto' not in b for b in native)
assert '-flto=2 -flto-partition=balanced' in ninja
cfg=lambda name:tomllib.loads((root/f'sd-configs/{name}/prototype2.toml').read_text(encoding='utf-8-sig'))
assert cfg('28-guest-lto')==cfg('27-direct-calls') and cfg('28-guest-lto')['p2_task_pump_timeout_fix']
gen=root/'project/generated/default'
assert report['dispatch_hashes']=={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [gen/'prototype2_init.cpp',gen/'prototype2_register.cpp']}
assembly=(root/'build/switch-hot/guest_lto_task_pump.asm').read_text()
# ctx.r31 is the 32nd 64-bit GPR (offset248); inspect the explicit reset in actual ARM code.
assert '<__imp__sub_822BAA78>:' in assembly
assert re.search(r'str\s+xzr,\s*\[x\d+,\s*#248\]',assembly), 'Consumed r31 reset not found; manual disassembly review required'
# The enable flag is loaded through the GOT, not named by objdump at this site.
assert re.search(r'blr\s+x2\s*\n[^\n]*ldrb\s+w0,\s*\[x26\]\s*\n[^\n]*tbz\s+w0,\s*#0,[^\n]*\n[^\n]*str\s+xzr,\s*\[x19,\s*#248\]',assembly)
got=re.search(r'adrp\s+x26,\s*([0-9a-f]+)[^\n]*\n[^\n]*ldr\s+x26,\s*\[x26,\s*#(\d+)\]',assembly)
assert got, 'Enable flag GOT load not found'
got_slot=int(got[1],16)+int(got[2])
linked_elf=(root/'build/nro-archive'/f'{tag}.elf').read_bytes()
shoff=struct.unpack_from('<Q',linked_elf,40)[0]
shsize,shcount=struct.unpack_from('<HH',linked_elf,58)
got_value=None
for i in range(shcount):
 section=struct.unpack_from('<IIQQQQIIQQ',linked_elf,shoff+i*shsize)
 _,kind,_,address,offset,size,*_=section
 if kind!=8 and address<=got_slot and got_slot+8<=address+size:
  got_value=struct.unpack_from('<Q',linked_elf,offset+got_slot-address)[0]
  break
assert got_value==symbols['p2_task_pump_timeout_fix_enabled'][0], 'GOT slot does not resolve to timeout fix flag'
elf=(root/'build/nro-archive'/f'{tag}.elf').read_bytes(); assert elf[:4]==b'\x7fELF' and struct.unpack_from('<H',elf,18)[0]==183
result={'sha256':sha,'tag':tag,'bytes':source.stat().st_size,'baseline_bytes':80996549,'guest_files_lto':len(guest),'native_files_without_lto':len(native),'strong_hooks':sorted(hooks),'public_aliases_verified':aliases,'timeout_flag_GOT_slot':hex(got_slot),'start_address':0,'task_timeout_reset_verified_in_ARM_disassembly':True,'runtime_settings_identical':True,'limits':'Hardware execution/FPS/FP-selftest and visual correctness pending; collision defect unresolved'}
(root/'build/switch-hot/guest_lto_build_check.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print('PASS '+json.dumps(result))
