// SPDX-License-Identifier: BSD-3-Clause
// Adapted from this project's SDK VulkanPipelineCache executable-statistics query.
// Keep device function-table ABI unchanged; diagnostics never require new slots.
#pragma once
#include <vector>
#include <rex/logging.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/switch_shader_stats.h>
namespace p2::native {
inline void LogNativeShaderStats(const rex::ui::vulkan::VulkanDevice* device,
                                  VkPipeline pipeline, uint64_t vs_hash, uint64_t vs_mod,
                                  uint64_t stats_ps, uint64_t ps_mod) {
  const VkDevice vk = device->device();
    // Resolve separately: never add slots to the SDK's shared device function table.
    const auto get_proc = device->vulkan_instance()->functions().vkGetDeviceProcAddr;
    const auto get_properties = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        get_proc(vk, "vkGetPipelineExecutablePropertiesKHR"));
    const auto get_statistics = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        get_proc(vk, "vkGetPipelineExecutableStatisticsKHR"));
    if (get_properties && get_statistics) {
      VkPipelineInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
      info.pipeline = pipeline;
      uint32_t count = 0;
      VkResult result = get_properties(vk, &info, &count, nullptr);
      if (result == VK_SUCCESS && count && count <= 16) {
        std::vector<VkPipelineExecutablePropertiesKHR> properties(count);
        for (auto& property : properties) property.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
        result = get_properties(vk, &info, &count, properties.data());
        if (result == VK_SUCCESS) {
          for (uint32_t index = 0; index < count; ++index) {
            VkPipelineExecutableInfoKHR executable{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
            executable.pipeline = pipeline;
            executable.executableIndex = index;
            uint32_t statistic_count = 0;
            result = get_statistics(vk, &executable, &statistic_count, nullptr);
            if (result != VK_SUCCESS || !statistic_count || statistic_count > 64) {
              REXLOG_WARN("NATIVE_SHADER_STATS query failed result={} ps={:016X} executable={}",
                          int32_t(result), stats_ps, index);
              continue;
            }
            std::vector<VkPipelineExecutableStatisticKHR> statistics(statistic_count);
            for (auto& statistic : statistics) statistic.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
            result = get_statistics(vk, &executable, &statistic_count, statistics.data());
            if (result != VK_SUCCESS) continue;
            for (uint32_t stat_index = 0; stat_index < statistic_count; ++stat_index) {
              const auto& statistic = statistics[stat_index];
              // NVK exposes these counters as uint64. Preserve format for other drivers.
              if (statistic.format != VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) continue;
              REXLOG_INFO("NATIVE_SHADER_STATS vs={:016X} vs_mod={:016X} ps={:016X} ps_mod={:016X} placeholder={} executable={} stages=0x{:X} subgroup={} name=\"{}\" value={}",
                          vs_hash, vs_mod,
                          stats_ps, ps_mod, 0,
                          index, uint32_t(properties[index].stages), properties[index].subgroupSize,
                          statistic.name, statistic.value.u64);
            }
          }
        }
      } else {
        REXLOG_WARN("NATIVE_SHADER_STATS properties failed result={} ps={:016X} count={}", int32_t(result), stats_ps, count);
      }
    } else {
      REXLOG_WARN("NATIVE_SHADER_STATS entrypoints unavailable ps={:016X}", stats_ps);
    }
}
}  // namespace p2::native
