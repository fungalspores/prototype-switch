"""Conservative local CFG audit of guest stack adjustments in generated functions.
A nonzero local return offset is a candidate, not proof: tail-called fragments,
alloca and fiber/stack helpers may own the epilogue. Never patches generated code.
"""
from pathlib import Path
import re,json,collections
ROOT=Path(__file__).resolve().parents[1]
results=[]; total=0; unknown=0
for file in (ROOT/'project/generated/default').glob('prototype2_recomp.*.cpp'):
    s=file.read_text(encoding='utf-8-sig')
    funcs=list(re.finditer(r'DEFINE_REX_FUNC\((\w+)\) \{',s))
    for idx,fn in enumerate(funcs):
        total+=1
        body=s[fn.end():funcs[idx+1].start() if idx+1<len(funcs) else len(s)]
        if not re.search(r'// st[wd]u[x]? r1,|// addi r1,r1,',body): continue
        lines=body.splitlines(); labels={}
        for i,line in enumerate(lines):
            m=re.match(r'(loc_[0-9A-F]+):',line.strip())
            if m: labels[m[1]]=i
        queue=collections.deque([(0,0)]); seen=set(); returns=set(); dynamic=False
        while queue and len(seen)<10000:
            i,offset=queue.popleft()
            if i>=len(lines) or (i,offset) in seen or abs(offset)>0x10000: continue
            seen.add((i,offset)); line=lines[i].strip()
            m=re.match(r'// st[wd]u r1,(-?\d+)\(r1\)',line)
            if m: offset+=int(m[1])
            m=re.match(r'// addi r1,r1,(-?\d+)',line)
            if m: offset+=int(m[1])
            if re.match(r'// (?:mr|lwz|ld|addi|add|subf|rlwinm|rldicr|stwux|stdux) r1,',line) and not re.match(r'// addi r1,r1,-?\d+$',line):
                dynamic=True; continue
            if line=='return;': returns.add(offset); continue
            if line.startswith('if ') and line.endswith('return;'): returns.add(offset)
            m=re.search(r'goto (loc_[0-9A-F]+);',line)
            if m:
                if m[1] in labels: queue.append((labels[m[1]],offset))
                if line.startswith('goto '): continue
            # Add all switch targets, including later cases that linear traversal misses.
            if line.startswith('switch ('):
                depth=0
                for branch in lines[i:]:
                    depth+=branch.count('{')-branch.count('}')
                    g=re.search(r'goto (loc_[0-9A-F]+);',branch)
                    if g and g[1] in labels: queue.append((labels[g[1]],offset))
                    if depth==0: break
                continue
            if '__builtin_trap()' in line: continue
            queue.append((i+1,offset))
        if dynamic: unknown+=1
        bad=sorted(x for x in returns if x)
        if bad:
            results.append({'function':fn[1],'file':str(file.relative_to(ROOT)),
                'return_offsets':bad,'dynamic_stack_path':dynamic,
                'tail':lines[-16:],
                'calls':re.findall(r'^\s*(\w+)\(ctx, base\);',body,re.M)})
report={'functions':total,'dynamic_stack_functions':unknown,'candidates':results,
        'limitation':'Local CFG only: offsets may be restored by a tail fragment/helper. No candidate is an automatic fix.'}
out=ROOT/'build/switch-hot/stack_cfg_audit.json';out.write_text(json.dumps(report,indent=2),encoding='utf-8')
print('functions',total,'local imbalance candidates',len(results),'dynamic stacks',unknown)
for r in results:
    if -96 in r['return_offsets']: print(r['function'],r['return_offsets'],'dynamic',r['dynamic_stack_path'],'last calls',r['calls'][-4:])
