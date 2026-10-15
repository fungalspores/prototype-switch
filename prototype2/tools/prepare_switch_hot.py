"""Derive an exact register-local skinning kernel; never edit ReXGlue output.
The original algorithm, rounding, packs, and memory writes are retained. The sole
helper is inlined because it intentionally returns vector constants outside the ABI.
"""
from pathlib import Path
import re, hashlib, json
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'switch-hot'

def extract(name):
    matches = []
    for p in (ROOT / 'project/generated/default').glob('prototype2_recomp.*.cpp'):
        s = p.read_text(encoding='utf-8')
        mark = 'DEFINE_REX_FUNC(' + name + ') {'
        a = s.find(mark)
        if a >= 0:
            b = s.find('\nDEFINE_REX_FUNC(', a + len(mark))
            matches.append(s[a: b if b >= 0 else len(s)].strip())
    if len(matches) != 1:
        raise RuntimeError(f'{name}: expected exactly one body, got {len(matches)}')
    return matches[0]

def generate():
    helper = extract('sub_829F4C58')
    original = extract('sub_829F5978')
    calls = re.findall(r'^\s*([\w]+)\(ctx, base\);', original, re.M)
    if calls != ['sub_829F4C58'] or original.count('return;') != 1:
        raise RuntimeError('Skinning control flow changed; audit before regenerating')
    hb = helper[helper.index('{') + 1:helper.rindex('}')]
    hb = hb.replace('\n\tREX_FUNC_PROLOGUE();', '').replace('\n\tuint32_t ea{};', '')
    if hb.count('return;') != 1 or 'goto ' in hb:
        raise RuntimeError('Constant helper changed')
    hb = hb.replace('\n\treturn;', '')
    body = original[original.index('{')+1:original.rindex('}')]
    body = body.replace('\n\tREX_FUNC_PROLOGUE();', '')
    body = body.replace('sub_829F4C58(ctx, base);', '// Inline non-ABI constant setup.' + hb)
    regs = sorted(set(re.findall(r'ctx\.((?:r|f|v)\d+|cr\d|xer|lr|ctr|reserved)\b', body)))
    declarations = '\n'.join(f'\tauto p2_{r} = ctx.{r};' for r in regs)
    body = re.sub(r'ctx\.(' + '|'.join(regs) + r')\b', lambda m: 'p2_' + m[1], body)
    stores = '\n'.join(f'\tctx.{r} = p2_{r};' for r in regs)
    body = body.replace('\treturn;', stores + '\n\treturn;')
    digest = hashlib.sha256((original + helper).encode()).hexdigest()
    fast = f'// Derived from ReXGlue output SHA256 {digest}. Do not hand-edit.\nstatic REX_FUNC(p2_skinning_local) {{\n{declarations}\n{body}\n}}\n'
    ref = original.replace('DEFINE_REX_FUNC(sub_829F5978)', 'static REX_FUNC(p2_skinning_reference)')
    ref = ref.replace('sub_829F4C58(ctx, base);', '// Inline setup for standalone reference.' + hb)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT/'p2_skinning_kernel.inc').write_text(fast, encoding='utf-8')
    (OUT/'p2_skinning_reference.inc').write_text(ref, encoding='utf-8')
    (OUT/'provenance.json').write_text(json.dumps({'algorithm_sha256': digest, 'local_registers': regs, 'calls': calls}, indent=2))
    print(f'skinning: {len(regs)} local registers; source {digest}')

if __name__ == '__main__':
    generate()
