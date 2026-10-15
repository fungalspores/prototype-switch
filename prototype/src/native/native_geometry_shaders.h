// SPDX-License-Identifier: GPL-3.0-or-later
//
// Geometry shaders for the primitive types Vulkan does not have. The key has
// the same layout as ReXGlue's VulkanPipelineCache::GeometryShaderKey, which
// this code comes from (see the .cpp).

#pragma once

#include <cstdint>
#include <functional>

#include <rex/ui/vulkan/device.h>

namespace p2::native {

enum class PipelineGeometryShader : uint32_t {
  kNone,
  kPointList,
  kRectangleList,
  kQuadList,
};

union GeometryShaderKey {
  uint32_t key;
  struct {
    PipelineGeometryShader type : 2;
    uint32_t interpolator_count : 5;
    uint32_t user_clip_plane_count : 3;
    uint32_t user_clip_plane_cull : 1;
    uint32_t has_vertex_kill_and : 1;
    uint32_t has_point_size : 1;
    uint32_t has_point_coordinates : 1;
    // PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives.
    uint32_t point_ps_ucp_mode : 2;
  };
  GeometryShaderKey() : key(0) {}
  bool operator==(const GeometryShaderKey& other) const { return key == other.key; }
};

// VK_NULL_HANDLE (and an error in the log) if the module cannot be created.
VkShaderModule CreateGeometryShader(const rex::ui::vulkan::VulkanDevice* vulkan_device,
                                    GeometryShaderKey key);

}  // namespace p2::native
