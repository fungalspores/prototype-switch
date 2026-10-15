from pathlib import Path
import re,random,json
r=Path(__file__).resolve().parents[1]
s=(r/'project/src/native/native_renderer.cpp').read_text()
a=s.index('RenderTarget* Renderer::Impl::GetRenderTarget');b=s.index('\nbool Renderer::Impl::BeginPass',a);f=s[a:b]
assert 'REXCVAR_DEFINE_BOOL(p2_native_depth_attachment_only, false,' in s
assert 'std::clamp(min_height, 16u, 2048u)' in f
assert 'rt->storage_height = rt->attachment_only_depth ? 2048u : rt->height;' in f
assert 'if (!rt->attachment_only_depth) info.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;' in f
assert f.index('if (old->attachment_only_depth')<f.index('auto rt = std::make_unique')
grow=f[f.index('if (old->attachment_only_depth'):f.index('auto rt = std::make_unique')]
assert grow.index('EndPass(')<grow.index('old->height =')<grow.index('++rt_generation')<grow.index('return old;')
assert 'rendering.renderArea.extent = {rt->width, rt->storage_height};' in f
assert 'rendering.pStencilAttachment = HasStencil(rt->format) ? &depth : nullptr;' in f
assert 'depth.clearValue.depthStencil = {1.0f, 0};' in f
assert 'barrier.dstAccessMask = init_access;' in f
assert 'vkCmdPipelineBarrier(cmd(), init_stage,' in f
# Design differential model, NOT execution of Vulkan or the actual renderer.
# Baseline reallocates, clears new rows and copies old rows; reserved storage
# clears once, while every draw/copy is limited to unchanged logical extents.
rng=random.Random(415608);cases=0
for trial in range(256):
 height=16;width=rng.randrange(1,9);baseline=[0xffffffff]*(width*height);reserved=[0xffffffff]*(width*2048)
 for step in range(96):
  requested=max(16,min(rng.randrange(1,2300),2048));nextheight=(requested+15)&~15
  if nextheight>height:
   baseline.extend([0xffffffff]*(width*(nextheight-height)));height=nextheight
  for _ in range(5):
   address=rng.randrange(width*height);value=rng.getrandbits(32)
   baseline[address]=value;reserved[address]=value
  assert baseline==reserved[:width*height]
  assert all(x==0xffffffff for x in reserved[width*height:])
  cases+=1
# Real depth copies use integer texelFetch, not normalized image-size UVs.
for file in ('depth_to_float.cs.glsl','depth_words.cs.glsl','depth_to_float_fs.frag.glsl'):
 text=(r/'project/src/native/shaders'/file).read_text();assert 'texelFetch(' in text and 'textureSize(' not in text
report={'status':'PASS source contracts and growth design differential model','model_sequences':cases,'limits':'Not Vulkan execution. ARM native translation unit compiled in job256; whole NRO/hardware/visual/depth/stencil/ZCULL allocation and memory impact still pending. Flag default false; current console unchanged.'}
(r/'build/switch-hot/depth_attachment_trial_check.json').write_text(json.dumps(report,indent=2));print(json.dumps(report))