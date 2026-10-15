"""Validate the diagnostic hook against the original function's actual ABI."""
from pathlib import Path
import sys, tomllib
sys.path.insert(0,str(Path(__file__).resolve().parent))
from prepare_switch_hot import extract
root=Path(__file__).resolve().parents[1]
pump=extract('sub_822BAA78')
assert pump.count('__savegprlr_26(ctx, base);')==1
assert pump.count('__restgprlr_26(ctx, base);')==1 and pump.count('return;')==1
save=extract('__savegprlr_26'); restore=extract('__restgprlr_26')
for i in range(26,32):
    offset=-56+(i-26)*8
    assert f'REX_STORE_U64(ctx.r1.u32 + {offset}, ctx.r{i}.u64);' in save
    assert f'ctx.r{i}.u64 = REX_LOAD_U64(ctx.r1.u32 + {offset});' in restore
scene=extract('sub_821D2D20')
assert 'ctx.lr = 0x821D2D3C;\n\tsub_822BAA78(ctx, base);' in scene
cfg=tomllib.loads((root/'sd-configs/22-job-abi-audit/prototype2.toml').read_text(encoding='utf-8-sig'))
assert cfg['p2_job_register_audit'] and cfg['p2_msr_lock_mode']==5
assert not cfg['p2_native_copy_ab'] and not cfg['p2_native_fragment_copies']
assert not cfg['p2_skinning_local'] and cfg['p2_skinning_selftest'] and cfg['p2_skinning_verify_calls']==32
assert not cfg['p2_guest_affinity'] and not cfg['p2_guest_priorities']
print('PASS original task-pump save/restore ABI, parent call site, compute-only diagnostic configuration')
