// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native renderer for the Switch port: the app's own graphics system.
//
// Adapted from the native renderer of nfsmw-nx (StevensND/nfsmw-nx,
// app/src/nfsmw_nativo_sistema.h and .cpp, GPL-3.0), which draws Need for
// Speed: Most Wanted's frames with Vulkan instead of emulating the Xbox 360
// GPU. See docs/SWITCH_NATIVE_RENDERER.md.
//
// With p2_renderer = "native", the app's OnPreSetup puts this system in
// config.graphics and the SDK's Xenos emulation is not created. The game's
// Direct3D keeps running unchanged and fills its command ring; this system
// consumes the ring as the GPU would, answers everything the game waits for
// (registers, memory writes, interrupts, vblanks) and presents on each swap.

#pragma once

#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace p2::native {

// Whether the p2_renderer cvar asks for the native renderer.
bool Enabled();

// Swaps the native renderer has presented since startup (0 when disabled).
uint64_t SwapsPresented();

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem();

}  // namespace p2::native
