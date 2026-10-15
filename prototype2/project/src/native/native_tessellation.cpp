// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mercenaries 2 port: tessellation for the native renderer (project8-sw's renderer has none,
// and the game draws its water tessellated: HANDOFF section 52). The helper shaders and the
// rules follow the SDK's Vulkan backend (VulkanPipelineCache, PrimitiveProcessor; ReXGlue,
// BSD-3-Clause): a vertex shader passes each index (or adaptive edge factor) on, a control
// shader sets the tessellation levels and the control point indices, and the guest vertex
// shader, translated as a domain shader, is the evaluation stage.
#include "native_tessellation.h"

#include <cstddef>
#include <mutex>

#include <fmt/format.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/logging.h>

namespace p2::native::tessellation {
namespace {

using rex::graphics::SpirvShaderTranslator;
namespace xenos = rex::graphics::xenos;

// glslang default built-in resource limits (copied from the SDK's command_processor.cpp).
constexpr TBuiltInResource kDefaultResources = {
    /* .maxLights = */ 32,
    /* .maxClipPlanes = */ 6,
    /* .maxTextureUnits = */ 32,
    /* .maxTextureCoords = */ 32,
    /* .maxVertexAttribs = */ 64,
    /* .maxVertexUniformComponents = */ 4096,
    /* .maxVaryingFloats = */ 64,
    /* .maxVertexTextureImageUnits = */ 32,
    /* .maxCombinedTextureImageUnits = */ 80,
    /* .maxTextureImageUnits = */ 32,
    /* .maxFragmentUniformComponents = */ 4096,
    /* .maxDrawBuffers = */ 32,
    /* .maxVertexUniformVectors = */ 128,
    /* .maxVaryingVectors = */ 8,
    /* .maxFragmentUniformVectors = */ 16,
    /* .maxVertexOutputVectors = */ 16,
    /* .maxFragmentInputVectors = */ 15,
    /* .minProgramTexelOffset = */ -8,
    /* .maxProgramTexelOffset = */ 7,
    /* .maxClipDistances = */ 8,
    /* .maxComputeWorkGroupCountX = */ 65535,
    /* .maxComputeWorkGroupCountY = */ 65535,
    /* .maxComputeWorkGroupCountZ = */ 65535,
    /* .maxComputeWorkGroupSizeX = */ 1024,
    /* .maxComputeWorkGroupSizeY = */ 1024,
    /* .maxComputeWorkGroupSizeZ = */ 64,
    /* .maxComputeUniformComponents = */ 1024,
    /* .maxComputeTextureImageUnits = */ 16,
    /* .maxComputeImageUniforms = */ 8,
    /* .maxComputeAtomicCounters = */ 8,
    /* .maxComputeAtomicCounterBuffers = */ 1,
    /* .maxVaryingComponents = */ 60,
    /* .maxVertexOutputComponents = */ 64,
    /* .maxGeometryInputComponents = */ 64,
    /* .maxGeometryOutputComponents = */ 128,
    /* .maxFragmentInputComponents = */ 128,
    /* .maxImageUnits = */ 8,
    /* .maxCombinedImageUnitsAndFragmentOutputs = */ 8,
    /* .maxCombinedShaderOutputResources = */ 8,
    /* .maxImageSamples = */ 0,
    /* .maxVertexImageUniforms = */ 0,
    /* .maxTessControlImageUniforms = */ 0,
    /* .maxTessEvaluationImageUniforms = */ 0,
    /* .maxGeometryImageUniforms = */ 0,
    /* .maxFragmentImageUniforms = */ 8,
    /* .maxCombinedImageUniforms = */ 8,
    /* .maxGeometryTextureImageUnits = */ 16,
    /* .maxGeometryOutputVertices = */ 256,
    /* .maxGeometryTotalOutputComponents = */ 1024,
    /* .maxGeometryUniformComponents = */ 1024,
    /* .maxGeometryVaryingComponents = */ 64,
    /* .maxTessControlInputComponents = */ 128,
    /* .maxTessControlOutputComponents = */ 128,
    /* .maxTessControlTextureImageUnits = */ 16,
    /* .maxTessControlUniformComponents = */ 1024,
    /* .maxTessControlTotalOutputComponents = */ 4096,
    /* .maxTessEvaluationInputComponents = */ 128,
    /* .maxTessEvaluationOutputComponents = */ 128,
    /* .maxTessEvaluationTextureImageUnits = */ 16,
    /* .maxTessEvaluationUniformComponents = */ 1024,
    /* .maxTessPatchComponents = */ 120,
    /* .maxPatchVertices = */ 32,
    /* .maxTessGenLevel = */ 64,
    /* .maxViewports = */ 16,
    /* .maxVertexAtomicCounters = */ 0,
    /* .maxTessControlAtomicCounters = */ 0,
    /* .maxTessEvaluationAtomicCounters = */ 0,
    /* .maxGeometryAtomicCounters = */ 0,
    /* .maxFragmentAtomicCounters = */ 8,
    /* .maxCombinedAtomicCounters = */ 8,
    /* .maxAtomicCounterBindings = */ 1,
    /* .maxVertexAtomicCounterBuffers = */ 0,
    /* .maxTessControlAtomicCounterBuffers = */ 0,
    /* .maxTessEvaluationAtomicCounterBuffers = */ 0,
    /* .maxGeometryAtomicCounterBuffers = */ 0,
    /* .maxFragmentAtomicCounterBuffers = */ 1,
    /* .maxCombinedAtomicCounterBuffers = */ 1,
    /* .maxAtomicCounterBufferSize = */ 16384,
    /* .maxTransformFeedbackBuffers = */ 4,
    /* .maxTransformFeedbackInterleavedComponents = */ 64,
    /* .maxCullDistances = */ 8,
    /* .maxCombinedClipAndCullDistances = */ 8,
    /* .maxSamples = */ 4,
    /* .maxMeshOutputVerticesNV = */ 256,
    /* .maxMeshOutputPrimitivesNV = */ 512,
    /* .maxMeshWorkGroupSizeX_NV = */ 32,
    /* .maxMeshWorkGroupSizeY_NV = */ 1,
    /* .maxMeshWorkGroupSizeZ_NV = */ 1,
    /* .maxTaskWorkGroupSizeX_NV = */ 32,
    /* .maxTaskWorkGroupSizeY_NV = */ 1,
    /* .maxTaskWorkGroupSizeZ_NV = */ 1,
    /* .maxMeshViewCountNV = */ 4,
    /* .maxDualSourceDrawBuffersEXT = */ 1,
    /* .limits = */
    {
        /* .nonInductiveForLoops = */ 1,
        /* .whileLoops = */ 1,
        /* .doWhileLoops = */ 1,
        /* .generalUniformIndexing = */ 1,
        /* .generalAttributeMatrixVectorIndexing = */ 1,
        /* .generalVaryingIndexing = */ 1,
        /* .generalSamplerIndexing = */ 1,
        /* .generalVariableIndexing = */ 1,
        /* .generalConstantMatrixVectorIndexing = */ 1,
    },
};

// Same as VulkanPipelineCache's (anonymous) helpers, from the SDK (BSD-3-Clause). One fix:
// the system constants are in the translator's constants set and binding
// (kDescriptorSetConstants, kConstantBufferSystem), not set 0 binding 0, which in this
// renderer, as in the SDK, is the shared memory storage buffer.
std::string SystemConstantsBlock() {
  std::string source;
  source += fmt::format(
      "layout(set = {}, binding = {}, std140) uniform XeSystemConstants {{\n",
      uint32_t(SpirvShaderTranslator::kDescriptorSetConstants),
      uint32_t(SpirvShaderTranslator::kConstantBufferSystem));
  source += fmt::format("  layout(offset = {}) uint xe_vertex_index_endian;\n",
                        offsetof(SpirvShaderTranslator::SystemConstants, vertex_index_endian));
  source += fmt::format("  layout(offset = {}) int xe_vertex_base_index;\n",
                        offsetof(SpirvShaderTranslator::SystemConstants, vertex_base_index));
  source += fmt::format("  layout(offset = {}) uint xe_vertex_index_min;\n",
                        offsetof(SpirvShaderTranslator::SystemConstants, vertex_index_min));
  source += fmt::format("  layout(offset = {}) uint xe_vertex_index_max;\n",
                        offsetof(SpirvShaderTranslator::SystemConstants, vertex_index_max));
  source += fmt::format(
      "  layout(offset = {}) float xe_tessellation_factor_range_min;\n",
      offsetof(SpirvShaderTranslator::SystemConstants, tessellation_factor_range_min));
  source += fmt::format(
      "  layout(offset = {}) float xe_tessellation_factor_range_max;\n",
      offsetof(SpirvShaderTranslator::SystemConstants, tessellation_factor_range_max));
  source += "} xe_system_cbuffer;\n";
  return source;
}

constexpr const char* kSwapFunctions = R"(
uint xe_swap_8_in_16(uint value) {
  return ((value << 8u) & 0xFF00FF00u) | ((value >> 8u) & 0x00FF00FFu);
}
uint xe_swap_16_in_32(uint value) {
  return ((value & 0x0000FFFFu) << 16u) | (value >> 16u);
}
)";

std::string VertexShaderGlsl(bool adaptive) {
  std::string source = "#version 450\n" + SystemConstantsBlock();
  source += "layout(location = 0) out float xe_out_value;\n";
  source += kSwapFunctions;
  source += R"(
void main() {
  uint value = uint(gl_VertexIndex);
  uint endian = xe_system_cbuffer.xe_vertex_index_endian;
  if (endian == 1u || endian == 2u || endian == 3u) {
    value = xe_swap_8_in_16(value);
  }
  if (endian == 2u || endian == 3u) {
    value = xe_swap_16_in_32(value);
  }
)";
  if (adaptive) {
    source += R"(
  float tessellation_factor = float(value) + 1.0;
  tessellation_factor = max(tessellation_factor, xe_system_cbuffer.xe_tessellation_factor_range_min);
  tessellation_factor = min(tessellation_factor, xe_system_cbuffer.xe_tessellation_factor_range_max);
  xe_out_value = tessellation_factor;
}
)";
  } else {
    source += R"(
  value = uint(int(value) + xe_system_cbuffer.xe_vertex_base_index);
  value &= 0x00FFFFFFu;
  value = max(value, xe_system_cbuffer.xe_vertex_index_min);
  value = min(value, xe_system_cbuffer.xe_vertex_index_max);
  xe_out_value = float(value);
}
)";
  }
  return source;
}

