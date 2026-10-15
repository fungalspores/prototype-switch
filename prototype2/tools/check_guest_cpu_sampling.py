from pathlib import Path
import hashlib,json,tomllib,re
root=Path(__file__).resolve().parents[1]
cfg=lambda name:tomllib.loads((root/'sd-configs'/name/'prototype2.toml').read_text(encoding='utf-8-sig'))
assert cfg('31-guest-cpu-samples')==cfg('30-binding-refresh')
flag=(root/'sd-configs/31-guest-cpu-samples/stacks_profile.flag').read_bytes()
assert flag==b'20 180 20'
ms,delay,duration=map(int,flag.split())
assert 2<=ms<=100 and 0<delay<=600 and 0<duration<=120
source=(root/'reference/masseffect-nx/sdk/src/ui/switch_perf.cpp').read_text()
assert 'p2_sample_ms = ms;' in source and 'p2_delay_seconds = std::min(delay, 600u);' in source
assert 'p2_duration_seconds = std::min(duration, 120u);' in source
assert 'elapsed >= p2_delay_seconds && elapsed < uint64_t(p2_delay_seconds)+p2_duration_seconds' in source
assert 'svcSetThreadActivity(h, ThreadActivity_Runnable);' in source
archive=root/'build/nro-archive/prototype2-4d8f61b7-binding-refresh.nro'
assert hashlib.sha256(archive.read_bytes()).hexdigest()=='4d8f61b7163e3acf5a82ae392f1989c2fed8b171bfd28cdcc2203d1385b6e77a'
elf=(root/'build/nro-archive/prototype2-4d8f61b7-binding-refresh.elf').read_bytes()
assert b'P2 sparse sampling: %u ms, after scene %u s, duration %u s' in elf
result={'nro':'4d8f61b7 (unchanged)','runtime_config_identical_to_profile30':True,'interval_ms':ms,'after_scene_seconds':delay,'duration_seconds':duration,'flag_sha256':hashlib.sha256(flag).hexdigest(),'limits':'Sampling briefly pauses individual threads; exclude its window from FPS comparison. One window per process after first >=1200 draw scene frame. Runtime START/END and nonzero sample count must be verified after manual run.'}
(root/'build/switch-hot/guest_cpu_sampling_check.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print('PASS '+json.dumps(result))