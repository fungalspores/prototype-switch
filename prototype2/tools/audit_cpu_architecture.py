"""Inventory global CPU optimization candidates without editing generated code.
Direct-call eligibility is conservative: any guest address mentioned in app/SDK
sources or manifest is protected, plus every strong sub_X symbol of the last NRO.
This report is not permission to bypass a hook or enable ABI-changing flags.
"""
from pathlib import Path
import collections,json,re
ROOT=Path(__file__).resolve().parents[1]
protected=set(); paths=[]
for folder in (ROOT/'project/src',ROOT/'reference/masseffect-nx/sdk/src',ROOT/'reference/masseffect-nx/sdk/include'):
    paths.extend(p for p in folder.rglob('*') if p.suffix in ('.h','.hpp','.cpp','.cc','.c','.inc'))
paths.append(ROOT/'project/prototype2_manifest.toml')
for p in paths:
    for x in re.findall(r'(?<![0-9a-fA-F])(82[0-9a-fA-F]{6})(?![0-9a-fA-F])',p.read_text(errors='replace')):
        protected.add('sub_'+x.upper())
strong=set()
for line in (ROOT/'build/nro-archive/prototype2-5ff9df81-job-abi-audit-final.nm.txt').read_text(errors='replace').splitlines():
    m=re.match(r'^[0-9a-f]+ T (sub_[0-9A-F]{8})$',line)
    if m: strong.add(m[1])
protected|=strong
counts=collections.Counter(); calls=collections.Counter(); total_bytes=0; files=0
for p in (ROOT/'project/generated/default').glob('prototype2_recomp.*.cpp'):
    files+=1;s=p.read_text(encoding='utf-8-sig');total_bytes+=p.stat().st_size
    counts['functions']+=s.count('DEFINE_REX_FUNC(')
    counts['global_atomic_regions']+=s.count('REX_ENTER_GLOBAL_LOCK();')
    counts['hot_atomic_regions']+=s.count('REX_ENTER_HOT_LOCK(ea);')
    counts['indirect_dispatch_sites']+=s.count('REX_CALL_INDIRECT_FUNC(')
    counts['ctx_register_mentions']+=len(re.findall(r'ctx\.(?:r\d+|f\d+|v\d+|cr\d|xer|ctr|lr)\b',s))
    calls.update(re.findall(r'(?<![\w])((?:sub_)[0-9A-F]{8})\(ctx, base\);',s))
eligible={fn:n for fn,n in calls.items() if fn not in protected}
report={'generated_files':files,'generated_source_bytes':total_bytes,'counts':dict(counts),
        'direct_call_sites_total':sum(calls.values()),'direct_call_sites_conservatively_eligible':sum(eligible.values()),
        'protected_guest_addresses':len(protected),'strong_hooks':sorted(strong),
        'candidate_targets':eligible,'status':'Dry-run only; no codegen ABI or call targets modified.',
        'architecture_flags':'armv8-a+crc+crypto, mtune=cortex-a57, mtp=soft, TLS local-exec, O3; fp-contract off, strict-aliasing off',
        'not_enabled':['global register-local flags','direct calls','LTO','PGO','lock-free reservation replacement']}
(ROOT/'build/switch-hot/architecture_cpu_audit.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in report.items() if k!='candidate_targets'},indent=2))
