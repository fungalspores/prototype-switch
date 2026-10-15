from pathlib import Path
import re,json,tomllib,hashlib
root=Path(__file__).resolve().parents[1]
r=json.loads((root/'build/switch-hot/arm_cpu_build_check.json').read_text())
assert r['tag'].endswith('-blocking-record-queue')
config=lambda n:tomllib.loads((root/'sd-configs'/n/'prototype2.toml').read_text(encoding='utf-8-sig'))
assert config('36-blocking-record-queue')==config('35-arm-cpu-lowering')
flag=(root/'sd-configs/36-blocking-record-queue/stacks_profile.flag').read_bytes()
assert flag==b'2 180 40'
source=(root/'project/src/native/native_renderer.cpp').read_text();header=(root/'project/src/native/blocking_record_queue.h').read_text()
part=source[source.index('  static constexpr uint32_t kRecordQueueSize'):source.index('  RendererStats& stats()')]
assert 'std::this_thread::yield' not in part and 'wait_for' not in part
assert 'record_work.Drain();' in part and 'record_work.Close();' in source
assert part.index('ExecuteDrawCommand(*draw);')<part.index('record_work.CompleteFront();')
assert 'wait_for' not in header and 'yield' not in header
asm=(root/'build/switch-hot/blocking_record_thread.asm').read_text()
assert ('RecordThread' in asm or 'Initialize()::{lambda()#2}' in asm) and re.search(r'condition_variable.*wait',asm)
assert 'svcSetThreadCoreMask' in asm and 'svcSetThreadPriority' in asm
assert 'sched_yield' not in asm and 'nanosleep' not in asm
wait=(root/'build/switch-hot/blocking_record_wait.asm').read_text()
assert re.search(r'condition_variable.*wait',wait) and 'sched_yield' not in wait
elf=(root/'build/nro-archive'/(r['tag']+'.elf')).read_bytes()
assert b'periodic polls=0' in elf and b'Native draw recorder closed before submission' in elf
prof=(root/'reference/masseffect-nx/sdk/src/ui/switch_perf.cpp').read_text()
assert 'ms >= 2 && ms <= 100' in prof and 'elapsed < uint64_t(p2_delay_seconds)+p2_duration_seconds' in prof
r.update({'queue_capacity':4096,'queue_batch':32,'blocking_recorder_disassembly_verified':True,'runtime_settings_identical_to':'35-arm-cpu-lowering','sampling_flag':flag.decode(),'sampling_flag_sha256':hashlib.sha256(flag).hexdigest(),'queue_header_sha256':hashlib.sha256((root/'project/src/native/blocking_record_queue.h').read_bytes()).hexdigest(),'limits':'Hardware park/counter/FPS/collision/visual validation pending; exclude bounded180–220scene-second sampling window from performance comparisons'})
(root/'build/switch-hot/blocking_record_build_check.json').write_text(json.dumps(r,indent=2))
print('PASS ARM condition-variable waits/no yield loops in consumer and producer wait, execute-before-complete/drain/close, settings36==35, bounded2ms/180s/40s sampling',r['tag'])