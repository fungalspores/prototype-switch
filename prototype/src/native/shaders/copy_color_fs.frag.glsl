#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mercenaries 2 (HANDOFF B:112): copy_color.cs.glsl as a fragment pass (a colour attachment of
// the destination's format, render area = the destination rectangle). On this GPU a compute
// dispatch between render passes switches engines and waits for idle each way; the shadow
// atlas conversion went from 2.6 to 0.6 ms a frame as a fragment pass (B:111).
//
// Rebuild copy_color_fs.h after editing:
//   glslangValidator -S frag -V --target-env vulkan1.1
//     --vn kCopyColorFs -o copy_color_fs.h copy_color_fs.frag.glsl
#extension GL_EXT_samplerless_texture_functions : require
layout(set = 0, binding = 0) uniform texture2D source;
layout(push_constant) uniform Params {
  ivec2 source_origin;
  ivec2 dest_origin;
  // As in copy_color.cs.glsl: samples per pixel (averaged), the exponent bias as a factor and
  // the destination format's range (0 none, 1 [0, 1], 2 [0, 31.875] for 7e3).
  ivec2 sample_scale;
  float scale;
  uint clamp_mode;
};
layout(location = 0) out vec4 dest;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) - dest_origin;
  ivec2 base = (source_origin + p) * sample_scale;
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
  dest = color;
}
