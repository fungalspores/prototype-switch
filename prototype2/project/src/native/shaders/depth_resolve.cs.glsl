#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turns a depth buffer copied into a buffer into the two forms a game can read
// a depth resolve back as, in place:
// - words[i]: depth as a 32-bit float in [0, 1], for k_24_8 fetches (the
//   translated shaders sample those as R32_SFLOAT);
// - words[packed_offset + i]: what the resolve leaves in Xbox 360 memory,
//   (depth24 << 8) | stencil, for fetches as any other 32-bit format.
// Depth comes either as a 24-bit unorm in the low bits of each word (from a
// D24_UNORM_S8_UINT image) or as a float; stencil, when present, is one byte
// per texel starting at word stencil_offset.
//
// Rebuild depth_resolve_cs.h after editing:
//   glslangValidator -S comp -V --target-env vulkan1.1
//     --vn kDepthResolveCs -o depth_resolve_cs.h depth_resolve.cs.glsl
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) buffer Data { uint words[]; };
layout(push_constant) uniform Params {
  uint count;
  // Texels per row of work groups: large copies need a second dimension.
  uint row_words;
  uint stencil_offset;
  uint packed_offset;
  // Bit 0: depth is 24-bit unorm (else float). Bit 1: stencil present.
  uint flags;
};
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * row_words;
  if (i >= count) {
    return;
  }
  uint raw = words[i];
  uint depth24;
  float depth;
  if ((flags & 1u) != 0u) {
    depth24 = raw & 0xFFFFFFu;
    depth = min(float(depth24) / 16777215.0, 1.0);
  } else {
    depth = clamp(uintBitsToFloat(raw), 0.0, 1.0);
    // min: 1.0 * 16777215.0 + 0.5 rounds to 2^24 in float32, which << 8 wraps to 0
    // (HANDOFF B:114: cleared depth then reads as nearest, the sky keeps old pixels).
    depth24 = min(uint(depth * 16777215.0 + 0.5), 0xFFFFFFu);
  }
  uint stencil = 0u;
  if ((flags & 2u) != 0u) {
    stencil = (words[stencil_offset + (i >> 2u)] >> ((i & 3u) * 8u)) & 0xFFu;
  }
  words[packed_offset + i] = (depth24 << 8u) | stencil;
  words[i] = floatBitsToUint(depth);
}
