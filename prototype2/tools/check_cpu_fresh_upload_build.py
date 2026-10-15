from pathlib import Path
import json,tomllib,hashlib
r=Path(__file__).resolve().parents[1];cfg=lambda d:tomllib.loads((r/'sd-configs'/d/'prototype2.toml').read_text())
a=cfg('36-blocking-record-queue');b=cfg('39-cpu-fresh-upload')
assert b.pop('p2_native_dynamic_buffer_upload') is True and b==a
assert (r/'sd-configs/39-cpu-fresh-upload/stacks_profile.flag').read_bytes()==b'off'
meta=json.loads((r/'build/switch-hot/arm_cpu_build_check.json').read_text());assert meta['tag'].endswith('-cpu-fresh-upload')
elf=(r/'build/nro-archive'/(meta['tag']+'.elf')).read_bytes()
for message in (b'[p2-buffer-selftest] PASS 16384 frames',b'ARM dynamic buffer cache selftest failed before guest launch',b'buffer CPU scans: full_hash_bytes=',b'dynamic buffer fresh uploads='):
 assert message in elf,message
s=(r/'project/src/native/native_renderer.cpp').read_text();assert 'p2_native_depth_attachment_only' not in s
assert 'REXCVAR_DEFINE_BOOL(p2_native_dynamic_buffer_upload, false,' in s
assert 'known.epoch, known.frame_local' in s
assert 'using VertexRangeKey = BufferRangeKey;' in s
assert 'known.detector.CheckExact(data, bytes, vertex_hash_bytes)' in s
header=(r/'project/src/native/dynamic_buffer_stamp.h').read_text()
assert 'frame_local==other.frame_local' in header and 'epoch==other.epoch' in header
assert 'stamp_={0,frame,true}' in header and 'stamp_={content,0,false}' in header
app=(r/'project/src/prototype2_app.h').read_text()
assert app.index('prototype2_RunDynamicBufferSelfTest())')<app.index('    prototype2_RunSkinningSelfTests();')
meta.update({'profile':'39-cpu-fresh-upload','TOML_delta':'only p2_native_dynamic_buffer_upload=true relative restored36','profiler':'off','runtime_settings_identical':False,'runtime_settings_identical_to':'36 with only dynamic_buffer_upload added','buffer_policy_sha256':hashlib.sha256(header.encode()).hexdigest(),'limits':'Host policy/actual cache methods/ARM compilation pass; mandatory hardware startup16384frames and actual avoided-byte counters/FPS/geometry/crashes pending. No 30FPS/collision repair claim. GPU depth candidate reverted.'})
(r/'build/switch-hot/cpu_fresh_upload_build_check.json').write_text(json.dumps(meta,indent=2))
print('PASS exactCPUartifact/flags/hooks/ARMtimeout+rounding/sameTOML exceptoneCPUflag/offprofile/mandatoryARMbufferselftest',meta['tag'])