// SPDX-License-Identifier: GPL-3.0-or-later
//
// Geometry shaders that expand the Xbox 360 primitive types Vulkan lacks
// (point sprites, rectangle lists, quad lists) into triangle strips.
//
// Copied from ReXGlue's src/graphics/vulkan/pipeline_cache.cpp
// (VulkanPipelineCache::GetGeometryShader), itself from Xenia:
//   Copyright 2022 Ben Vanik. All rights reserved.
//   Released under the BSD license - see LICENSES/ for the full text.
// Changes: a free function that returns the module instead of caching it in the
// pipeline cache; logging through REXLOG.

#include "native_geometry_shaders.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/graphics/pipeline/shader/spirv_builder.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/util.h>

namespace p2::native {

using namespace rex;
using namespace rex::graphics;

VkShaderModule CreateGeometryShader(const rex::ui::vulkan::VulkanDevice* vulkan_device,
                                    GeometryShaderKey key) {

  std::vector<spv::Id> id_vector_temp;
  std::vector<unsigned int> uint_vector_temp;

  spv::ExecutionMode input_primitive_execution_mode = spv::ExecutionMode(0);
  uint32_t input_primitive_vertex_count = 0;
  spv::ExecutionMode output_primitive_execution_mode = spv::ExecutionMode(0);
  uint32_t output_max_vertices = 0;
  switch (key.type) {
    case PipelineGeometryShader::kPointList:
      // Point to a strip of 2 triangles.
      input_primitive_execution_mode = spv::ExecutionModeInputPoints;
      input_primitive_vertex_count = 1;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    case PipelineGeometryShader::kRectangleList:
      // Triangle to a strip of 2 triangles.
      input_primitive_execution_mode = spv::ExecutionModeTriangles;
      input_primitive_vertex_count = 3;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    case PipelineGeometryShader::kQuadList:
      // 4 vertices passed via a line list with adjacency to a strip of 2
      // triangles.
      input_primitive_execution_mode = spv::ExecutionModeInputLinesAdjacency;
      input_primitive_vertex_count = 4;
      output_primitive_execution_mode = spv::ExecutionModeOutputTriangleStrip;
      output_max_vertices = 4;
      break;
    default:
      assert_unhandled_case(key.type);
  }

  uint32_t clip_distance_count = key.user_clip_plane_cull ? 0 : key.user_clip_plane_count;
  uint32_t point_user_cull_distance_count =
      key.type == PipelineGeometryShader::kPointList && key.user_clip_plane_cull
          ? key.user_clip_plane_count
          : 0;
  uint32_t cull_distance_count = point_user_cull_distance_count + key.has_vertex_kill_and;
  bool point_recalculate_clip_distances = key.type == PipelineGeometryShader::kPointList &&
                                          clip_distance_count && key.point_ps_ucp_mode >= 2;
  bool point_recalculate_cull_distances = key.type == PipelineGeometryShader::kPointList &&
                                          point_user_cull_distance_count &&
                                          key.point_ps_ucp_mode >= 3;

  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();
  const ui::vulkan::VulkanDevice::Extensions& device_extensions = vulkan_device->extensions();
  spv::SpvBuildLogger builder_logger;
  spv::SpvBuildLogger* builder_logger_ptr = nullptr;
  spv::SpvVersion spirv_version;
  if (device_properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
    spirv_version = spv::Spv_1_5;
  } else if (device_extensions.ext_1_2_KHR_spirv_1_4) {
    spirv_version = spv::Spv_1_4;
  } else if (device_properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    spirv_version = spv::Spv_1_3;
  } else {
    spirv_version = spv::Spv_1_0;
    // Keep the build log around for compatibility diagnostics on older paths.
    builder_logger_ptr = &builder_logger;
  }
  SpirvBuilder builder(spirv_version, (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1,
                       builder_logger_ptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityGeometry);
  if (clip_distance_count) {
    builder.addCapability(spv::CapabilityClipDistance);
  }
  if (cull_distance_count) {
    builder.addCapability(spv::CapabilityCullDistance);
  }
  bool denorm_flush_to_zero_float32 = device_properties.shaderDenormFlushToZeroFloat32;
  bool signed_zero_inf_nan_preserve_float32 =
      device_properties.shaderSignedZeroInfNanPreserveFloat32;
  bool rounding_mode_rte_float32 = device_properties.shaderRoundingModeRTEFloat32;
  if (spirv_version < spv::Spv_1_4) {
    if (denorm_flush_to_zero_float32 || signed_zero_inf_nan_preserve_float32 ||
        rounding_mode_rte_float32) {
      builder.addExtension("SPV_KHR_float_controls");
    }
  }
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  std::vector<spv::Id> main_interface;

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_bool4 = builder.makeVectorType(type_bool, 4);
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_float2 = builder.makeVectorType(type_float, 2);
  spv::Id type_float3 = builder.makeVectorType(type_float, 3);
  spv::Id type_float4 = builder.makeVectorType(type_float, 4);
  spv::Id type_float4_array_6 =
      builder.makeArrayType(type_float4, builder.makeUintConstant(6), sizeof(float) * 4);
  builder.addDecoration(type_float4_array_6, spv::DecorationArrayStride, sizeof(float) * 4);
  spv::Id type_clip_distances =
      clip_distance_count
          ? builder.makeArrayType(type_float, builder.makeUintConstant(clip_distance_count), 0)
          : spv::NoType;
  spv::Id type_cull_distances =
      cull_distance_count
          ? builder.makeArrayType(type_float, builder.makeUintConstant(cull_distance_count), 0)
          : spv::NoType;

  // System constants.
  // For points:
  // - float2 point_constant_diameter
  // - float2 point_screen_diameter_to_ndc_radius
  // - float3 ndc_scale
  // - float3 ndc_offset
  // - float4[6] user_clip_planes
  enum PointConstant : uint32_t {
    kPointConstantConstantDiameter,
    kPointConstantScreenDiameterToNdcRadius,
    kPointConstantNdcScale,
    kPointConstantNdcOffset,
    kPointConstantUserClipPlanes,
    kPointConstantCount,
  };
  spv::Id type_system_constants = spv::NoType;
  if (key.type == PipelineGeometryShader::kPointList) {
    id_vector_temp.clear();
    id_vector_temp.resize(kPointConstantCount);
    id_vector_temp[kPointConstantConstantDiameter] = type_float2;
    id_vector_temp[kPointConstantScreenDiameterToNdcRadius] = type_float2;
    id_vector_temp[kPointConstantNdcScale] = type_float3;
    id_vector_temp[kPointConstantNdcOffset] = type_float3;
    id_vector_temp[kPointConstantUserClipPlanes] = type_float4_array_6;
    type_system_constants = builder.makeStructType(id_vector_temp, "XeSystemConstants");
    builder.addMemberName(type_system_constants, kPointConstantConstantDiameter,
                          "point_constant_diameter");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantConstantDiameter, spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants, point_constant_diameter)));
    builder.addMemberName(type_system_constants, kPointConstantScreenDiameterToNdcRadius,
                          "point_screen_diameter_to_ndc_radius");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantScreenDiameterToNdcRadius, spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants, point_screen_diameter_to_ndc_radius)));
    builder.addMemberName(type_system_constants, kPointConstantNdcScale, "ndc_scale");
    builder.addMemberDecoration(type_system_constants, kPointConstantNdcScale,
                                spv::DecorationOffset,
                                int(offsetof(SpirvShaderTranslator::SystemConstants, ndc_scale)));
    builder.addMemberName(type_system_constants, kPointConstantNdcOffset, "ndc_offset");
    builder.addMemberDecoration(type_system_constants, kPointConstantNdcOffset,
                                spv::DecorationOffset,
                                int(offsetof(SpirvShaderTranslator::SystemConstants, ndc_offset)));
    builder.addMemberName(type_system_constants, kPointConstantUserClipPlanes, "user_clip_planes");
    builder.addMemberDecoration(
        type_system_constants, kPointConstantUserClipPlanes, spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants, user_clip_planes)));
  }
  spv::Id uniform_system_constants = spv::NoResult;
  if (type_system_constants != spv::NoType) {
    builder.addDecoration(type_system_constants, spv::DecorationBlock);
    uniform_system_constants =
        builder.createVariable(spv::NoPrecision, spv::StorageClassUniform, type_system_constants,
                               "xe_uniform_system_constants");
    builder.addDecoration(uniform_system_constants, spv::DecorationDescriptorSet,
                          int(SpirvShaderTranslator::kDescriptorSetConstants));
    builder.addDecoration(uniform_system_constants, spv::DecorationBinding,
                          int(SpirvShaderTranslator::kConstantBufferSystem));
    main_interface.push_back(uniform_system_constants);
  }

  // Inputs and outputs - matching glslang order, in gl_PerVertex gl_in[],
  // user-defined outputs, user-defined inputs, out gl_PerVertex.

  spv::Id const_input_primitive_vertex_count =
      builder.makeUintConstant(input_primitive_vertex_count);

  // in gl_PerVertex gl_in[].
  // gl_Position.
  id_vector_temp.clear();
  uint32_t member_in_gl_per_vertex_position = uint32_t(id_vector_temp.size());
  id_vector_temp.push_back(type_float4);
  spv::Id const_member_in_gl_per_vertex_position =
      builder.makeIntConstant(int32_t(member_in_gl_per_vertex_position));
  // gl_ClipDistance.
  uint32_t member_in_gl_per_vertex_clip_distance = UINT32_MAX;
  spv::Id const_member_in_gl_per_vertex_clip_distance = spv::NoResult;
  if (clip_distance_count) {
    member_in_gl_per_vertex_clip_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_clip_distances);
    const_member_in_gl_per_vertex_clip_distance =
        builder.makeIntConstant(int32_t(member_in_gl_per_vertex_clip_distance));
  }
  // gl_CullDistance.
  uint32_t member_in_gl_per_vertex_cull_distance = UINT32_MAX;
  if (cull_distance_count) {
    member_in_gl_per_vertex_cull_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_cull_distances);
  }
  // Structure and array.
  spv::Id type_struct_in_gl_per_vertex = builder.makeStructType(id_vector_temp, "gl_PerVertex");
  builder.addMemberName(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_position,
                        "gl_Position");
  builder.addMemberDecoration(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_position,
                              spv::DecorationBuiltIn, spv::BuiltInPosition);
  if (clip_distance_count) {
    builder.addMemberName(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_clip_distance,
                          "gl_ClipDistance");
    builder.addMemberDecoration(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_clip_distance,
                                spv::DecorationBuiltIn, spv::BuiltInClipDistance);
  }
  if (cull_distance_count) {
    builder.addMemberName(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_cull_distance,
                          "gl_CullDistance");
    builder.addMemberDecoration(type_struct_in_gl_per_vertex, member_in_gl_per_vertex_cull_distance,
                                spv::DecorationBuiltIn, spv::BuiltInCullDistance);
  }
  builder.addDecoration(type_struct_in_gl_per_vertex, spv::DecorationBlock);
  spv::Id type_array_in_gl_per_vertex =
      builder.makeArrayType(type_struct_in_gl_per_vertex, const_input_primitive_vertex_count, 0);
  spv::Id in_gl_per_vertex = builder.createVariable(spv::NoPrecision, spv::StorageClassInput,
                                                    type_array_in_gl_per_vertex, "gl_in");
  main_interface.push_back(in_gl_per_vertex);

  uint32_t output_location = 0;

  // Interpolators outputs.
  std::array<spv::Id, xenos::kMaxInterpolators> out_interpolators;
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    spv::Id out_interpolator =
        builder.createVariable(spv::NoPrecision, spv::StorageClassOutput, type_float4,
                               fmt::format("xe_out_interpolator_{}", i).c_str());
    out_interpolators[i] = out_interpolator;
    builder.addDecoration(out_interpolator, spv::DecorationLocation, int(output_location));
    builder.addDecoration(out_interpolator, spv::DecorationInvariant);
    main_interface.push_back(out_interpolator);
    ++output_location;
  }

  // Point coordinate output.
  spv::Id out_point_coordinates = spv::NoResult;
  if (key.has_point_coordinates) {
    out_point_coordinates = builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                   type_float2, "xe_out_point_coordinates");
    builder.addDecoration(out_point_coordinates, spv::DecorationLocation, int(output_location));
    builder.addDecoration(out_point_coordinates, spv::DecorationInvariant);
    main_interface.push_back(out_point_coordinates);
    ++output_location;
  }

  uint32_t input_location = 0;

  // Interpolator inputs.
  std::array<spv::Id, xenos::kMaxInterpolators> in_interpolators;
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    spv::Id in_interpolator = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder.makeArrayType(type_float4, const_input_primitive_vertex_count, 0),
        fmt::format("xe_in_interpolator_{}", i).c_str());
    in_interpolators[i] = in_interpolator;
    builder.addDecoration(in_interpolator, spv::DecorationLocation, int(input_location));
    main_interface.push_back(in_interpolator);
    ++input_location;
  }

  // Point size input.
  spv::Id in_point_size = spv::NoResult;
  if (key.has_point_size) {
    in_point_size = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder.makeArrayType(type_float, const_input_primitive_vertex_count, 0),
        "xe_in_point_size");
    builder.addDecoration(in_point_size, spv::DecorationLocation, int(input_location));
    main_interface.push_back(in_point_size);
    ++input_location;
  }

  // out gl_PerVertex.
  // gl_Position.
  id_vector_temp.clear();
  uint32_t member_out_gl_per_vertex_position = uint32_t(id_vector_temp.size());
  id_vector_temp.push_back(type_float4);
  spv::Id const_member_out_gl_per_vertex_position =
      builder.makeIntConstant(int32_t(member_out_gl_per_vertex_position));
  // gl_ClipDistance.
  uint32_t member_out_gl_per_vertex_clip_distance = UINT32_MAX;
  spv::Id const_member_out_gl_per_vertex_clip_distance = spv::NoResult;
  if (clip_distance_count) {
    member_out_gl_per_vertex_clip_distance = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_clip_distances);
    const_member_out_gl_per_vertex_clip_distance =
        builder.makeIntConstant(int32_t(member_out_gl_per_vertex_clip_distance));
  }
  // Structure.
  spv::Id type_struct_out_gl_per_vertex = builder.makeStructType(id_vector_temp, "gl_PerVertex");
  builder.addMemberName(type_struct_out_gl_per_vertex, member_out_gl_per_vertex_position,
                        "gl_Position");
  builder.addMemberDecoration(type_struct_out_gl_per_vertex, member_out_gl_per_vertex_position,
                              spv::DecorationBuiltIn, spv::BuiltInPosition);
  if (clip_distance_count) {
    builder.addMemberName(type_struct_out_gl_per_vertex, member_out_gl_per_vertex_clip_distance,
                          "gl_ClipDistance");
    builder.addMemberDecoration(type_struct_out_gl_per_vertex,
                                member_out_gl_per_vertex_clip_distance, spv::DecorationBuiltIn,
                                spv::BuiltInClipDistance);
  }
  builder.addDecoration(type_struct_out_gl_per_vertex, spv::DecorationBlock);
  spv::Id out_gl_per_vertex = builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                     type_struct_out_gl_per_vertex, "");
  builder.addDecoration(out_gl_per_vertex, spv::DecorationInvariant);
  main_interface.push_back(out_gl_per_vertex);

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function = builder.makeFunctionEntry(
      spv::NoPrecision, type_void, "main", main_param_types, main_precisions, &main_entry);
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelGeometry, main_function, "main");
  for (spv::Id interface_id : main_interface) {
    entry_point->addIdOperand(interface_id);
  }
  builder.addExecutionMode(main_function, input_primitive_execution_mode);
  builder.addExecutionMode(main_function, spv::ExecutionModeInvocations, 1);
  builder.addExecutionMode(main_function, output_primitive_execution_mode);
  builder.addExecutionMode(main_function, spv::ExecutionModeOutputVertices,
                           int(output_max_vertices));
  if (denorm_flush_to_zero_float32) {
    builder.addCapability(spv::CapabilityDenormFlushToZero);
    builder.addExecutionMode(main_function, spv::ExecutionModeDenormFlushToZero, 32);
  }
  if (signed_zero_inf_nan_preserve_float32) {
    builder.addCapability(spv::CapabilitySignedZeroInfNanPreserve);
    builder.addExecutionMode(main_function, spv::ExecutionModeSignedZeroInfNanPreserve, 32);
  }
  if (rounding_mode_rte_float32) {
    builder.addCapability(spv::CapabilityRoundingModeRTE);
    builder.addExecutionMode(main_function, spv::ExecutionModeRoundingModeRTE, 32);
  }

  // Note that after every OpEmitVertex, all output variables are undefined.

  // Discard the whole primitive if any vertex has a NaN position (may also be
  // set to NaN for emulation of vertex killing with the OR operator).
  for (uint32_t i = 0; i < input_primitive_vertex_count; ++i) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
    id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
    spv::Id position_is_nan = builder.createUnaryOp(
        spv::OpAny, type_bool,
        builder.createUnaryOp(
            spv::OpIsNan, type_bool4,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision)));
    spv::Block& discard_predecessor = *builder.getBuildPoint();
    spv::Block& discard_then_block = builder.makeNewBlock();
    spv::Block& discard_merge_block = builder.makeNewBlock();
    builder.createSelectionMerge(&discard_merge_block, spv::SelectionControlDontFlattenMask);
    {
      std::unique_ptr<spv::Instruction> branch_conditional_op(
          std::make_unique<spv::Instruction>(spv::OpBranchConditional));
      branch_conditional_op->addIdOperand(position_is_nan);
      branch_conditional_op->addIdOperand(discard_then_block.getId());
      branch_conditional_op->addIdOperand(discard_merge_block.getId());
      branch_conditional_op->addImmediateOperand(1);
      branch_conditional_op->addImmediateOperand(2);
      discard_predecessor.addInstruction(std::move(branch_conditional_op));
    }
    discard_then_block.addPredecessor(&discard_predecessor);
    discard_merge_block.addPredecessor(&discard_predecessor);
    builder.setBuildPoint(&discard_then_block);
    builder.createNoResultOp(spv::OpReturn);
    builder.setBuildPoint(&discard_merge_block);
  }

  // Cull the whole primitive if any cull distance for all vertices in the
  // primitive is < 0.
  // For point lists with ps_ucp_mode 3, user cull plane distances are
  // calculated per expanded vertex later.
  if (cull_distance_count) {
    spv::Id const_member_in_gl_per_vertex_cull_distance =
        builder.makeIntConstant(int32_t(member_in_gl_per_vertex_cull_distance));
    spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
    spv::Id cull_condition = spv::NoResult;
    uint32_t cull_distance_start =
        point_recalculate_cull_distances ? point_user_cull_distance_count : 0;
    if (cull_distance_start < cull_distance_count) {
      for (uint32_t i = cull_distance_start; i < cull_distance_count; ++i) {
        for (uint32_t j = 0; j < input_primitive_vertex_count; ++j) {
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeIntConstant(int32_t(j)));
          id_vector_temp.push_back(const_member_in_gl_per_vertex_cull_distance);
          id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
          spv::Id cull_distance_is_negative = builder.createBinOp(
              spv::OpFOrdLessThan, type_bool,
              builder.createLoad(builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex,
                                                           id_vector_temp),
                                 spv::NoPrecision),
              const_float_0);
          if (cull_condition != spv::NoResult) {
            cull_condition = builder.createBinOp(spv::OpLogicalAnd, type_bool, cull_condition,
                                                 cull_distance_is_negative);
          } else {
            cull_condition = cull_distance_is_negative;
          }
        }
      }
      assert_true(cull_condition != spv::NoResult);
      spv::Block& discard_predecessor = *builder.getBuildPoint();
      spv::Block& discard_then_block = builder.makeNewBlock();
      spv::Block& discard_merge_block = builder.makeNewBlock();
      builder.createSelectionMerge(&discard_merge_block, spv::SelectionControlDontFlattenMask);
      {
        std::unique_ptr<spv::Instruction> branch_conditional_op(
            std::make_unique<spv::Instruction>(spv::OpBranchConditional));
        branch_conditional_op->addIdOperand(cull_condition);
        branch_conditional_op->addIdOperand(discard_then_block.getId());
        branch_conditional_op->addIdOperand(discard_merge_block.getId());
        branch_conditional_op->addImmediateOperand(1);
        branch_conditional_op->addImmediateOperand(2);
        discard_predecessor.addInstruction(std::move(branch_conditional_op));
      }
      discard_then_block.addPredecessor(&discard_predecessor);
      discard_merge_block.addPredecessor(&discard_predecessor);
      builder.setBuildPoint(&discard_then_block);
      builder.createNoResultOp(spv::OpReturn);
      builder.setBuildPoint(&discard_merge_block);
    }
  }

  switch (key.type) {
    case PipelineGeometryShader::kPointList: {
      // Expand the point sprite, with left-to-right, top-to-bottom UVs.

      spv::Id const_int_0 = builder.makeIntConstant(0);
      spv::Id const_int_1 = builder.makeIntConstant(1);
      spv::Id const_float_0 = builder.makeFloatConstant(0.0f);

      // Load the point diameter in guest pixels.
      id_vector_temp.clear();
      id_vector_temp.push_back(builder.makeIntConstant(int32_t(kPointConstantConstantDiameter)));
      id_vector_temp.push_back(const_int_0);
      spv::Id point_guest_diameter_x =
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision);
      id_vector_temp.back() = const_int_1;
      spv::Id point_guest_diameter_y =
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision);
      if (key.has_point_size) {
        // The vertex shader's header writes -1.0 to point_size by default, so
        // any non-negative value means that it was overwritten by the
        // translated vertex shader, and needs to be used instead of the
        // constant size. The per-vertex diameter is already clamped in the
        // vertex shader (combined with making it non-negative).
        id_vector_temp.clear();
        // 0 is the input primitive vertex index.
        id_vector_temp.push_back(const_int_0);
        spv::Id point_vertex_diameter = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_point_size, id_vector_temp),
            spv::NoPrecision);
        spv::Id point_vertex_diameter_written = builder.createBinOp(
            spv::OpFOrdGreaterThanEqual, type_bool, point_vertex_diameter, const_float_0);
        point_guest_diameter_x =
            builder.createTriOp(spv::OpSelect, type_float, point_vertex_diameter_written,
                                point_vertex_diameter, point_guest_diameter_x);
        point_guest_diameter_y =
            builder.createTriOp(spv::OpSelect, type_float, point_vertex_diameter_written,
                                point_vertex_diameter, point_guest_diameter_y);
      }

      // 4D5307F1 has zero-size snowflakes, drop them quicker, and also drop
      // points with a constant size of zero since point lists may also be used
      // as just "compute" with memexport.
      spv::Id point_size_not_zero =
          builder.createBinOp(spv::OpLogicalAnd, type_bool,
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  point_guest_diameter_x, const_float_0),
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  point_guest_diameter_y, const_float_0));
      spv::Block& point_size_zero_predecessor = *builder.getBuildPoint();
      spv::Block& point_size_zero_then_block = builder.makeNewBlock();
      spv::Block& point_size_zero_merge_block = builder.makeNewBlock();
      builder.createSelectionMerge(&point_size_zero_merge_block,
                                   spv::SelectionControlDontFlattenMask);
      {
        std::unique_ptr<spv::Instruction> branch_conditional_op(
            std::make_unique<spv::Instruction>(spv::OpBranchConditional));
        branch_conditional_op->addIdOperand(point_size_not_zero);
        branch_conditional_op->addIdOperand(point_size_zero_merge_block.getId());
        branch_conditional_op->addIdOperand(point_size_zero_then_block.getId());
        branch_conditional_op->addImmediateOperand(2);
        branch_conditional_op->addImmediateOperand(1);
        point_size_zero_predecessor.addInstruction(std::move(branch_conditional_op));
      }
      point_size_zero_then_block.addPredecessor(&point_size_zero_predecessor);
      point_size_zero_merge_block.addPredecessor(&point_size_zero_predecessor);
      builder.setBuildPoint(&point_size_zero_then_block);
      builder.createNoResultOp(spv::OpReturn);
      builder.setBuildPoint(&point_size_zero_merge_block);

      // Transform the diameter in the guest screen coordinates to radius in the
      // normalized device coordinates, and then to the clip space by
      // multiplying by W.
      id_vector_temp.clear();
      id_vector_temp.push_back(
          builder.makeIntConstant(int32_t(kPointConstantScreenDiameterToNdcRadius)));
      id_vector_temp.push_back(const_int_0);
      spv::Id point_radius_x = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_guest_diameter_x,
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision));
      id_vector_temp.back() = const_int_1;
      spv::Id point_radius_y = builder.createNoContractionBinOp(
          spv::OpFMul, type_float, point_guest_diameter_y,
          builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                       uniform_system_constants, id_vector_temp),
                             spv::NoPrecision));
      id_vector_temp.clear();
      // 0 is the input primitive vertex index.
      id_vector_temp.push_back(const_int_0);
      id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
      spv::Id point_position = builder.createLoad(
          builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
          spv::NoPrecision);
      spv::Id point_w = builder.createCompositeExtract(point_position, type_float, 3);
      point_radius_x =
          builder.createNoContractionBinOp(spv::OpFMul, type_float, point_radius_x, point_w);
      point_radius_y =
          builder.createNoContractionBinOp(spv::OpFMul, type_float, point_radius_y, point_w);

      // Load the inputs for the guest point.
      // Interpolators.
      std::array<spv::Id, xenos::kMaxInterpolators> point_interpolators;
      id_vector_temp.clear();
      // 0 is the input primitive vertex index.
      id_vector_temp.push_back(const_int_0);
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        point_interpolators[i] = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_interpolators[i], id_vector_temp),
            spv::NoPrecision);
      }
      // Positions.
      spv::Id point_x = builder.createCompositeExtract(point_position, type_float, 0);
      spv::Id point_y = builder.createCompositeExtract(point_position, type_float, 1);
      std::array<spv::Id, 2> point_edge_x, point_edge_y;
      for (uint32_t i = 0; i < 2; ++i) {
        spv::Op point_radius_add_op = i ? spv::OpFAdd : spv::OpFSub;
        point_edge_x[i] = builder.createNoContractionBinOp(point_radius_add_op, type_float, point_x,
                                                           point_radius_x);
        point_edge_y[i] = builder.createNoContractionBinOp(point_radius_add_op, type_float, point_y,
                                                           point_radius_y);
      };
      spv::Id point_z = builder.createCompositeExtract(point_position, type_float, 2);
      // Clip distances.
      spv::Id point_clip_distances = spv::NoResult;
      std::vector<spv::Id> point_user_clip_planes;
      point_user_clip_planes.reserve(clip_distance_count);
      spv::Id point_ndc_scale = spv::NoResult;
      spv::Id point_ndc_offset = spv::NoResult;
      if (point_recalculate_clip_distances || point_recalculate_cull_distances) {
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.makeIntConstant(int32_t(kPointConstantNdcScale)));
        point_ndc_scale =
            builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                         uniform_system_constants, id_vector_temp),
                               spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(builder.makeIntConstant(int32_t(kPointConstantNdcOffset)));
        point_ndc_offset =
            builder.createLoad(builder.createAccessChain(spv::StorageClassUniform,
                                                         uniform_system_constants, id_vector_temp),
                               spv::NoPrecision);
        uint32_t point_user_clip_plane_count =
            point_recalculate_cull_distances ? point_user_cull_distance_count : clip_distance_count;
        for (uint32_t i = 0; i < point_user_clip_plane_count; ++i) {
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeIntConstant(int32_t(kPointConstantUserClipPlanes)));
          id_vector_temp.push_back(builder.makeIntConstant(int32_t(i)));
          point_user_clip_planes.push_back(builder.createLoad(
              builder.createAccessChain(spv::StorageClassUniform, uniform_system_constants,
                                        id_vector_temp),
              spv::NoPrecision));
        }
      } else if (clip_distance_count) {
        id_vector_temp.clear();
        // 0 is the input primitive vertex index.
        id_vector_temp.push_back(const_int_0);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        point_clip_distances = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
      }

      if (point_recalculate_cull_distances) {
        // Cull if any user clip plane has all expanded vertices outside.
        std::array<spv::Id, 4> point_guest_positions;
        for (uint32_t i = 0; i < 4; ++i) {
          uint32_t point_vertex_x = i >> 1;
          uint32_t point_vertex_y = i & 1;
          id_vector_temp.clear();
          id_vector_temp.push_back(point_edge_x[point_vertex_x]);
          id_vector_temp.push_back(point_edge_y[point_vertex_y]);
          id_vector_temp.push_back(point_z);
          id_vector_temp.push_back(point_w);
          spv::Id point_vertex_position =
              builder.createCompositeConstruct(type_float4, id_vector_temp);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 0));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 1));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 2));
          spv::Id point_vertex_position_xyz =
              builder.createCompositeConstruct(type_float3, id_vector_temp);
          spv::Id point_guest_position_xyz = builder.createNoContractionBinOp(
              spv::OpFSub, type_float3, point_vertex_position_xyz,
              builder.createNoContractionBinOp(spv::OpVectorTimesScalar, type_float3,
                                               point_ndc_offset, point_w));
          point_guest_position_xyz = builder.createNoContractionBinOp(
              spv::OpFDiv, type_float3, point_guest_position_xyz, point_ndc_scale);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 0));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 1));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 2));
          id_vector_temp.push_back(point_w);
          point_guest_positions[i] = builder.createCompositeConstruct(type_float4, id_vector_temp);
        }
        spv::Id point_cull_condition = spv::NoResult;
        for (uint32_t i = 0; i < point_user_cull_distance_count; ++i) {
          spv::Id point_plane_cull_condition = spv::NoResult;
          for (uint32_t j = 0; j < 4; ++j) {
            spv::Id point_distance_is_negative = builder.createBinOp(
                spv::OpFOrdLessThan, type_bool,
                builder.createBinOp(spv::OpDot, type_float, point_guest_positions[j],
                                    point_user_clip_planes[i]),
                const_float_0);
            if (point_plane_cull_condition != spv::NoResult) {
              point_plane_cull_condition =
                  builder.createBinOp(spv::OpLogicalAnd, type_bool, point_plane_cull_condition,
                                      point_distance_is_negative);
            } else {
              point_plane_cull_condition = point_distance_is_negative;
            }
          }
          if (point_cull_condition != spv::NoResult) {
            point_cull_condition = builder.createBinOp(
                spv::OpLogicalOr, type_bool, point_cull_condition, point_plane_cull_condition);
          } else {
            point_cull_condition = point_plane_cull_condition;
          }
        }
        assert_true(point_cull_condition != spv::NoResult);
        spv::Block& point_cull_predecessor = *builder.getBuildPoint();
        spv::Block& point_cull_then_block = builder.makeNewBlock();
        spv::Block& point_cull_merge_block = builder.makeNewBlock();
        builder.createSelectionMerge(&point_cull_merge_block, spv::SelectionControlDontFlattenMask);
        {
          std::unique_ptr<spv::Instruction> branch_conditional_op(
              std::make_unique<spv::Instruction>(spv::OpBranchConditional));
          branch_conditional_op->addIdOperand(point_cull_condition);
          branch_conditional_op->addIdOperand(point_cull_then_block.getId());
          branch_conditional_op->addIdOperand(point_cull_merge_block.getId());
          branch_conditional_op->addImmediateOperand(1);
          branch_conditional_op->addImmediateOperand(2);
          point_cull_predecessor.addInstruction(std::move(branch_conditional_op));
        }
        point_cull_then_block.addPredecessor(&point_cull_predecessor);
        point_cull_merge_block.addPredecessor(&point_cull_predecessor);
        builder.setBuildPoint(&point_cull_then_block);
        builder.createNoResultOp(spv::OpReturn);
        builder.setBuildPoint(&point_cull_merge_block);
      }

      for (uint32_t i = 0; i < 4; ++i) {
        // Same interpolators for the entire sprite.
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          builder.createStore(point_interpolators[j], out_interpolators[j]);
        }
        // Top-left, bottom-left, top-right, bottom-right order (chosen
        // arbitrarily, simply based on counterclockwise meaning front with
        // frontFace = VkFrontFace(0), but faceness is ignored for non-polygon
        // primitive types).
        uint32_t point_vertex_x = i >> 1;
        uint32_t point_vertex_y = i & 1;
        // Point coordinates.
        if (key.has_point_coordinates) {
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeFloatConstant(float(point_vertex_x)));
          id_vector_temp.push_back(builder.makeFloatConstant(float(point_vertex_y)));
          builder.createStore(builder.makeCompositeConstant(type_float2, id_vector_temp),
                              out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(point_edge_x[point_vertex_x]);
        id_vector_temp.push_back(point_edge_y[point_vertex_y]);
        id_vector_temp.push_back(point_z);
        id_vector_temp.push_back(point_w);
        spv::Id point_vertex_position =
            builder.createCompositeConstruct(type_float4, id_vector_temp);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            point_vertex_position,
            builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (point_recalculate_clip_distances) {
          // Convert host clip space back to guest clip space before applying
          // user clip planes.
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 0));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 1));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_vertex_position, type_float, 2));
          spv::Id point_vertex_position_xyz =
              builder.createCompositeConstruct(type_float3, id_vector_temp);
          spv::Id point_guest_position_xyz = builder.createNoContractionBinOp(
              spv::OpFSub, type_float3, point_vertex_position_xyz,
              builder.createNoContractionBinOp(spv::OpVectorTimesScalar, type_float3,
                                               point_ndc_offset, point_w));
          point_guest_position_xyz = builder.createNoContractionBinOp(
              spv::OpFDiv, type_float3, point_guest_position_xyz, point_ndc_scale);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 0));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 1));
          id_vector_temp.push_back(
              builder.createCompositeExtract(point_guest_position_xyz, type_float, 2));
          id_vector_temp.push_back(point_w);
          spv::Id point_guest_position =
              builder.createCompositeConstruct(type_float4, id_vector_temp);
          for (uint32_t j = 0; j < clip_distance_count; ++j) {
            id_vector_temp.clear();
            id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
            id_vector_temp.push_back(builder.makeIntConstant(int32_t(j)));
            builder.createStore(builder.createBinOp(spv::OpDot, type_float, point_guest_position,
                                                    point_user_clip_planes[j]),
                                builder.createAccessChain(spv::StorageClassOutput,
                                                          out_gl_per_vertex, id_vector_temp));
          }
        } else if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(point_clip_distances,
                              builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex,
                                                        id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    case PipelineGeometryShader::kRectangleList: {
      // Construct a strip with the fourth vertex generated by mirroring a
      // vertex across the longest edge (the diagonal).
      //
      // Possible options:
      //
      // 0---1
      // |  /|
      // | / |  - 12 is the longest edge, strip 0123 (most commonly used)
      // |/  |    v3 = v0 + (v1 - v0) + (v2 - v0), or v3 = -v0 + v1 + v2
      // 2--[3]
      //
      // 1---2
      // |  /|
      // | / |  - 20 is the longest edge, strip 1203
      // |/  |
      // 0--[3]
      //
      // 2---0
      // |  /|
      // | / |  - 01 is the longest edge, strip 2013
      // |/  |
      // 1--[3]

      spv::Id const_int_0 = builder.makeIntConstant(0);
      spv::Id const_int_1 = builder.makeIntConstant(1);
      spv::Id const_int_2 = builder.makeIntConstant(2);
      spv::Id const_int_3 = builder.makeIntConstant(3);

      // Get squares of edge lengths to choose the longest edge.
      // [0] - 12, [1] - 20, [2] - 01.
      spv::Id edge_lengths[3];
      id_vector_temp.resize(3);
      id_vector_temp[1] = const_member_in_gl_per_vertex_position;
      for (uint32_t i = 0; i < 3; ++i) {
        id_vector_temp[0] = builder.makeIntConstant(int32_t((1 + i) % 3));
        id_vector_temp[2] = const_int_0;
        spv::Id edge_0_x = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[2] = const_int_1;
        spv::Id edge_0_y = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = builder.makeIntConstant(int32_t((2 + i) % 3));
        id_vector_temp[2] = const_int_0;
        spv::Id edge_1_x = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[2] = const_int_1;
        spv::Id edge_1_y = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        spv::Id edge_x = builder.createBinOp(spv::OpFSub, type_float, edge_1_x, edge_0_x);
        spv::Id edge_y = builder.createBinOp(spv::OpFSub, type_float, edge_1_y, edge_0_y);
        edge_lengths[i] = builder.createBinOp(
            spv::OpFAdd, type_float, builder.createBinOp(spv::OpFMul, type_float, edge_x, edge_x),
            builder.createBinOp(spv::OpFMul, type_float, edge_y, edge_y));
      }

      // Choose the index of the first vertex in the strip based on which edge
      // is the longest, and calculate the indices of the other vertices.
      spv::Id vertex_indices[3];
      // If 12 > 20 && 12 > 01, then 12 is the longest edge, and the strip is
      // 0123. Otherwise, if 20 > 01, then 20 is the longest, and the strip is
      // 1203, but if not, 01 is the longest, and the strip is 2013.
      vertex_indices[0] = builder.createTriOp(
          spv::OpSelect, type_int,
          builder.createBinOp(spv::OpLogicalAnd, type_bool,
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  edge_lengths[0], edge_lengths[1]),
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  edge_lengths[0], edge_lengths[2])),
          const_int_0,
          builder.createTriOp(spv::OpSelect, type_int,
                              builder.createBinOp(spv::OpFOrdGreaterThan, type_bool,
                                                  edge_lengths[1], edge_lengths[2]),
                              const_int_1, const_int_2));
      for (uint32_t i = 1; i < 3; ++i) {
        // vertex_indices[i] = (vertex_indices[0] + i) % 3
        spv::Id vertex_index_without_wrapping = builder.createBinOp(
            spv::OpIAdd, type_int, vertex_indices[0], builder.makeIntConstant(int32_t(i)));
        vertex_indices[i] = builder.createTriOp(
            spv::OpSelect, type_int,
            builder.createBinOp(spv::OpSLessThan, type_bool, vertex_index_without_wrapping,
                                const_int_3),
            vertex_index_without_wrapping,
            builder.createBinOp(spv::OpISub, type_int, vertex_index_without_wrapping, const_int_3));
      }

      // Initialize the point coordinates output for safety if this shader type
      // is used with has_point_coordinates for some reason.
      spv::Id const_point_coordinates_zero = spv::NoResult;
      if (key.has_point_coordinates) {
        spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_float_0);
        id_vector_temp.push_back(const_float_0);
        const_point_coordinates_zero = builder.makeCompositeConstant(type_float2, id_vector_temp);
      }

      // Emit the triangle in the strip that consists of the original vertices.
      for (uint32_t i = 0; i < 3; ++i) {
        spv::Id vertex_index = vertex_indices[i];
        // Interpolators.
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_index);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          builder.createStore(
              builder.createLoad(builder.createAccessChain(spv::StorageClassInput,
                                                           in_interpolators[j], id_vector_temp),
                                 spv::NoPrecision),
              out_interpolators[j]);
        }
        // Point coordinates.
        if (key.has_point_coordinates) {
          builder.createStore(const_point_coordinates_zero, out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_index);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
        spv::Id vertex_position = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            vertex_position,
            builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(vertex_index);
          id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
          spv::Id vertex_clip_distances = builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision);
          id_vector_temp.clear();
          id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(vertex_clip_distances,
                              builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex,
                                                        id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }

      // Construct the fourth vertex.
      // Interpolators.
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        spv::Id in_interpolator = in_interpolators[i];
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_indices[0]);
        spv::Id vertex_interpolator_v0 = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_interpolator, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = vertex_indices[1];
        spv::Id vertex_interpolator_v01 = builder.createNoContractionBinOp(
            spv::OpFSub, type_float4,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_interpolator, id_vector_temp),
                spv::NoPrecision),
            vertex_interpolator_v0);
        id_vector_temp[0] = vertex_indices[2];
        spv::Id vertex_interpolator_v3 = builder.createNoContractionBinOp(
            spv::OpFAdd, type_float4, vertex_interpolator_v01,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_interpolator, id_vector_temp),
                spv::NoPrecision));
        builder.createStore(vertex_interpolator_v3, out_interpolators[i]);
      }
      // Point coordinates.
      if (key.has_point_coordinates) {
        builder.createStore(const_point_coordinates_zero, out_point_coordinates);
      }
      // Position.
      id_vector_temp.clear();
      id_vector_temp.push_back(vertex_indices[0]);
      id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
      spv::Id vertex_position_v0 = builder.createLoad(
          builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
          spv::NoPrecision);
      id_vector_temp[0] = vertex_indices[1];
      spv::Id vertex_position_v01 = builder.createNoContractionBinOp(
          spv::OpFSub, type_float4,
          builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision),
          vertex_position_v0);
      id_vector_temp[0] = vertex_indices[2];
      spv::Id vertex_position_v3 = builder.createNoContractionBinOp(
          spv::OpFAdd, type_float4, vertex_position_v01,
          builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision));
      id_vector_temp.clear();
      id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
      builder.createStore(
          vertex_position_v3,
          builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
      // Clip distances.
      for (uint32_t i = 0; i < clip_distance_count; ++i) {
        spv::Id const_int_i = builder.makeIntConstant(int32_t(i));
        id_vector_temp.clear();
        id_vector_temp.push_back(vertex_indices[0]);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
        id_vector_temp.push_back(const_int_i);
        spv::Id vertex_clip_distance_v0 = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp[0] = vertex_indices[1];
        spv::Id vertex_clip_distance_v01 = builder.createNoContractionBinOp(
            spv::OpFSub, type_float,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision),
            vertex_clip_distance_v0);
        id_vector_temp[0] = vertex_indices[2];
        spv::Id vertex_clip_distance_v3 = builder.createNoContractionBinOp(
            spv::OpFAdd, type_float, vertex_clip_distance_v01,
            builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
                spv::NoPrecision));
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
        id_vector_temp.push_back(const_int_i);
        builder.createStore(
            vertex_clip_distance_v3,
            builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
      }
      // Emit the vertex.
      builder.createNoResultOp(spv::OpEmitVertex);
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    case PipelineGeometryShader::kQuadList: {
      // Initialize the point coordinates output for safety if this shader type
      // is used with has_point_coordinates for some reason.
      spv::Id const_point_coordinates_zero = spv::NoResult;
      if (key.has_point_coordinates) {
        spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_float_0);
        id_vector_temp.push_back(const_float_0);
        const_point_coordinates_zero = builder.makeCompositeConstant(type_float2, id_vector_temp);
      }

      // Build the triangle strip from the original quad vertices in the
      // 0, 1, 3, 2 order (like specified for GL_QUAD_STRIP).
      // TODO(Triang3l): Find the correct decomposition of quads into triangles
      // on the real hardware.
      for (uint32_t i = 0; i < 4; ++i) {
        spv::Id const_vertex_index = builder.makeIntConstant(int32_t(i ^ (i >> 1)));
        // Interpolators.
        id_vector_temp.clear();
        id_vector_temp.push_back(const_vertex_index);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          builder.createStore(
              builder.createLoad(builder.createAccessChain(spv::StorageClassInput,
                                                           in_interpolators[j], id_vector_temp),
                                 spv::NoPrecision),
              out_interpolators[j]);
        }
        // Point coordinates.
        if (key.has_point_coordinates) {
          builder.createStore(const_point_coordinates_zero, out_point_coordinates);
        }
        // Position.
        id_vector_temp.clear();
        id_vector_temp.push_back(const_vertex_index);
        id_vector_temp.push_back(const_member_in_gl_per_vertex_position);
        spv::Id vertex_position = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
            spv::NoPrecision);
        id_vector_temp.clear();
        id_vector_temp.push_back(const_member_out_gl_per_vertex_position);
        builder.createStore(
            vertex_position,
            builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex, id_vector_temp));
        // Clip distances.
        if (clip_distance_count) {
          id_vector_temp.clear();
          id_vector_temp.push_back(const_vertex_index);
          id_vector_temp.push_back(const_member_in_gl_per_vertex_clip_distance);
          spv::Id vertex_clip_distances = builder.createLoad(
              builder.createAccessChain(spv::StorageClassInput, in_gl_per_vertex, id_vector_temp),
              spv::NoPrecision);
          id_vector_temp.clear();
          id_vector_temp.push_back(const_member_out_gl_per_vertex_clip_distance);
          builder.createStore(vertex_clip_distances,
                              builder.createAccessChain(spv::StorageClassOutput, out_gl_per_vertex,
                                                        id_vector_temp));
        }
        // Emit the vertex.
        builder.createNoResultOp(spv::OpEmitVertex);
      }
      builder.createNoResultOp(spv::OpEndPrimitive);
    } break;

    default:
      assert_unhandled_case(key.type);
  }

  // End the main function.
  builder.leaveFunction();

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the shader module, and store the handle even if creation fails not
  // to try to create it again later.
  VkShaderModule shader_module = ui::vulkan::util::CreateShaderModule(
      vulkan_device, reinterpret_cast<const uint32_t*>(shader_code.data()),
      sizeof(uint32_t) * shader_code.size());
  if (shader_module == VK_NULL_HANDLE) {
    REXLOG_ERROR(
        "VulkanPipelineCache: Failed to create the primitive type geometry "
        "shader 0x{:08X}",
        key.key);
  }
  return shader_module;
}

}  // namespace p2::native
