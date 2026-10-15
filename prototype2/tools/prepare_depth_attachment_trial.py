from pathlib import Path
p=Path('project/src/native/native_renderer.cpp');s=p.read_text();assert 'p2_native_depth_attachment_only' not in s,'Trial already applied; do not apply twice';start=s.index('RenderTarget* Renderer::Impl::GetRenderTarget');end=s.index('\nbool Renderer::Impl::BeginPass',start)
f=s[start:end]; Path('build/switch-hot/native_renderer_before_depth_attachment.cpp').write_text(s)
def replace(a,b):
 global f
 assert f.count(a)==1,(a,f.count(a))
 f=f.replace(a,b)
replace('    if (old->height >= min_height) {\n      return old;\n    }','''    if (old->height >= min_height) {
      return old;
    }
    if (old->attachment_only_depth && old->storage_height >= min_height) {
      BeginRecording();
      EndPass(PassEnd::TargetCreate);
      old->height = rex::align(min_height, 16u);
      ++rt_generation;
      return old;
    }''')
replace('  rt->height = rex::align(min_height, 16u);','''  rt->height = rex::align(min_height, 16u);
  rt->attachment_only_depth = key.is_depth && REXCVAR_GET(p2_native_depth_attachment_only);
  rt->storage_height = rt->attachment_only_depth ? 2048u : rt->height;''')
replace('  info.extent = {rt->width, rt->height, 1};','  info.extent = {rt->width, rt->storage_height, 1};')
replace('VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;','''VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (!rt->attachment_only_depth) info.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;''')
replace('  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;','''  const VkPipelineStageFlags init_stage = rt->attachment_only_depth
      ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
      : VK_PIPELINE_STAGE_TRANSFER_BIT;
  const VkAccessFlags init_access = rt->attachment_only_depth
      ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = init_access;''')
replace('VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,','VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, init_stage,')
replace('  if (key.is_depth) {\n    VkClearDepthStencilValue clear = {1.0f, 0};','''  if (rt->attachment_only_depth) {
    VkRenderingAttachmentInfo depth = {};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = rt->view;
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo rendering = {};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = {rt->width, rt->storage_height};
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;
    rendering.pStencilAttachment = HasStencil(rt->format) ? &depth : nullptr;
    dfn().vkCmdBeginRendering(c, &rendering);
    dfn().vkCmdEndRendering(c);
  } else if (key.is_depth) {
    VkClearDepthStencilValue clear = {1.0f, 0};''')
replace('  if (old) {','''  if (old) {
    if (rt->attachment_only_depth)
      REX_FATAL("[native] unexpected attachment-only depth replacement");''')
replace('                           VK_ACCESS_TRANSFER_WRITE_BIT,','                           init_access,')
replace('vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT,','vkCmdPipelineBarrier(cmd(), init_stage,')
replace('  return result;','''  if (result->attachment_only_depth)
    REXLOG_INFO("[native] attachment-only depth: logical {} rows, storage {} rows; transfer writes=0",
                result->height, result->storage_height);
  return result;''')
s=s[:start]+f+s[end:]
s=s.replace('REXCVAR_DEFINE_BOOL(p2_native_msaa, true,','''REXCVAR_DEFINE_BOOL(p2_native_depth_attachment_only, false, "Prototype2",
                    "Experimental attachment clears and reserved depth height without transfer writes; restart required")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_msaa, true,''',1)
mark='  uint64_t last_used_frame = 0;';assert s.count(mark)==1
s=s.replace(mark,'  bool attachment_only_depth = false;\n  uint32_t storage_height = 0;\n'+mark)
p.write_text(s)
print('Prepared default-OFF attachment-only depth candidate; existing logical extent retained; no console write')