bool IsTriangleDomain(HostType t) {
  return t == HostType::kTriangleDomainCPIndexed || t == HostType::kTriangleDomainPatchIndexed;
}
bool IsPatchIndexed(HostType t) {
  return t == HostType::kTriangleDomainPatchIndexed || t == HostType::kQuadDomainPatchIndexed;
}

std::string ControlShaderGlsl(HostType type, xenos::TessellationMode mode, uint32_t points) {
  std::string source = "#version 450\n" + SystemConstantsBlock();
  source += "layout(location = 0) in float xe_input_value[];\n";
  source += "layout(location = 0) patch out vec4 xe_out_patch_control_point_indices;\n";
  source += fmt::format("layout(vertices = {}) out;\n", points);
  source += R"(
void main() {
  gl_out[gl_InvocationID].gl_Position = vec4(0.0);
  if (gl_InvocationID != 0) {
    return;
  }
  vec4 patch_control_point_indices = vec4(0.0);
)";
  if (mode == xenos::TessellationMode::kAdaptive) {
    source += R"(
  uint patch_index = uint(int(gl_PrimitiveID) + xe_system_cbuffer.xe_vertex_base_index);
  patch_index &= 0x00FFFFFFu;
  patch_index = max(patch_index, xe_system_cbuffer.xe_vertex_index_min);
  patch_index = min(patch_index, xe_system_cbuffer.xe_vertex_index_max);
  patch_control_point_indices.x = float(patch_index);
)";
    if (IsTriangleDomain(type)) {
      source += R"(
  gl_TessLevelOuter[0] = xe_input_value[1];
  gl_TessLevelOuter[1] = xe_input_value[2];
  gl_TessLevelOuter[2] = xe_input_value[0];
  gl_TessLevelInner[0] = min(min(xe_input_value[2], xe_input_value[1]), xe_input_value[0]);
)";
    } else {
      source += R"(
  gl_TessLevelOuter[0] = xe_input_value[3];
  gl_TessLevelOuter[1] = xe_input_value[0];
  gl_TessLevelOuter[2] = xe_input_value[1];
  gl_TessLevelOuter[3] = xe_input_value[2];
  gl_TessLevelInner[0] = min(xe_input_value[2], xe_input_value[0]);
  gl_TessLevelInner[1] = min(xe_input_value[1], xe_input_value[3]);
)";
    }
  } else {
    source += "  patch_control_point_indices.x = xe_input_value[0];\n";
    if (!IsPatchIndexed(type)) {
      if (points >= 2) source += "  patch_control_point_indices.y = xe_input_value[1];\n";
      if (points >= 3) source += "  patch_control_point_indices.z = xe_input_value[2];\n";
      if (points >= 4) source += "  patch_control_point_indices.w = xe_input_value[3];\n";
    }
    source += "  float tessellation_factor = xe_system_cbuffer.xe_tessellation_factor_range_max;\n";
    if (IsTriangleDomain(type)) {
      source += R"(
  gl_TessLevelOuter[0] = tessellation_factor;
  gl_TessLevelOuter[1] = tessellation_factor;
  gl_TessLevelOuter[2] = tessellation_factor;
  gl_TessLevelInner[0] = tessellation_factor;
)";
    } else {
      source += R"(
  gl_TessLevelOuter[0] = tessellation_factor;
  gl_TessLevelOuter[1] = tessellation_factor;
  gl_TessLevelOuter[2] = tessellation_factor;
  gl_TessLevelOuter[3] = tessellation_factor;
  gl_TessLevelInner[0] = tessellation_factor;
  gl_TessLevelInner[1] = tessellation_factor;
)";
    }
  }
  source += R"(
  xe_out_patch_control_point_indices = patch_control_point_indices;
}
)";
  return source;
}

