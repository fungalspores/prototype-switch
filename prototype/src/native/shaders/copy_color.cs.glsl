#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copies a rectangle of a color image into another of the same format, for
// resolves. vkCmdCopyImage on NVK draws through its meta path, which creates
// image views and buffers for every copy and destroys them when the command
// buffer is reset: with about 30 resolves a frame that was 5% of the command
// ring thread. This needs no objects of its own (shaderStorageImageWrite-
// WithoutFormat covers every destination format with one pipeline).
//
// Rebuild copy_color_cs.h after editing:
//   glslangValidator -S comp -V --target-env vulkan1.1
//     --vn kCopyColorCs -o copy_color_cs.h copy_color.cs.glsl
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform texture2D source;
layout(set = 0, binding = 1) uniform writeonly image2D dest;
layout(push_constant) uniform Params {
  ivec2 source_origin;
  ivec2 dest_origin;
  uvec2 size;
  // Mercenaries 2 resolves (HANDOFF section 52): samples per pixel of a multisampled surface
  // drawn with its samples as pixels (averaged, as the guest resolve does), the resolve's
  // exponent bias as a factor, and the destination format's range: 0 none, 1 [0, 1],
  // 2 [0, 31.875] (7e3).
  ivec2 sample_scale;
  float scale;
  uint clamp_mode;
};
void main() {
  uvec2 p = gl_GlobalInvocationID.xy;
  if (p.x >= size.x || p.y >= size.y) {
    return;
  }
  ivec2 base = (source_origin + ivec2(p)) * sample_scale;
  vec4 color = vec4(0.0);
  for (int y = 0; y < sample_scale.y; ++y) {
    for (int x = 0; x < sample_scale.x; ++x) {
      color += texelFetch(source, base + ivec2(x, y), 0);
    }
  }
  color *= scale / float(sample_scale.x * sample_scale.y);
  if (clamp_mode == 1u) {
    color = clamp(color, vec4(0.0), vec4(1.0));
  } else if (clamp_mode == 2u) {
    color = vec4(clamp(color.rgb, vec3(0.0), vec3(31.875)), clamp(color.a, 0.0, 1.0));
  }
  imageStore(dest, dest_origin + ivec2(p), color);
}
