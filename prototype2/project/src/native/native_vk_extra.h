// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mercenaries 2 port: Vulkan device functions the native renderer needs but the SDK's shared
// VulkanDevice::Functions table does not have (copy image, pipeline cache, timestamps). They are
// resolved here with vkGetDeviceProcAddr instead of growing the shared table: changing its layout
// once broke the ABI between objects (HANDOFF section 24).
#pragma once

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

namespace p2::native {

struct ExtraVk {
  PFN_vkCmdCopyImage vkCmdCopyImage = nullptr;
  PFN_vkCmdBlitImage vkCmdBlitImage = nullptr;
  PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp = nullptr;
  PFN_vkGetQueryPoolResults vkGetQueryPoolResults = nullptr;
  PFN_vkCreatePipelineCache vkCreatePipelineCache = nullptr;
  PFN_vkDestroyPipelineCache vkDestroyPipelineCache = nullptr;
  PFN_vkGetPipelineCacheData vkGetPipelineCacheData = nullptr;
  float timestampPeriod = 0.0f;
};

// Resolved on first use for the device (one device per process).
inline const ExtraVk& Extra(const rex::ui::vulkan::VulkanDevice* device) {
  static ExtraVk extra;
  static const rex::ui::vulkan::VulkanDevice* resolved_for = nullptr;
  if (resolved_for != device) {
    resolved_for = device;
    const auto& ifn = device->vulkan_instance()->functions();
    auto get = [&](const char* name) { return ifn.vkGetDeviceProcAddr(device->device(), name); };
    extra.vkCmdCopyImage = reinterpret_cast<PFN_vkCmdCopyImage>(get("vkCmdCopyImage"));
    extra.vkCmdBlitImage = reinterpret_cast<PFN_vkCmdBlitImage>(get("vkCmdBlitImage"));
    extra.vkCmdWriteTimestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(get("vkCmdWriteTimestamp"));
    extra.vkGetQueryPoolResults =
        reinterpret_cast<PFN_vkGetQueryPoolResults>(get("vkGetQueryPoolResults"));
    extra.vkCreatePipelineCache =
        reinterpret_cast<PFN_vkCreatePipelineCache>(get("vkCreatePipelineCache"));
    extra.vkDestroyPipelineCache =
        reinterpret_cast<PFN_vkDestroyPipelineCache>(get("vkDestroyPipelineCache"));
    extra.vkGetPipelineCacheData =
        reinterpret_cast<PFN_vkGetPipelineCacheData>(get("vkGetPipelineCacheData"));
    VkPhysicalDeviceProperties properties = {};
    ifn.vkGetPhysicalDeviceProperties(device->physical_device(), &properties);
    extra.timestampPeriod = properties.limits.timestampPeriod;
  }
  return extra;
}

}  // namespace p2::native