bool Compile(EShLanguage stage, const std::string& source, std::vector<uint32_t>& spirv) {
  static std::once_flag initialize_once;
  std::call_once(initialize_once, [] { glslang::InitializeProcess(); });
  const char* text = source.c_str();
  glslang::TShader shader(stage);
  shader.setStrings(&text, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
  EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&kDefaultResources, 450, false, messages)) {
    REXLOG_ERROR("[native] Tessellation shader parse failed: {}", shader.getInfoLog());
    return false;
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    REXLOG_ERROR("[native] Tessellation shader link failed: {}", program.getInfoLog());
    return false;
  }
  glslang::SpvOptions options = {};
  options.disableOptimizer = true;
  glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &options);
  return !spirv.empty();
}

}  // namespace

bool GetHostVertexShaderType(xenos::PrimitiveType primitive, xenos::TessellationMode mode,
                             HostType& type_out) {
  switch (primitive) {
    case xenos::PrimitiveType::kTriangleList:
      if (mode == xenos::TessellationMode::kAdaptive) return false;
      type_out = HostType::kTriangleDomainCPIndexed;
      return true;
    case xenos::PrimitiveType::kQuadList:
      if (mode == xenos::TessellationMode::kAdaptive) return false;
      type_out = HostType::kQuadDomainCPIndexed;
      return true;
    case xenos::PrimitiveType::kTrianglePatch:
      type_out = HostType::kTriangleDomainPatchIndexed;
      return true;
    case xenos::PrimitiveType::kQuadPatch:
      type_out = HostType::kQuadDomainPatchIndexed;
      return true;
    default:
      return false;
  }
}

