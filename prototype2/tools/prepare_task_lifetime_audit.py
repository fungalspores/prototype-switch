"""Instrument statically proven stores of the task base vtable; no guest behavior changes."""
import re, json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK=' /* P2 task base write */'
VT=0x82008C50

def instrument(source):
    # Regeneration and repeated preparation produce the same output.
    source=''.join(line for line in source.splitlines(keepends=True) if MARK not in line)
    known={}; output=[]; sites=[]; fn=''
    for number,line in enumerate(source.splitlines(keepends=True),1):
        m=re.match(r'DEFINE_REX_FUNC\((\w+)\)',line)
        if m: fn=m[1]; known.clear()
        if re.match(r'loc_[0-9A-F]+:',line) or '(ctx, base);' in line or 'REX_CALL_INDIRECT_FUNC(' in line:
            known.clear()
        store=re.match(r'\tREX_STORE_U32\((.+), ctx\.r(\d+)\.u32\);',line)
        if store and known.get(int(store[2]))==VT:
            address=store[1]
            output.append(f'\tP2_TASK_BASE_WRITE(ctx, base, {address}, "{fn}");{MARK}\n')
            sites.append({'function':fn,'line':number,'address':address})
        assignment=re.match(r'\tctx\.r(\d+)\.\w+ = (.+);',line)
        if assignment:
            reg=int(assignment[1]); expr=assignment[2]; value=None
            if re.fullmatch(r'-?\d+',expr): value=int(expr)
            else:
                add=re.fullmatch(r'ctx\.r(\d+)\.s64 \+ (-?\d+)',expr)
                move=re.fullmatch(r'ctx\.r(\d+)\.u64',expr)
                if add and int(add[1]) in known: value=known[int(add[1])]+int(add[2])
                if move and int(move[1]) in known: value=known[int(move[1])]
            if value is None: known.pop(reg,None)
            else: known[reg]=value&0xffffffff
        output.append(line)
    return ''.join(output),sites

def prepare():
    gen=ROOT/'project/generated/default'; sites=[]
    for path in sorted(gen.glob('prototype2_recomp.*.cpp')):
        source=path.read_text(encoding='utf-8'); result,found=instrument(source)
        if result!=source: path.write_text(result,encoding='utf-8',newline='\n')
        sites.extend(dict(file=path.name,**x) for x in found)
    if len(sites)<30: raise RuntimeError(f'Unexpected base-vtable coverage: {len(sites)}; inspect codegen')
    pch=gen/'prototype2_pch.h'; original_pch=pch.read_text(encoding='utf-8'); source=original_pch
    source=source.replace('#include "task_lifetime_audit.h"', '#include "../../src/task_lifetime_audit.h"')
    if source!=original_pch: pch.write_text(source,encoding='utf-8',newline='\n')
    include='#include "../../src/task_lifetime_audit.h"'
    if include not in source: pch.write_text(source+'\n'+include+'\n',encoding='utf-8',newline='\n')
    out=ROOT/'build/switch-hot'; out.mkdir(parents=True,exist_ok=True)
    (out/'task_base_write_sites.json').write_text(json.dumps(sites,indent=2),encoding='utf-8')
    print(f'Task base-vtable write sites: {len(sites)} in {len(set(x["function"] for x in sites))} functions')
    return sites
if __name__=='__main__': prepare()
