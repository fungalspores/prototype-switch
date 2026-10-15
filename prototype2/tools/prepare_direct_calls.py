"""Allow Switch inlining of unhooked guest calls, preserving public dispatch entries."""
from pathlib import Path
import re,json,hashlib
ROOT=Path(__file__).resolve().parents[1]
CALL=re.compile(r'(?<![\w])(?:P2_DIRECT_CALL\((sub_[0-9A-F]{8})\)|(sub_[0-9A-F]{8}))\(ctx, base\);')
def convert(text,protected):
    targets=set()
    def change(m):
        name=m[1] or m[2]
        if name in protected: return name+'(ctx, base);'
        targets.add(name); return 'P2_DIRECT_CALL('+name+')(ctx, base);'
    return CALL.sub(change,text),targets

def protected_targets():
    result=set()
    for folder in ['project/src','reference/masseffect-nx/sdk/src','reference/masseffect-nx/sdk/include','tools','tests']:
        for p in (ROOT/folder).rglob('*'):
            if p.suffix not in ['.h','.hpp','.cpp','.cc','.c','.inc','.py']: continue
            result.update('sub_'+x.upper() for x in re.findall(r'(?<![0-9a-fA-F])(82[0-9a-fA-F]{6})(?![0-9a-fA-F])',p.read_text(errors='replace')))
    manifest=(ROOT/'project/prototype2_manifest.toml').read_text()
    result.update('sub_'+x.upper() for x in re.findall(r'82[0-9a-fA-F]{6}',manifest))
    # Public list of strong dispatch overrides; no archived NRO or nm file is required.
    strong=set(json.loads((ROOT/'config/strong-overrides.json').read_text(encoding='utf-8'))['symbols'])
    result|=strong
    return result,strong

def prepare():
    gen=ROOT/'project/generated/default'; out=ROOT/'build/switch-hot'; out.mkdir(exist_ok=True)
    protected,strong=protected_targets(); used=set(); count=0; same_file=0
    dispatch={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [gen/'prototype2_init.cpp',gen/'prototype2_register.cpp']}
    for p in sorted(gen.glob('prototype2_recomp.*.cpp')):
        s=p.read_text(encoding='utf-8'); index=p.name.split('.')[1]
        # Remove our previous header stanza before deriving canonical output.
        s=re.sub(r'\n#if defined\(__SWITCH__\) && defined\(P2_DIRECT_CALLS\) && P2_DIRECT_CALLS\n#include "p2_direct_funcs\.\d+\.inc"\n#endif\n','\n',s,count=1)
        result,targets=convert(s,protected)
        defined=set(re.findall(r'DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)',s))
        same_file+=sum(1 for m in CALL.finditer(s) if (m[1] or m[2]) in targets&defined)
        count+=result.count('P2_DIRECT_CALL('); used|=targets
        header=out/f'p2_direct_funcs.{index}.inc'
        contents='// Derived unhooked targets; public dispatch remains unchanged.\n'+''.join('REX_EXTERN(__imp__'+t+');\n' for t in sorted(targets))
        if not header.exists() or header.read_text()!=contents: header.write_text(contents,encoding='utf-8',newline='\n')
        first=result.index('\n')
        result=result[:first]+'\n#if defined(__SWITCH__) && defined(P2_DIRECT_CALLS) && P2_DIRECT_CALLS\n#include "'+header.name+'"\n#endif'+result[first:]
        # Normalize one newline after the per-file include stanza.
        result=result.replace('#endif\n\n\nDEFINE_REX_FUNC','#endif\n\nDEFINE_REX_FUNC',1)
        if result!=p.read_text(encoding='utf-8'): p.write_text(result,encoding='utf-8',newline='\n')
    pch=gen/'prototype2_pch.h'; s=pch.read_text(encoding='utf-8')
    marker='// P2 direct unhooked calls (Switch compile option)'
    if marker not in s:
        s+='\n'+marker+'\n#if defined(__SWITCH__) && defined(P2_DIRECT_CALLS) && P2_DIRECT_CALLS\n#define P2_DIRECT_CALL(name) __imp__##name\n#else\n#define P2_DIRECT_CALL(name) name\n#endif\n'
        pch.write_text(s,encoding='utf-8',newline='\n')
    assert not used&protected
    assert dispatch=={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [gen/'prototype2_init.cpp',gen/'prototype2_register.cpp']}
    report={'sites':count,'targets':len(used),'same_file_sites':same_file,'protected_count':len(protected),'strong_hooks':sorted(strong),'converted_targets':sorted(used),'dispatch_hashes':dispatch,'status':'Switch option only; no runtime FPS claim'}
    (out/'direct_calls_report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(f'Direct calls: {count} sites, {same_file} same-file candidates; {len(protected)} protected addresses; public dispatch unchanged')
    return report
if __name__=='__main__': prepare()
