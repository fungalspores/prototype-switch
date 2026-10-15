// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mercenaries 2 port: tessellation for the native renderer (see native_tessellation.cpp).
#pragma once

#include <cstdint>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/xenos.h>

namespace p2::native::tessellation {

using HostType = rex::graphics::Shader::HostVertexShaderType;

// The domain the guest vertex shader runs as for a tessellated draw (PrimitiveProcessor's
// rules); false for combinations the SDK does not support either.
bool GetHostVertexShaderType(rex::graphics::xenos::PrimitiveType primitive,
                             rex::graphics::xenos::TessellationMode mode, HostType& type_out);
// Control points per host patch; 0 if unsupported.
uint32_t PatchControlPoints(HostType type, rex::graphics::xenos::TessellationMode mode);
// SPIR-V of the helper stages, compiled from GLSL with glslang (once per variant by the caller).
bool CompileVertexShader(bool adaptive, std::vector<uint32_t>& spirv);
// Any fragment shader from GLSL source (diagnostics).
bool CompileFragmentShader(const char* source, std::vector<uint32_t>& spirv);
bool CompileControlShader(HostType type, rex::graphics::xenos::TessellationMode mode,
                          std::vector<uint32_t>& spirv);

}  // namespace p2::native::tessellation
