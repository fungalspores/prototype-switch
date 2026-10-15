#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One triangle covering the viewport (draw 3 vertices), for depth_to_float_fs.frag.glsl.
//
// Rebuild fullscreen_vs.h after editing:
//   glslangValidator -S vert -V --target-env vulkan1.1
//     --vn kFullscreenVs -o fullscreen_vs.h fullscreen_vs.vert.glsl
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