uint32_t PatchControlPoints(HostType type, xenos::TessellationMode mode) {
  if (mode == xenos::TessellationMode::kAdaptive) {
    if (type == HostType::kTriangleDomainPatchIndexed) return 3;
    if (type == HostType::kQuadDomainPatchIndexed) return 4;
    return 0;
  }
  switch (type) {
    case HostType::kTriangleDomainCPIndexed:
      return 3;
    case HostType::kQuadDomainCPIndexed:
      return 4;
    case HostType::kTriangleDomainPatchIndexed:
    case HostType::kQuadDomainPatchIndexed:
      return 1;
    default:
      return 0;
  }
}

bool CompileVertexShader(bool adaptive, std::vector<uint32_t>& spirv) {
  return Compile(EShLangVertex, VertexShaderGlsl(adaptive), spirv);
}

bool CompileFragmentShader(const char* source, std::vector<uint32_t>& spirv) {
  return Compile(EShLangFragment, source, spirv);
}

bool CompileControlShader(HostType type, xenos::TessellationMode mode,
                          std::vector<uint32_t>& spirv) {
  uint32_t points = PatchControlPoints(type, mode);
  if (!points) return false;
  return Compile(EShLangTessControl, ControlShaderGlsl(type, mode, points), spirv);
}

}  // namespace p2::native::tessellation
