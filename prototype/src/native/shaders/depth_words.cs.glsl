#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mercenaries 2 (HANDOFF B:91): a depth resolve the game also samples as a 32-bit color
// format, in one pass straight from the depth and stencil aspects of the render target,
// instead of depth_resolve.cs.glsl's route (image -> buffer -> compute -> buffer -> image).
// The arithmetic is depth_resolve.cs.glsl's, so both outputs keep the same bits:
// - dest: depth as a float in [0, 1], for k_24_8 fetches;
// - words: what the resolve leaves in Xbox 360 memory, (depth24 << 8) | stencil, stored
//   in an R8G8B8A8 image (byte 0 = stencil, as the buffer copy laid it out).
//
// Rebuild depth_words_cs.h after editing:
//   glslangValidator -S comp -V --target-env vulkan1.1
//     --vn kDepthWordsCs -o depth_words_cs.h depth_words.cs.glsl
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform texture2D depth_buffer;
layout(set = 0, binding = 1) uniform utexture2D stencil_buffer;
layout(set = 0, binding = 2, r32f) uniform writeonly image2D dest;
layout(set = 0, binding = 3, rgba8) uniform writeonly image2D words;
layout(push_constant) uniform Params {
  ivec2 origin;
  uvec2 size;
  // Bit 0: depth is 24-bit unorm (else float).
  uint flags;
};
void main() {
  uvec2 p = gl_GlobalInvocationID.xy;
  if (p.x >= size.x || p.y >= size.y) {
    return;
  }
  ivec2 c = origin + ivec2(p);
  float raw = texelFetch(depth_buffer, c, 0).r;
  uint depth24;
  float depth;
  if ((flags & 1u) != 0u) {
    // The sampled unorm24 value back to its integer, then depth_resolve.cs.glsl's float.
    // min: 1.0 * 16777215.0 + 0.5 rounds to 2^24 in float32, which << 8 wraps to 0
    // (HANDOFF B:114: cleared depth then reads as nearest, the sky keeps old pixels).
    depth24 = min(uint(clamp(raw, 0.0, 1.0) * 16777215.0 + 0.5), 0xFFFFFFu);
    depth = min(float(depth24) / 16777215.0, 1.0);
  } else {
    depth = clamp(raw, 0.0, 1.0);
    depth24 = min(uint(depth * 16777215.0 + 0.5), 0xFFFFFFu);
  }
  uint stencil = texelFetch(stencil_buffer, c, 0).r & 0xFFu;
  imageStore(dest, c, vec4(depth));
  imageStore(words, c, unpackUnorm4x8((depth24 << 8u) | stencil));
}
