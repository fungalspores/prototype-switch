#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mercenaries 2 (HANDOFF B:111): depth_to_float.cs.glsl as a fragment pass, for the shadow
// cascades written into their atlas. On this GPU a compute dispatch between render passes costs
// a switch of engines (a wait for idle) each way; masseffect-nx measured its conversions at
// 0.1 ms as fragment passes against 0.4-0.5 ms as compute.
//
// Rebuild depth_to_float_fs.h after editing:
//   glslangValidator -S frag -V --target-env vulkan1.1
//     --vn kDepthToFloatFs -o depth_to_float_fs.h depth_to_float_fs.frag.glsl
#extension GL_EXT_samplerless_texture_functions : require
layout(set = 0, binding = 0) uniform texture2D depth_buffer;
layout(push_constant) uniform Params {
  // The source pixel of the first destination pixel, and that destination pixel.
  ivec2 origin;
  ivec2 dest_offset;
  // Bit 0: a texel nothing was drawn into (depth 0, the far plane of the reversed light depth)
  // becomes -1 (p2_native_shadow_empty_lit).
  uint flags;
};
layout(location = 0) out float dest;
void main() {
  ivec2 c = origin + (ivec2(gl_FragCoord.xy) - dest_offset);
  float depth = texelFetch(depth_buffer, c, 0).r;
  if ((flags & 1u) != 0u && depth <= 0.0) {
    depth = -1.0;
  }
  dest = depth;
}
