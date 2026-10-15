"""ABI-preserving leaf register cache and exact integer VMX lowering.
No guest calls, locks, callbacks, task instrumentation or unknown context uses
are eligible. All touched register images are initialized from ctx and committed
on EVERY return. Volatile guest memory and FPSCR operations remain unchanged.
"""
from pathlib import Path
import re,json,hashlib
ROOT=Path(__file__).resolve().parents[1]
REG=r'(?:r\d+|f\d+|v\d+|cr\d|xer|lr|ctr)'
FUN=re.compile(r'DEFINE_REX_FUNC\((\w+)\) \{[\s\S]*?(?=\nDEFINE_REX_FUNC|\Z)')
BLOCK=re.compile(r'\n/\* P2 leaf (?:declarations|commit) begin \*/[\s\S]*?/\* P2 leaf (?:declarations|commit) end \*/\n')
LEFT=re.compile(r'\tfor \(size_t i = 0; i < \(16 - \(ea & 0xF\)\); i\+\+\)\n\t\tREX_STORE_U8\(ea \+ i, ctx\.(v\d+)\.u8\[15 - i\]\);')
RIGHT=re.compile(r'\tfor \(size_t i = 0; i < \(ea & 0xF\); i\+\+\)\n\t\tREX_STORE_U8\(ea - i - 1, ctx\.(v\d+)\.u8\[i\]\);')
def canonical(text):
 text=BLOCK.sub('',text)
 text=re.sub(r'\bp2_leaf_('+REG+r')\b',r'ctx.\1',text)
 text=re.sub(r'\tP2_VMX_STORE_LEFT\(base, ea, ctx\.(v\d+)\.u8\);',lambda m:'\tfor (size_t i = 0; i < (16 - (ea & 0xF)); i++)\n\t\tREX_STORE_U8(ea + i, ctx.'+m[1]+'.u8[15 - i]);',text)
 text=re.sub(r'\tP2_VMX_STORE_RIGHT\(base, ea, ctx\.(v\d+)\.u8\);',lambda m:'\tfor (size_t i = 0; i < (ea & 0xF); i++)\n\t\tREX_STORE_U8(ea - i - 1, ctx.'+m[1]+'.u8[i]);',text)
 return text.replace('P2_VMX_PERMUTE(', 'rex::ppc::simde_mm_perm_epi8_(')
def eligible(name,text):
 if not name.startswith('sub_') or name in {'sub_829F5978','sub_829F4C58','sub_822BAA78','sub_822DA288','sub_821D2D20','sub_82C2CC60'}:return False
 body=text[text.index('{')+1:text.rindex('}')]
 if any(x in body for x in ['(ctx, base)','REX_CALL','_LOCK','P2_TASK','REX_MM_','setjmp','longjmp','throw','REX_FATAL','REX_ERROR','ctx.reserved']):return False
 if re.search(r'\bctx\b(?!\.)',body):return False
 fields=set(re.findall(r'ctx\.(\w+)',body))
 if any(not re.fullmatch(REG,x) and x!='fpscr' for x in fields):return False
 if len(re.findall(r'(?m)^[\t ]*return;',body))!=len(re.findall(r'\breturn\s*;',body)):return False
 return len(re.findall(r'ctx\.',body))>=40

def transform(text):
 text=canonical(text); name=re.match(r'DEFINE_REX_FUNC\((\w+)\)',text)[1]
 original=text
 text,nleft=LEFT.subn(lambda m:'\tP2_VMX_STORE_LEFT(base, ea, ctx.'+m[1]+'.u8);',text)
 text,nright=RIGHT.subn(lambda m:'\tP2_VMX_STORE_RIGHT(base, ea, ctx.'+m[1]+'.u8);',text)
 perms=text.count('rex::ppc::simde_mm_perm_epi8_(')
 text=text.replace('rex::ppc::simde_mm_perm_epi8_(', 'P2_VMX_PERMUTE(')
 cached=eligible(name,original); regs=[]
 if cached:
  regs=sorted(set(re.findall(r'ctx\.('+REG+r')\b',text)))
  text=re.sub(r'ctx\.('+REG+r')\b',lambda m:'p2_leaf_'+m[1],text)
  decl='\n/* P2 leaf declarations begin */\n#if defined(P2_LEAF_LOCALS) && P2_LEAF_LOCALS\n'+''.join('\tauto p2_leaf_'+r+' = ctx.'+r+';\n' for r in regs)+'#else\n'+''.join('\tauto& p2_leaf_'+r+' = ctx.'+r+';\n' for r in regs)+'#endif\n/* P2 leaf declarations end */\n'
  opening=text.index('{'); text=text[:opening+1]+decl+text[opening+1:]
  commit='\n/* P2 leaf commit begin */\n#if defined(P2_LEAF_LOCALS) && P2_LEAF_LOCALS\n'+''.join('\tctx.'+r+' = p2_leaf_'+r+';\n' for r in regs)+'#endif\n/* P2 leaf commit end */\n'
  assert re.search(r'\breturn\s*;',text),name
  assert not re.search(r'\breturn\s+(?!;)',text),name
  text=re.sub(r'(?m)^[\t ]*return;',lambda m:commit+m[0],text)
  assert text.count('/* P2 leaf commit begin */')==len(re.findall(r'\breturn\s*;',original)),name
 assert canonical(text)==original, 'Non-reversible lowering '+name
 return text,{'name':name,'leaf':cached,'registers':regs,'left':nleft,'right':nright,'permute':perms,'sha256':hashlib.sha256(original.encode()).hexdigest()}
def prepare():
 out=ROOT/'build/switch-hot'; out.mkdir(parents=True,exist_ok=True); rows=[]; fixtures={}
 for p in (ROOT/'project/generated/default').glob('prototype2_recomp.*.cpp'):
  s=p.read_text()
  def one(m):
   t,r=transform(m[0]);r['file']=p.name
   if r['name'] in {'sub_8222AA30','sub_829FF6A8'}: fixtures[r['name']]=(canonical(m[0]),t)
   if r['leaf'] or r['left'] or r['right'] or r['permute']:rows.append(r)
   return t
  t=FUN.sub(one,s)
  if t!=s:p.write_text(t,encoding='utf-8',newline='\n')
 pch=ROOT/'project/generated/default/prototype2_pch.h'; s=pch.read_text()
 if '#include "src/arm_vmx.h"' not in s:pch.write_text(s+'\n#include "src/arm_vmx.h"\n',encoding='utf-8',newline='\n')
 report={'functions':rows,'leaf_functions':sum(x['leaf'] for x in rows),'left_sites':sum(x['left'] for x in rows),'right_sites':sum(x['right'] for x in rows),'permute_sites':sum(x['permute'] for x in rows),'status':'Compile-time opt-in; reversible source lowering, normal-return ABI preserved; host/hardware validation required'}
 assert len(fixtures)==2, 'Mandatory ARM reference fixture bodies missing'
 parts=[]
 for name,tag in [('sub_8222AA30','matrix'),('sub_829FF6A8','stream')]:
  for body,version in zip(fixtures[name],['reference','optimized']):
   parts.append(body.replace('DEFINE_REX_FUNC('+name+')','static REX_FUNC(p2_'+version+'_'+tag+')'))
 contents='\n'.join(parts); path=out/'arm_cpu_fixtures.inc'
 if not path.exists() or path.read_text()!=contents:path.write_text(contents)
 (out/'arm_cpu_lowering.json').write_text(json.dumps(report,indent=2))
 print({k:v for k,v in report.items() if k!='functions'})
if __name__=='__main__':prepare()