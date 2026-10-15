// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native renderer for the Switch port. See native_renderer.h for the design.
//
// Parts of this file are ports of ReXGlue's Vulkan backend (src/graphics/vulkan,
// itself from Xenia: Copyright Ben Vanik and the Xenia contributors, BSD-3-Clause,
// see LICENSES/): the system constants (VulkanCommandProcessor::
// UpdateSystemConstantValues), the dynamic state (UpdateDynamicState), the
// shader modifications and pipeline state (VulkanPipelineCache), and the resolve
// rectangle (draw_util::GetResolveInfo). They are what the translated shaders
// expect, so they are kept as close to the original as the native design allows;
// everything about EDRAM, fragment shader interlock and tessellation is gone.

#include <rex/ui/gpu_wait_diagnostics.h>
#include "native_renderer.h"
#include "skin_write_log.h"
#include "native_vk_extra.h"
#include "native_shader_stats.h"
#include "native_resolve_probe.h"
#include "texture_binding_reuse.h"
#include "blocking_record_queue.h"
#include "occlusion_tracker.h"
#include <rex/thread.h>
#include "native_tessellation.h"

extern "C" void RexSwitchScreenshotRequest() __attribute__((weak));
extern "C" void RexSwitchPerfSceneFrame(uint32_t draws);

#include <algorithm>
#include <cmath>
#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <fmt/format.h>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/string/buffer.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/system/xmemory.h>
#include <rex/system/xvideo.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>

#if defined(__SWITCH__)
extern "C" {
#include <switch/kernel/svc.h>
#include <switch/services/hid.h>
}
#endif

#include "native_geometry_shaders.h"
#include "native_hash.h"
#include "dynamic_buffer_stamp.h"
#include "native_textures.h"

// Texture-sign specialization constants, adapted from project8 SDK patch0015.
// Universal rollback: false and restart. FSI SDK translators remain universal.
REXCVAR_DEFINE_BOOL(gpu_specialize_texture_signs, false, "Prototype2",
                    "Specialize exact texture signedness/gamma at pipeline creation (restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_depth24, false, "Prototype2",
                    "Store depth in 32 bits per pixel (24-bit depth with stencil, or float "
                    "without) instead of 32-bit float with stencil, which NVIDIA GPUs keep "
                    "in 64");
REXCVAR_DECLARE(uint32_t, p2_host_core_mask);  // SDK xthread.cpp
REXCVAR_DECLARE(bool, p2_skin_write_log);  // switch_skinning.cpp
REXCVAR_DEFINE_BOOL(p2_native_copy_ab, false, "Prototype2", "One scene test: compute copies 30s, fragment copies 45s, compute copies thereafter; image-preserving A/B");

REXCVAR_DEFINE_BOOL(p2_frame_diagnostics, false, "Prototype2", "Per-frame percentiles in 300-frame windows, excludes low-draw UI");

REXCVAR_DEFINE_BOOL(p2_native_gpu_profile, false, "Prototype2",
                    "Log GPU time by render pass and resolve, measured on one frame in 16");
REXCVAR_DEFINE_BOOL(p2_native_shader_stats, false, "Prototype2",
                    "Diagnostic: capture native pipeline compiler statistics (requires shader_stats.flag)");
REXCVAR_DEFINE_BOOL(p2_native_copy_specialization, false, "Prototype2",
                    "Specialize exact 1x color copies by clamp, universal fallback (restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_depth_words_direct, false, "Prototype2",
                    "Resolve depth the game also reads as color words in one compute pass from the "
                    "depth and stencil aspects, not through a buffer (restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(p2_native_post_trace_seconds, 0, "Prototype2",
                     "Diagnostic (HANDOFF B:106): from the first shadow cascade resolve on (level), every this many "
                     "seconds log one frame's draws that sample a resolve or draw into a small target, "
                     "and its resolves (SIMTRACE lines), up to 6 frames; 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_scene_r11g11b10, false, "Prototype2",
                    "The game's HDR scene target (7e3 float, k_2_10_10_10_FLOAT) as R11G11B10 float, 32 "
                    "bits per pixel, instead of RGBA16 float, 64: half the memory traffic of the main "
                    "pass and its resolves. No alpha (only 2 bits on the Xbox 360; the post-processing "
                    "reads RGB only) (HANDOFF B:108; restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_dof, true, "Prototype2",
                    "The game's depth of field in its final composite (PS C45C07BB16BF2E70: blends "
                    "a half-resolution blurred picture by depth; at 960x544 distant scenery smears and "
                    "the player's head bleeds into the blurred background as ghost copies). false: "
                    "always the sharp picture (HANDOFF B:108; restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_capture_combo, false, "Prototype2",
                    "Diagnostic (HANDOFF B:108): pressing both stick buttons (L3 + R3) together "
                    "captures the next frame: every draw and resolve (CAPTURE lines), the "
                    "constants of the shadow / composite shaders and images of the shadow atlas, depth, "
                    "scene and post-processing inputs (logs/capN_*.bin). Up to 6 captures; the game "
                    "stops for a few seconds while it is written")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_edge_blur, true, "Prototype2",
                    "The game's depth edge blur after the composite (PS 3E0AB1A6A7CD8CFB, its own "
                    "anti-aliasing: blurs every depth edge, foliage included). false: skipped "
                    "(HANDOFF B:107; restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_motion_blur, true, "Prototype2",
                    "The game's camera motion blur (in its final composite, PS C45C07BB16BF2E70). "
                    "false: no blur and none of its 960x544 sample loop, the player no longer smeared "
                    "into the background (HANDOFF B:107; restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_fragment_copies, false, "Prototype2",
                    "Colour resolves are copied by fragment passes instead of compute dispatches "
                    "(no switch between drawing and compute, each of which idles this GPU) "
                    "(HANDOFF B:112; restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_shadow_atlas_fragment, false, "Prototype2",
                    "Shadow cascades are converted into their atlas by a fragment pass instead of a "
                    "compute dispatch: on this GPU every switch between drawing and compute waits for "
                    "the GPU to go idle (HANDOFF B:111; restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_shadow_depth_clamp, false, "Prototype2",
                    "Shadow cascade draws clamp depth to the cascade's range instead of being clipped by "
                    "it (\"pancaking\"): casters nearer the sun than the cascade still cast shadows "
                    "(HANDOFF B:111; restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_shadow_empty_lit, false, "Prototype2",
                    "Shadow atlas texels nothing was drawn into (cleared to the far plane) never shadow "
                    "a receiver: ground beyond the light's far plane, clipped from the shadow pass, came "
                    "out as a large dark patch that appeared and vanished with the camera angle "
                    "(HANDOFF B:110; restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(p2_native_shadow_max_cascades, 4, "Prototype2",
                      "HANDOFF B:129: shadow cascades drawn (1-4). The game draws 4 (to ~11, 41, 131 and "
                      "401 m); a lower value skips the far ones' caster draws (their maps stay empty = "
                      "lit), saving hundreds of draws per frame when driving");
REXCVAR_DEFINE_BOOL(p2_native_shadow_stagger, false, "Prototype2",
                    "With p2_native_shadow_atlas_direct: the two far shadow cascades (3rd and 4th, "
                    "beyond ~41 m) are redrawn on alternate frames, one per frame, instead of both every "
                    "frame; the other keeps the previous frame's map (HANDOFF B:109; restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_shadow_atlas_direct, false, "Prototype2",
                    "Depth resolves into a texture the game samples taller than the resolve (the "
                    "four shadow cascades, sampled as one 1024x4096 map) are written straight into "
                    "it at their row, not into their own image with only the first copied (HANDOFF "
                    "B:105; restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_pipeline_prewarm, false, "Prototype2",
                    "Record the pipelines the renderer creates (keys and shader microcode, next to "
                    "the driver cache) and, at the next start, translate and create them on a "
                    "low-priority thread before the game needs them (restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_structured_jumps, false, "Prototype2",
                    "Translate shaders whose jumps all go forward with structured ifs instead of "
                    "the program counter loop (SDK patch, HANDOFF B:94; restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_water_dump, false, "Prototype2",
                    "Diagnostic: 20 s after the water first draws, save once the raw texels of the "
                    "textures it reads (VS fetch 16, PS fetch 5 and 6) to logs/water_dump_*.bin")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_water_sim_trace, false, "Prototype2",
                    "Diagnostic: with p2_native_water_dump, log every draw into a small render "
                    "target (host width up to 320) and every resolve of one frame, when the dump fires")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_edram_depth_alias_clear, false, "Prototype2",
                    "A depth clear (screen-space rectangle draw writing only depth/stencil) also "
                    "clears a 32-bit colour target on the same EDRAM tiles, as the shared EDRAM does "
                    "on the Xbox 360 (water simulation, HANDOFF B:102; restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_resolve_probe, false, "Prototype2",
                    "Diagnostic: bounded resolve paths, compute failures and texture deltas every 10 seconds")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_binding_revalidate, false, "Prototype2",
                    "Refresh binding views after texture answer changes; reuse descriptor sets only when live view IDs still match")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_work_stats, false, "Prototype2",
                    "Diagnostic: report pass breaks, resolve and sized-copy work every 10 seconds");
REXCVAR_DEFINE_BOOL(p2_native_gpu_profile_draws, false, "Prototype2",
                    "With p2_native_gpu_profile: also time every draw, grouped by its "
                    "vertex and pixel shader (a timestamp per draw; the times run a little high)");
REXCVAR_DEFINE_BOOL(p2_native_gpu_profile_draw_kinds, false, "Prototype2",
                    "HANDOFF B:122: with p2_native_gpu_profile_draws, group the per-draw times by "
                    "what changed before the draw (pipeline, shared set, constants, vertex/pixel "
                    "textures) instead of by shader pair");
REXCVAR_DEFINE_BOOL(p2_native_occlusion_queries, true, "Prototype2",
                    "Measure guest occlusion queries on the GPU instead of faking visibility");
REXCVAR_DEFINE_BOOL(p2_native_record_thread, false, "Prototype2",
                    "Make the Vulkan calls of draws on a second thread, so the command ring "
                    "thread prepares the next draw meanwhile");
REXCVAR_DEFINE_BOOL(p2_native_dynamic_buffer_upload, false, "Prototype2",
                    "Always copy changing buffers once per frame without a preceding hash; distinct frame-local cache keys and periodic exact checks (restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_msaa, true, "Prototype2",
                    "Render with the multisampling the game asks for; off draws every target "
                    "with one sample, which costs the GPU much less memory bandwidth");
REXCVAR_DEFINE_BOOL(p2_native_water_no_ucp, false, "Prototype2",
                    "Diagnostic: draw the traced (water) shader without user clip planes");
REXCVAR_DEFINE_BOOL(p2_native_water_debug_cycle, false, "Prototype2",
                    "Diagnostic: cycle water draw modes and take screenshots (HANDOFF 57)");
REXCVAR_DEFINE_BOOL(p2_native_sampled_stable_ranges, true, "Prototype2",
                    "With p2_native_dynamic_buffer_upload: vertex/index ranges that stopped changing are "
                    "checked by samples plus a full hash every 16 frames instead of hashing every byte "
                    "every frame. false = exact hash each frame (clothing-twitch diagnostic)");
REXCVAR_DEFINE_BOOL(p2_native_skin_unique, true, "Prototype2",
                    "Vertex ranges on pages the skinning routine wrote in the last 3 frames get a new identity "
                    "(copied, not hashed); false = exact hash checks (WORKLOG #115)");
REXCVAR_DEFINE_UINT32(p2_native_settled_check_every, 4, "Prototype2",
                      "WORKLOG #96: vertex/index ranges below the sampled size that have been byte-identical on "
                      "p2_native_settled_frames exact checks in a row are hashed only every Nth frame (staggered; "
                      "1 = every frame). A change is then seen up to N-1 frames late, once.");
REXCVAR_DEFINE_UINT32(p2_native_settled_frames, 300, "Prototype2",
                      "Exact checks in a row without a change before a small range counts as settled");
REXCVAR_DEFINE_UINT32(p2_native_settled_min_bytes, 1024, "Prototype2",
                      "Smaller ranges (UI quads, particles) are always hashed every frame");
REXCVAR_DEFINE_UINT32(p2_native_sampled_min_bytes, 16384, "Prototype2",
                      "p2_native_sampled_stable_ranges applies only to ranges of at least this many bytes; "
                      "smaller ones are hashed exactly every frame");
REXCVAR_DEFINE_BOOL(p2_native_exact_vertex_check, false, "Prototype2",
                    "Hash whole vertex/index ranges once a frame (no sampled or trusted skips)");
REXCVAR_DEFINE_UINT32(p2_native_auto_capture_draws, 0, "Prototype2",
                      "Diagnostic: when frames have at least this many draws for ~300 frames "
                      "(gameplay), capture one frame like L3+R3 (CAPTURE lines and logs/capN_rNN_*.bin "
                      "images of every resolve), twice 30 s apart. 0 = off");
REXCVAR_DEFINE_BOOL(p2_native_resolve_int_as_unorm, false, "Prototype2",
                    "Integer-number resolves into k_16_16(_16_16) as UNORM/SNORM word/65535 (run10: "
                    "black). false = a float image with the integer value");
REXCVAR_DEFINE_BOOL(p2_native_gamma_shares_storage, true, "Prototype2",
                    "8_8_8_8_GAMMA and 8_8_8_8 colour targets at the same EDRAM base share one "
                    "image (both are stored as R8G8B8A8, gamma is applied in the shader). "
                    "Prototype 2 draws the albedo pass as GAMMA and resolves it as 8_8_8_8 (run14).");
REXCVAR_DEFINE_BOOL(p2_native_resolve_xenia_pack, true, "Prototype2",
                    "Colour resolves into k_16_16 / k_16_16_16_16 textures packed as on the console "
                    "(Xenia XePackFixed: exp bias, then [0,1] or [-1,1] by the copy's number format, "
                    "UNORM/SNORM image). false = the Mercenaries 2 rule (float image, 1/32)");
REXCVAR_DEFINE_DOUBLE(p2_native_signed_resolve_scale, 1.0, "Prototype2",
                      "Factor for k_16_16(_16_16) render targets resolved to float textures "
                      "(diagnostic; they are stored as floats with the guest values)");
REXCVAR_DEFINE_STRING(p2_native_trace_vs, "", "Prototype2",
                      "Hex vertex shader hash whose draw setups are all logged (empty: none)");
REXCVAR_DEFINE_STRING(p2_native_skip_ps, "", "Prototype2",
                      "Diagnostic (HANDOFF B:113): comma-separated hex pixel shader hashes whose draws "
                      "are not issued, to see what a pass contributes (empty: none; restart required)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_skip_depth_prepass, false, "Prototype2",
                    "HANDOFF B:118: depth-only draws (no colour target, depth write, LESS/LEQUAL, no "
                    "stencil) outside the shadow maps are not issued: the colour draws of the same "
                    "objects write the same depth. Draws that write nothing at all are dropped too")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_reflection_half_rate, false, "Prototype2",
                    "HANDOFF B:118: the water reflection pass (the target the water PS 9AD799B0 "
                    "samples) is drawn and resolved on every other frame only; the water keeps the "
                    "previous reflection in between")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(p2_native_trace_ps, "", "Prototype2",
                      "Hex pixel shader hash whose draw setups are logged (empty: none)");
REXCVAR_DEFINE_BOOL(p2_native_water_lod_probe, false, "Prototype2",
                    "Diagnostic: bounded successful water setup fetch/mip/sampler/constants trace; restart required")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(p2_native_async_pipelines, true, "Prototype2",
                    "Native renderer: compile new pipelines on a worker thread and skip the "
                    "draws that need them until they are ready, instead of stalling the frame");

namespace p2::native {
namespace shaders {
#include "shaders/vulkan_spirv/apply_gamma_table_cs.h"
#include "shaders/depth_resolve_cs.h"
#include "shaders/depth_to_float_cs.h"
#include "shaders/depth_to_float_fs.h"
#include "shaders/copy_color_fs.h"
#include "shaders/fullscreen_vs.h"
#include "shaders/depth_words_cs.h"
#include "shaders/copy_color_cs.h"
#include "shaders/copy_color_specialized_cs.h"
}  // namespace shaders

namespace {

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
namespace draw_util = rex::graphics::draw_util;
using rex::graphics::RegisterFile;
using rex::graphics::Shader;
using rex::graphics::SpirvShader;
using rex::graphics::SpirvShaderTranslator;
using rex::graphics::vulkan::VulkanShader;
using rex::ui::vulkan::VulkanDevice;

// Four: with three, a single frame of heavy GPU work stalled the ring thread
// at once (30-49 ms waits for a free slot in frames that then dropped).
// WORKLOG #102: 8 (was 4). Since the sun occlusion queries each query end submits (2.3-3 submissions per frame),
// 4 slots were ~1.5 frames: the ring thread waited on GPU fences to reuse a slot.
constexpr uint32_t kSubmissionSlots = 8;
// Vertex, index, constant and texture upload data of the submissions in flight.
constexpr VkDeviceSize kStreamSize = 160u << 20;
// Room at the end of the stream for the fixed ranges of constant bindings.
constexpr VkDeviceSize kStreamGuard = 16u << 10;
// Past the end of the stream, never allocated from but inside the range the
// shaders' vertex fetches may address: a fetch past its copied data (a guest
// index out of range) reads garbage there instead of faulting the GPU's MMU,
// which loses the device for good.
constexpr VkDeviceSize kStreamSlack = 16u << 20;
// Sizes of the constant buffer bindings. The float ones are bound with their
// maximum size and filled up to the shader's count.
constexpr VkDeviceSize kFloatConstantsBytes = 256 * 4 * sizeof(float);
constexpr VkDeviceSize kBoolLoopConstantsBytes = sizeof(uint32_t) * (8 + 32);
constexpr VkDeviceSize kFetchConstantsBytes = sizeof(uint32_t) * 6 * 32;

// HANDOFF B:108 (p2_native_scene_r11g11b10): the 7e3 float targets (the game's HDR scene) in
// 32 bits per pixel instead of 64. Set once before any render target exists.
bool g_float7e3_as_r11g11b10 = false;

// The colour target image key format: the EDRAM storage format, with GAMMA folded into 8_8_8_8
// (same bits in EDRAM; the draw converts to gamma in the shader from RB_COLOR_INFO).
uint32_t RtKeyColorFormat(xenos::ColorRenderTargetFormat format) {
  auto storage = xenos::GetStorageColorFormat(format);
  if (storage == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
      REXCVAR_GET(p2_native_gamma_shares_storage)) {
    storage = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  }
  return uint32_t(storage);
}

VkFormat ColorFormatToVk(xenos::ColorRenderTargetFormat format) {
  using F = xenos::ColorRenderTargetFormat;
  if (g_float7e3_as_r11g11b10 && format == F::k_2_10_10_10_FLOAT) {
    return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
  }
  switch (format) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_GAMMA:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    // Mercenaries 2 (HANDOFF section 54): the signed 16-bit fixed targets hold [-32, 32].
    // Float storage keeps the guest values (no 1/32 scale), so blending works on them as on
    // the Xbox 360; SNORM made the water simulation diverge (spikes, run21).
    case F::k_16_16:
      return VK_FORMAT_R16G16_SFLOAT;
    case F::k_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case F::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case F::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

bool IsColorFormat64bpp(xenos::ColorRenderTargetFormat format) {
  using F = xenos::ColorRenderTargetFormat;
  return format == F::k_16_16_16_16 || format == F::k_16_16_16_16_FLOAT ||
         format == F::k_32_32_FLOAT;
}

// Depth buffers are D32_SFLOAT_S8_UINT unless p2_native_depth24 stores the
// game's unorm 24-bit ones as D24_UNORM_S8_UINT and its float 24-bit ones as
// D32_SFLOAT (see DepthFormatFor). A resolve turns depth into the R32_SFLOAT
// the translated shaders sample k_24_8 as, and into the guest's memory words
// (see Resolve).
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;

VkBlendFactor BlendFactorToVk(xenos::BlendFactor factor) {
  switch (uint32_t(factor)) {
    case 0:
      return VK_BLEND_FACTOR_ZERO;
    case 1:
      return VK_BLEND_FACTOR_ONE;
    case 4:
      return VK_BLEND_FACTOR_SRC_COLOR;
    case 5:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6:
      return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8:
      return VK_BLEND_FACTOR_DST_COLOR;
    case 9:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10:
      return VK_BLEND_FACTOR_DST_ALPHA;
    case 11:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12:
      return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14:
      return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16:
      return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:
      return VK_BLEND_FACTOR_ZERO;
  }
}

VkBlendOp BlendOpToVk(xenos::BlendOp op) {
  switch (uint32_t(op)) {
    case 1:
      return VK_BLEND_OP_SUBTRACT;
    case 2:
      return VK_BLEND_OP_MIN;
    case 3:
      return VK_BLEND_OP_MAX;
    case 4:
      return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:
      return VK_BLEND_OP_ADD;
  }
}

struct RenderTargetKey {
  uint32_t base_tiles;
  uint32_t pitch;
  uint32_t format;
  uint32_t msaa;
  uint32_t is_depth;
  bool operator==(const RenderTargetKey& o) const { return !std::memcmp(this, &o, sizeof(o)); }
};
struct RenderTargetKeyHasher {
  size_t operator()(const RenderTargetKey& k) const { return size_t(XXH3_64bits(&k, sizeof(k))); }
};

struct RenderTarget {
  RenderTargetKey key;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  // Depth only, for the compute pass that resolves depth.
  VkImageView sample_view = VK_NULL_HANDLE;
  // Depth with stencil only, for the depth words pass (p2_native_depth_words_direct).
  VkImageView stencil_view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t last_used_frame = 0;
};

// The attachments of a rendering scope: color 0-3 and depth.
struct PassTargets {
  RenderTarget* color[4] = {};
  RenderTarget* depth = nullptr;
  bool operator==(const PassTargets& o) const {
    return !std::memcmp(this, &o, sizeof(o));
  }
  bool Empty() const { return !color[0] && !color[1] && !color[2] && !color[3] && !depth; }
};

// Everything a graphics pipeline depends on, hashed as plain memory.
struct PipelineKey {
  uint64_t vs_hash;
  uint64_t vs_modification;
  uint64_t ps_hash;
  uint64_t ps_modification;
  uint32_t color_formats[4];
  uint32_t depth_format;
  uint32_t samples;
  uint32_t topology;
  uint32_t geometry_shader;
  uint32_t primitive_restart;
  uint32_t cull_mode;
  uint32_t front_face_clockwise;
  uint32_t polygon_line;
  uint32_t depth_clamp;
  uint32_t depth_test;
  uint32_t depth_write;
  uint32_t depth_compare;
  uint32_t stencil_test;
  uint32_t stencil_front[4];
  uint32_t stencil_back[4];
  uint32_t blend[4];
  uint32_t write_masks;
  // Mercenaries 2: host vertex shader type | tessellation mode << 8 | patch points << 16
  // for tessellated draws (native_tessellation.h), 0 otherwise. Two words keep the key
  // free of padding.
  uint32_t tessellation;
  uint32_t tessellation_reserved;
  uint32_t texture_signs[8];
  uint64_t layout;
};

struct PipelineLayout {
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout textures_vertex = VK_NULL_HANDLE;
  VkDescriptorSetLayout textures_pixel = VK_NULL_HANDLE;
};

using VertexRangeKey = BufferRangeKey;
struct VertexRangeKeyHasher {
  size_t operator()(const VertexRangeKey& k) const { return size_t(k.hash ^ k.address); }
};

}  // namespace

struct Renderer::Impl {
  Renderer* owner = nullptr;
  rex::ui::vulkan::VulkanProvider* provider = nullptr;
  const VulkanDevice* device = nullptr;
  VkDevice vk = VK_NULL_HANDLE;
  rex::memory::Memory* memory = nullptr;
  RegisterFile* regs = nullptr;

  // Shaders.
  std::unique_ptr<SpirvShaderTranslator> translator;
  rex::string::StringBuffer disasm;
  std::unordered_map<uint64_t, std::unique_ptr<VulkanShader>> shaders;
  struct KnownShader {
    VulkanShader* shader;
    uint64_t sample;  // of all of the code
    uint64_t verified_frame;
    uint64_t head;  // of the first 16 words
  };
  std::unordered_map<uint64_t, KnownShader> shaders_by_location;
  VulkanShader* active_vs = nullptr;
  VulkanShader* active_ps = nullptr;
  bool specialize_signs = false;
  bool structured_jumps = false;  // p2_native_structured_jumps

  // Layouts.
  VkDescriptorSetLayout layout_empty = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_shared = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_constants = VK_NULL_HANDLE;
  uint32_t shared_binding_count = 1;
  std::unordered_map<uint64_t, VkDescriptorSetLayout> texture_set_layouts;
  std::unordered_map<uint64_t, PipelineLayout> pipeline_layouts;

  // The stream: one host-visible buffer the shaders read vertices from (as
  // the translator's "shared memory"), with the indices, the constants and the
  // texture staging data in it too.
  VkBuffer stream_buffer = VK_NULL_HANDLE;
  VkDeviceMemory stream_memory = VK_NULL_HANDLE;
  uint8_t* stream_mapping = nullptr;
  uint64_t stream_head = 0;  // total bytes ever allocated
  VkDescriptorPool static_pool = VK_NULL_HANDLE;
  VkDescriptorSet set_shared = VK_NULL_HANDLE;
  VkDescriptorSet set_constants = VK_NULL_HANDLE;
  VkDeviceSize ubo_alignment = 256;

  // Submissions.
  static constexpr uint32_t kOcclusionPerSubmission = 512;
  using QueryTracker = OcclusionTracker<512>;
  VkQueryPool occlusion_pool = VK_NULL_HANDLE;
  QueryTracker occlusion;
  VkQueryControlFlags occlusion_flags = 0;
  uint64_t occlusion_completed = 0;
  struct Submission {
    VkCommandPool pool = VK_NULL_HANDLE;
    // The main command buffer's own: the setup one is recorded on the ring
    // thread while the recording thread records this one.
    VkCommandPool main_pool = VK_NULL_HANDLE;
    VkCommandBuffer setup = VK_NULL_HANDLE;
    VkCommandBuffer main = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    uint64_t number = 0;
    uint64_t frame = 0;
    // Oldest stream position the submission reads.
    uint64_t stream_oldest = 0;
    std::vector<VkDescriptorPool> descriptor_pools;
    size_t descriptor_pool_used = 0;
    bool timed = false;
    uint32_t occlusion_count = 0;
    std::array<QueryTracker::Owners,kOcclusionPerSubmission> occlusion_owners{};
  };
  VkQueryPool timestamps = VK_NULL_HANDLE;
  float timestamp_period = 1.0f;
  Submission submissions[kSubmissionSlots];
  uint64_t submission_number = 0;  // the one being recorded, if recording
  bool recording = false;
  uint64_t frame = 1;
  uint64_t completed_frame = 0;
  // Whether render targets get the game's sample count (p2_native_msaa).
  // The targets are sized in guest pixels whatever the sample count, so one
  // sample changes only the images, the pipelines and the resolves.
  bool multisample = true;
  // D24_UNORM_S8_UINT for the game's kD24S8 buffers (p2_native_depth24).
  bool depth24 = false;
  // The float24 ones (here only shadow maps) become D32_SFLOAT without
  // stencil, 32 instead of 64 bits per pixel; a stencil test on them passes.
  VkFormat DepthFormatFor(uint32_t guest_format) const {
    if (!depth24) return kDepthFormat;
    // Prototype 2 (run17): its main depth buffer is kD24FS8 and the light passes use its stencil,
    // so float24 keeps D32_SFLOAT_S8_UINT (Mercs2 had float24 only for shadow maps and dropped the
    // stencil: the sun mask came out black). Only unorm 24-bit buffers become D24_UNORM_S8_UINT.
    return guest_format == uint32_t(xenos::DepthRenderTargetFormat::kD24S8)
               ? VK_FORMAT_D24_UNORM_S8_UINT
               : kDepthFormat;
  }
  static bool HasStencil(VkFormat format) { return format != VK_FORMAT_D32_SFLOAT; }
  VkDescriptorSetLayout depth_convert_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout depth_convert_layout = VK_NULL_HANDLE;
  VkPipeline depth_convert_pipeline = VK_NULL_HANDLE;
  VkDescriptorPool depth_convert_pool = VK_NULL_HANDLE;
  VkDescriptorSet depth_convert_set = VK_NULL_HANDLE;
  // Depth straight from the depth buffer into the R32_SFLOAT image, in one
  // compute pass (depth_to_float.cs.glsl).
  VkDescriptorSetLayout depth_copy_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout depth_copy_layout = VK_NULL_HANDLE;
  VkPipeline depth_copy_pipeline = VK_NULL_HANDLE;
  // Depth and words in one pass (depth_words.cs.glsl), when enabled.
  VkDescriptorSetLayout depth_words_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout depth_words_layout = VK_NULL_HANDLE;
  VkPipeline depth_words_pipeline = VK_NULL_HANDLE;
  // Color resolves by compute (copy_color.cs.glsl), same set layout.
  VkPipelineLayout copy_color_layout = VK_NULL_HANDLE;
  VkPipeline copy_color_pipeline = VK_NULL_HANDLE;
  std::array<VkPipeline, 3> copy_color_specialized{};
  bool specialize_copy = false;
  bool ComputeColorCopy(VkCommandBuffer c, VkImageView source, VkImageView dest, int32_t sx,
                        int32_t sy, int32_t dx, int32_t dy, uint32_t width, uint32_t height,
                        uint32_t sample_x = 1, uint32_t sample_y = 1, float scale = 1.0f,
                        uint32_t clamp_mode = 0, VkFormat dest_format = VK_FORMAT_UNDEFINED);
  // HANDOFF B:112 (p2_native_fragment_copies).
  bool fragment_copies = false;
  VkPipelineLayout copy_fragment_layout = VK_NULL_HANDLE;
  std::unordered_map<uint32_t, VkPipeline> copy_fragment_pipelines;  // by destination format
  bool EnsureFragmentSetLayout();
  VkPipeline CopyFragmentPipeline(VkFormat format);
  VkPipeline CreateFullscreenPipeline(VkPipelineLayout layout, const uint32_t* fs_code,
                                      size_t fs_bytes, VkFormat format,
                                      VkColorComponentFlags write_mask);
  bool FragmentColorCopy(VkCommandBuffer c, VkImageView source, VkImageView dest,
                         VkFormat dest_format, int32_t sx, int32_t sy, int32_t dx, int32_t dy,
                         uint32_t width, uint32_t height, uint32_t sample_x, uint32_t sample_y,
                         float scale, uint32_t clamp_mode);
  // Resolve combinations already reported in the log (source format, destination format,
  // exponent bias, samples).
  std::unordered_set<uint32_t> resolve_logged;
  // HANDOFF B:105 (p2_native_shadow_atlas_direct).
  bool shadow_atlas_direct = false;
  // HANDOFF B:109 (p2_native_shadow_stagger). The cascades are drawn in one depth target and
  // resolved in order to rows 0, 1024, 2048, 3072 of the atlas; the draws before the k-th resolve
  // of a frame belong to cascade k.
  bool shadow_stagger = false;
  uint32_t shadow_max_cascades = 4;  // HANDOFF B:129
  bool shadow_empty_lit = false;
  bool shadow_depth_clamp = false;
  // HANDOFF B:111 (p2_native_shadow_atlas_fragment).
  VkDescriptorSetLayout atlas_fragment_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout atlas_fragment_layout = VK_NULL_HANDLE;
  VkPipeline atlas_fragment_pipeline = VK_NULL_HANDLE;
  bool CreateAtlasFragmentPipeline();
  bool AtlasFragmentPass(VkCommandBuffer c, const RenderTarget& source,
                         ResolveDestination::Sized& atlas, int32_t x0, int32_t y0, uint32_t width,
                         uint32_t height, uint32_t row);
  // After commands recorded outside the draw path, which binds by differences.
  void ForgetBoundGraphicsState() {
    bound_pipeline = VK_NULL_HANDLE;
    bound_layout = VK_NULL_HANDLE;
    bound_static_sets = false;
    std::memset(&bound_viewport, 0, sizeof(bound_viewport));
    std::memset(&bound_scissor, 0, sizeof(bound_scissor));
    bound_depth_bias[0] = bound_depth_bias[1] = NAN;
    bound_offsets_valid = false;
    bound_texture_sets[0] = bound_texture_sets[1] = VK_NULL_HANDLE;
    for (float& f : bound_blend_constants) f = NAN;
    for (uint32_t& v : bound_stencil) v = ~0u;
  }
  bool shadow_stagger_broken = false;
  bool shadow_rt_known = false;
  uint32_t shadow_rt_base = 0, shadow_rt_pitch = 0, shadow_rt_format = 0;
  uint32_t shadow_cascade = 0;  // resolves of the cascades seen this frame
  uint64_t work_shadow_skipped_draws = 0, work_shadow_skipped_resolves = 0;
  bool ShadowCascadeSkipped(uint32_t cascade) const {
    return shadow_stagger && !shadow_stagger_broken && (cascade == 2 || cascade == 3) &&
           (cascade & 1) != (frame & 1);
  }
  bool IsShadowTarget(const RenderTarget* rt) const {
    return shadow_rt_known && rt && rt->key.is_depth && rt->key.base_tiles == shadow_rt_base &&
           rt->key.pitch == shadow_rt_pitch && rt->key.format == shadow_rt_format;
  }
  // HANDOFF B:107 (p2_native_motion_blur = false).
  bool motion_blur_off = false;
  bool edge_blur_off = false;
  bool dof_off = false;
  std::unordered_set<uint64_t> skip_ps;  // p2_native_skip_ps
  // HANDOFF B:118.
  bool skip_depth_prepass = false;
  bool reflection_half_rate = false;
  uint32_t reflection_texture = 0;  // what the water PS fetches as t0 (the reflection resolve)
  RenderTargetKey reflection_color = {}, reflection_depth = {};
  bool reflection_color_known = false, reflection_depth_known = false;
  uint64_t reflection_resolved_frame = 0;  // last frame the reflection was resolved
  uint64_t work_prepass_skipped = 0, work_reflection_skipped = 0;
  uint64_t prepass_at_report = 0, reflection_at_report = 0;
  bool ReflectionSkipFrame() const {
    return reflection_half_rate && reflection_color_known && (frame & 1) &&
           reflection_resolved_frame + 1 == frame;
  }
  // HANDOFF B:108 (p2_native_capture_combo).
  bool capture_combo = false;
  bool capture_all = false;  // the traced frame logs every draw
  uint32_t capture_resolve_dumps = 0;  // Prototype 2: source images of the captured frame's resolves
  // Prototype 2 automatic capture: once the frame has this many draws for 10 s (gameplay), the
  // frame is captured like L3+R3 does, twice, 30 s apart (p2_native_auto_capture_draws).
  uint32_t auto_capture_draws = 0;
  uint32_t auto_capture_streak = 0;
  uint32_t auto_capture_done = 0;
  std::chrono::steady_clock::time_point auto_capture_next{};
  void ArmCapture(const char* why);
  uint32_t capture_count = 0;
  std::chrono::steady_clock::time_point capture_held_since{};
  bool capture_latched = false;  // fired for this hold
  void PollCaptureCombo();
  static constexpr uint64_t kEdgeBlurPs = UINT64_C(0x3E0AB1A6A7CD8CFB);
  static constexpr uint64_t kMotionBlurPs = UINT64_C(0xC45C07BB16BF2E70);
  uint64_t work_atlas_direct = 0;
  bool CreateDepthConversion();
  // HANDOFF B:91 (p2_native_depth_words_direct): false leaves the resolve to the buffer route.
  bool DepthWordsDirect(VkCommandBuffer c, const RenderTarget& source, ResolveDestination& dest,
                        uint32_t dest_base, uint32_t dest_width, uint32_t dest_height, int32_t x0,
                        int32_t y0, uint32_t width, uint32_t height);

  // GPU time by render pass and resolve (p2_native_gpu_profile): on one
  // frame in 16, a timestamp is written where each pass or resolve begins,
  // and the time to the next one is charged to it.
  static constexpr uint32_t kProfileQueries = 4096;
  VkQueryPool profile_pool = VK_NULL_HANDLE;
  bool profile_frame = false;
  bool profile_draws = false;
  bool profile_draw_kinds = false;  // B:122
  struct ProfileSegment {
    uint64_t key;
    uint32_t draws;
    // Index or vertex counts of those draws as submitted to the host.
    uint64_t elements = 0;
  };
  std::vector<ProfileSegment> profile_segments[kSubmissionSlots];
  bool profile_closed[kSubmissionSlots] = {};
  struct ProfileTotal {
    double ns = 0;
    uint64_t count = 0;
    uint64_t draws = 0;
    uint64_t elements = 0;
  };
  std::unordered_map<uint64_t, ProfileTotal> profile_totals;
  uint32_t profiled_frames = 0;
  double profiled_ns = 0;
  // Kinds, in the key's top 4 bits.
  enum : uint64_t { kProfileOther = 0, kProfilePass = 1, kProfileResolve = 2, kProfileDraw = 3 };
  static uint64_t ProfileTargetBits(const RenderTarget* rt) {
    if (!rt) return 0xFFFFFF;
    return (uint64_t(rt->key.base_tiles & 0xFFF) << 12) | (uint64_t(rt->key.pitch & 0x7FF) << 1) |
           (rt->key.format & 1);
  }
  void ProfileMark(uint64_t key) {
    if (!profile_frame || !profile_pool || !recording) return;
    uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
    auto& segments = profile_segments[slot];
    // The last query is kept for the closing timestamp.
    if (segments.size() + 1 >= kProfileQueries) return;
    Extra(device).vkCmdWriteTimestamp(cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profile_pool,
                              slot * kProfileQueries + uint32_t(segments.size()));
    segments.push_back({key, 0});
  }
  void ProfileDraw(uint32_t elements) {
    if (!profile_frame) return;
    auto& segments = profile_segments[submission_number % kSubmissionSlots];
    if (!segments.empty()) {
      ++segments.back().draws;
      segments.back().elements += elements;
    }
  }
  void ProfileCollect(uint32_t slot);
  struct TextureTimer {
    uint64_t& total;
    uint64_t start;
    explicit TextureTimer(uint64_t& t) : total(t), start(NowNs()) {}
    ~TextureTimer() { total += NowNs() - start; }
  };
  void ProfileReport();
  uint32_t HostMsaa(xenos::MsaaSamples guest) const {
    return multisample ? uint32_t(guest) : 0;
  }
  // Mercenaries 2 (HANDOFF section 50): without host MSAA, a guest multisampled surface is
  // drawn with its samples as pixels (4x: 2x2, 2x: 1x2), at the resolution its EDRAM layout
  // has as a 1x surface. The game draws depth at 4x (640 wide) and reuses the same EDRAM as
  // 1x (1280 wide) and back every frame; with this both are one image with one layout.
  static constexpr uint64_t kVposScaleXBit = uint64_t(1) << 51;
  static constexpr uint64_t kVposScaleYBit = uint64_t(1) << 52;
  // Translators by draw resolution scale for VPOS: [x2][y2].
  std::unique_ptr<SpirvShaderTranslator> scaled_translators[2][2];
  std::unique_ptr<SpirvShaderTranslator> scaled_worker_translators[2][2];
  SpirvShaderTranslator* TranslatorFor(const VulkanShader::VulkanTranslation& translation,
                                       bool worker) {
    const uint64_t value = translation.modification();
    const uint32_t x2 = (value & kVposScaleXBit) ? 1 : 0, y2 = (value & kVposScaleYBit) ? 1 : 0;
    if (translation.shader().type() != xenos::ShaderType::kPixel || (!x2 && !y2)) {
      return worker ? worker_translator.get() : translator.get();
    }
    auto& slot = worker ? scaled_worker_translators[x2][y2] : scaled_translators[x2][y2];
    if (!slot) {
      slot = std::make_unique<SpirvShaderTranslator>(SpirvShaderTranslator::Features(device), true,
                                                     true, false, x2 ? 2 : 1, y2 ? 2 : 1);
      slot->SetTextureSignSpecializationEnabled(specialize_signs);
      slot->SetStructuredForwardJumpsEnabled(structured_jumps);
    }
    return slot.get();
  }
  uint32_t SampleScaleX(xenos::MsaaSamples guest) const {
    return !multisample && guest >= xenos::MsaaSamples::k4X ? 2 : 1;
  }
  uint32_t SampleScaleY(xenos::MsaaSamples guest) const {
    return !multisample && guest >= xenos::MsaaSamples::k2X ? 2 : 1;
  }
  bool null_images_initialized = false;

  // Render targets.
  std::unordered_map<RenderTargetKey, std::unique_ptr<RenderTarget>, RenderTargetKeyHasher>
      render_targets;
  bool in_pass = false;
  PassTargets pass;
  VkExtent2D pass_extent = {};

  // Pipelines.
  std::unordered_map<uint64_t, VkPipeline> pipelines;
  std::unordered_map<uint32_t, VkShaderModule> geometry_shaders;
  // Tessellation (native_tessellation.h): helper shader modules by variant, and the
  // vertex shaders of tessellated draws already reported in the log.
  bool tessellation_supported = false;
  std::unordered_map<uint32_t, VkShaderModule> tessellation_shaders;
  std::unordered_set<uint64_t> tessellation_logged;
  VkShaderModule GetTessellationModule(bool control, tessellation::HostType type,
                                       xenos::TessellationMode mode) {
    const uint32_t variant =
        (uint32_t(control) << 16) | (uint32_t(type) << 8) |
        (control || mode == xenos::TessellationMode::kAdaptive ? uint32_t(mode) : 0u);
    auto it = tessellation_shaders.find(variant);
    if (it != tessellation_shaders.end()) return it->second;
    std::vector<uint32_t> spirv;
    bool compiled = control ? tessellation::CompileControlShader(type, mode, spirv)
                            : tessellation::CompileVertexShader(
                                  mode == xenos::TessellationMode::kAdaptive, spirv);
    VkShaderModule module = VK_NULL_HANDLE;
    if (compiled) {
      VkShaderModuleCreateInfo info = {};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = spirv.size() * sizeof(uint32_t);
      info.pCode = spirv.data();
      if (dfn().vkCreateShaderModule(vk, &info, nullptr, &module) != VK_SUCCESS) {
        module = VK_NULL_HANDLE;
      }
    }
    if (!module) {
      REXLOG_ERROR("[native] Tessellation {} shader unavailable (type {}, mode {})",
                   control ? "control" : "vertex", uint32_t(type), uint32_t(mode));
    }
    tessellation_shaders.emplace(variant, module);
    return module;
  }
  VkPipelineCache driver_cache = VK_NULL_HANDLE;
  std::filesystem::path driver_cache_path;
  uint64_t pipelines_at_last_save = 0;
  std::chrono::steady_clock::time_point last_pipeline_time;
  size_t driver_cache_saved_size = 0;
  std::atomic<bool> driver_cache_writing{false};
  std::thread driver_cache_writer;

  // Frame time tracking.
  std::chrono::steady_clock::time_point last_swap_time;
  std::array<double, 300> p2_frame_ms{};
  size_t p2_frame_count = 0;
  std::chrono::steady_clock::time_point p2_copy_scene_start;
  int p2_copy_phase = -1;
  RendererStats at_last_swap;

  // What the main command buffer has bound.
  VkPipeline bound_pipeline = VK_NULL_HANDLE;
  VkPipelineLayout bound_layout = VK_NULL_HANDLE;
  bool bound_static_sets = false;
  VkViewport bound_viewport = {};
  VkRect2D bound_scissor = {};
  float bound_depth_bias[2] = {NAN, NAN};
  float bound_blend_constants[4] = {NAN, NAN, NAN, NAN};
  uint32_t bound_stencil[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};

  // Vertex and index data already in the stream: stream position by the
  // range and its content.
  struct RangeCopy {
    VertexRangeKey key = {};
    uint64_t position = 0;
  };
  FlatMap64<RangeCopy> vertex_ranges;
  static uint64_t RangeSlot(const VertexRangeKey& k) {
    uint64_t h = k.hash ^ ((uint64_t(k.address) << 32) | k.size);
    if (k.frame_local) h ^= k.epoch * 0xD1B54A32D192ED03ull ^ 0x94D049BB133111EBull;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31;
    return h == ~uint64_t(0) ? 0 : h;
  }
  // The stream position of a copy of this range, or UINT64_MAX.
  uint64_t FindRange(const VertexRangeKey& k) {
    RangeCopy* c = vertex_ranges.find(RangeSlot(k));
    return c && c->key == k ? c->position : UINT64_MAX;
  }
  void AddRange(const VertexRangeKey& k, uint64_t position) {
    vertex_ranges[RangeSlot(k)] = {k, position};
  }

  TextureCache textures;
  UploadTarget upload;

  // Texture descriptor sets, by content: the game uses the same few hundred
  // combinations of textures and samplers every frame, and allocating and
  // writing a set per draw (then freeing them one by one on pool reset) was
  // an eighth of the renderer's time.
  std::unordered_map<uint64_t, VkDescriptorSet> texture_set_cache;
  std::vector<VkDescriptorPool> set_pools;
  size_t set_pool_used = 0;
  struct RetiredPools {
    uint64_t frame;
    std::vector<VkDescriptorPool> pools;
  };
  std::vector<RetiredPools> retired_set_pools;
  uint64_t texture_views_epoch = 0;

  // Vertex buffer content hashes, computed once per frame for each range: many
  // draws share one buffer.
  struct VertexHash {
    uint64_t frame = UINT64_MAX;
    uint64_t hash = 0;
    // Last real content hash, and how many checks in a row found it changed.
    uint64_t content = 0;
    uint32_t changes = 0;
    ChangeDetector detector;
    DynamicBufferStamp dynamic;
    uint64_t epoch = 0;
    bool frame_local = false;
    bool skin_valid = false;
    uint32_t skin_sequence = 0;
    uint64_t skin_id = 0;
    bool skin_pages = false;  // ever overlapped pages the skinning routine wrote: exact checks only
  };
  // This frame's hash of a vertex or index range. Data the game rewrites
  // every frame (skinned meshes, particles) was hashed in full every frame
  // only to be copied anyway, as the hash never matched: once a range has
  // changed on 4 checks in a row it gets a hash unique to the frame without
  // reading it (still copied once per frame, shared by its draws), and is
  // checked for real every 16 frames to notice it settling.
  uint64_t RangeHash(VertexHash& known, const uint8_t* data, uint32_t bytes, uint32_t address,
                     bool index = false) {
    // Written by the skinning routine within the last frames: a new identity per write, no hashing.
    if (skin_write_log) {
      uint32_t sequence;
      if (p2::skinlog::Lookup(address, bytes, uint32_t(frame), &sequence)) {
        if (!known.skin_valid || known.skin_sequence != sequence) {
          known.skin_valid = true;
          known.skin_sequence = sequence;
          known.skin_id = ((uint64_t(address) << 32) | sequence) * 0x9E3779B97F4A7C15ull ^ 0x51A0D0C5ull;
          ++skin_log_fresh;
        } else {
          ++skin_log_same;
        }
        known.frame = frame;
        known.hash = known.skin_id;
        known.epoch = 0;
        known.frame_local = false;
        return known.hash;
      }
      if (known.skin_valid) {
        // Back to content checks: forget the identities so the first check really hashes.
        known.skin_valid = false;
        known.frame = UINT64_MAX;
        known.detector = ChangeDetector();
        known.dynamic = DynamicBufferStamp();
      }
    }
    if (dynamic_buffer_upload) {
      const bool first = known.frame != frame;
      const uint64_t hashed_before = vertex_hash_bytes;
      const bool ever_changed = known.detector.changes() != 0;
      const auto stamp = known.dynamic.Check(frame, [&] {
        // Ranges that stopped changing: sampled checks with a full hash every 16 frames (and
        // none in between once trusted), as for textures, instead of every byte every frame
        // (run55: 10% of a core). Changing ranges never get here (fresh copies).
        // Small ranges (UI quads, the save-notice screen flickered with them sampled) stay exact, and so
        // does every range that has ever changed: a skinned mesh whose few moving vertices the samples
        // missed was taken as settled and reused stale (characters twitching in cutscenes, run57).
        // Ranges on pages the skinning routine wrote (WORKLOG #95): a mesh resting through a cutscene
        // looks settled, then its first moving frames were missed by both shortcuts (run78 proved it).
        if (!known.skin_pages && !index &&
            p2::skinlog::PagesSkinned(address, bytes, uint32_t(frame), skin_page_window)) {
          known.skin_pages = true;
          ++skin_page_ranges;
        }
        if (known.skin_pages) {
          // Skinned within the last frames (WORKLOG #115): a new identity, no hashing (run83: skinned outputs
          // move every frame, so their hash never matched anything and XXH3 was 6.5 % of the ring thread).
          // Treating unchanged data as changed only costs a copy; never stale.
          if (skin_unique && p2::skinlog::PagesSkinned(address, bytes, uint32_t(frame), 3)) {
            ++skin_page_unique;
            return uint64_t(((frame + 1) * 0xD1B54A32D192ED03ull) ^ (uint64_t(address) << 20) ^ bytes ^ 0x5C1D5EEDull);
          }
          ++skin_page_exact;
          return known.detector.CheckExact(data, bytes, vertex_hash_bytes);
        }
        if (sampled_stable_ranges && bytes >= sampled_min_bytes && known.detector.changes() == 0)
          return known.detector.Check(data, bytes, frame, address >> 4, vertex_hash_bytes);
        // Settled small ranges (run63: ~1000 index and ~1000 small vertex ranges per frame, 5 MB, mostly
        // static props): exact, but only every Nth frame, staggered by address.
        if (settled_every > 1 && bytes >= settled_min_bytes && known.detector.unchanged() >= settled_frames &&
            known.detector.changes() == 0 &&
            (frame + (address >> 8)) % settled_every != 0) {
          ++settled_skips;
          return known.detector.last_hash();
        }
        return known.detector.CheckExact(data, bytes, vertex_hash_bytes);
      });
      if (const uint64_t hashed = vertex_hash_bytes - hashed_before) {
        // WORKLOG #89: where the hashed bytes go (index / small exact / ever-changed exact / sampled).
        hash_class_bytes[index ? 0 : bytes < sampled_min_bytes ? 1 : ever_changed ? 2 : 3] += hashed;
        ++hash_class_ranges[index ? 0 : bytes < sampled_min_bytes ? 1 : ever_changed ? 2 : 3];
      }
      known.frame = frame;
      known.hash = stamp.hash;
      known.epoch = stamp.epoch;
      known.frame_local = stamp.frame_local;
      if (first && stamp.frame_local) { dynamic_hash_bytes_skipped += bytes; ++dynamic_ranges; }
      return known.hash;
    }
    if (known.frame == frame) return known.hash;
    known.frame = frame;
    if (!exact_vertex_check && known.changes >= 4 && (frame & 15)) {
      known.hash = (frame + 1) * 0xD1B54A32D192ED03ull ^ (uint64_t(address) << 20) ^ bytes;
      return known.hash;
    }
    uint64_t content = exact_vertex_check
                           ? known.detector.CheckExact(data, bytes, vertex_hash_bytes)
                           : known.detector.Check(data, bytes, frame, address >> 4,
                                                  vertex_hash_bytes);
    known.changes = content != known.content ? std::min(known.changes + 1, 8u) : 0;
    known.content = content;
    known.hash = content;
    return content;
  }
  uint64_t vertex_hash_bytes = 0;
  uint64_t hash_class_bytes[4] = {}, hash_class_ranges[4] = {};
  uint64_t dynamic_hash_bytes_skipped = 0, dynamic_ranges = 0;
  uint64_t hash_bytes_reported = 0, skip_bytes_reported = 0, ranges_reported = 0;
  bool dynamic_buffer_upload = false;
  bool exact_vertex_check = true;
  bool sampled_stable_ranges = true;
  bool skin_write_log = true;
  uint64_t skin_log_fresh = 0, skin_log_same = 0, hash_bytes_at_report = 0;
  uint32_t sampled_min_bytes = 16384;
  uint32_t settled_every = 4, settled_frames = 300, settled_min_bytes = 1024;
  uint64_t settled_skips = 0, vertex_hash_prunes = 0;
  uint64_t skin_page_ranges = 0, skin_page_exact = 0, skin_page_unique = 0;
  bool skin_unique = true;
  uint32_t skin_page_window = 600;
  FlatMap64<VertexHash> vertex_hashes;

  // Swap.
  VkDescriptorSetLayout swap_layout_ramp = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_layout_source = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_layout_dest = VK_NULL_HANDLE;
  VkPipelineLayout swap_pipeline_layout = VK_NULL_HANDLE;
  VkPipeline swap_pipeline = VK_NULL_HANDLE;
  VkBuffer ramp_buffer = VK_NULL_HANDLE;
  VkDeviceMemory ramp_memory = VK_NULL_HANDLE;
  uint8_t* ramp_mapping = nullptr;
  VkBufferView ramp_views[kSubmissionSlots] = {};
  VkDescriptorPool swap_pool = VK_NULL_HANDLE;
  VkDescriptorSet swap_sets[kSubmissionSlots][3] = {};
  uint32_t gamma_ramp[256] = {};

  SpirvShaderTranslator::SystemConstants system_constants = {};

  struct DrawSetup {
    bool valid = false;
    uint64_t state_generation = 0;
    uint64_t fetch_generation = 0;
    uint64_t rt_generation = 0;
    VulkanShader* active_vs = nullptr;
    VulkanShader* active_ps = nullptr;
    uint32_t initiator_state = 0;
    uint32_t skip_reason = UINT32_MAX;
    VulkanShader* vs = nullptr;
    VulkanShader* ps = nullptr;
    bool primitive_polygonal = false;
    reg::RB_DEPTHCONTROL depth_control = {};
    bool convert_fan = false, convert_loop = false, strip = false, reset_enabled = false;
    draw_util::Scissor scissor = {};
    draw_util::ViewportInfo viewport = {};
    PassTargets targets;
    const PipelineLayout* layout = nullptr;
    uint32_t counts[4] = {};
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkViewport vp = {};
    float bias_constant = 0.0f, bias_slope = 0.0f;
    float blend_constants[4] = {};
    uint32_t stencil[6] = {};
  };
  DrawSetup setup;
  std::unordered_map<uint64_t, DrawSetup> setup_cache;
  uint64_t setup_cache_rt_generation = 0;
  // Counts setup rebuilds, so derived caches can tell a setup apart.
  uint64_t setup_generation = 0;
  struct SystemInputs {
    uint64_t setup_generation = ~0ull;
    uint64_t fetch_generation = 0;
    uint64_t index_generation = 0;
    uint32_t used_mask = 0;
  };
  SystemInputs system_inputs;
  struct RetiredRenderTarget {
    uint64_t frame;
    VkImage image;
    VkImageView view;
    VkImageView sample_view;
    VkImageView stencil_view;
    VkDeviceMemory memory;
  };
  std::vector<RetiredRenderTarget> retired_render_targets;
  // Staging for depth resolves (depth to buffer to color image).
  static constexpr VkDeviceSize kDepthScratchSize = 16u << 20;
  VkBuffer depth_scratch = VK_NULL_HANDLE;
  VkDeviceMemory depth_scratch_memory = VK_NULL_HANDLE;
  // Bumped whenever a render target is created, as setups hold pointers.
  uint64_t rt_generation = 1;
  bool BuildDrawSetup(uint32_t initiator_value);
  void TraceSetup(uint32_t reason);
  void TraceWaterLodSetup();
  // Diagnostic (HANDOFF section 60): which draws are skipped for having no targets.
  struct NoTargetKey {
    uint64_t vs, ps;
    uint32_t color_mask, depth_control, edram_mode, ps_writes;
    bool operator==(const NoTargetKey& o) const {
      return vs == o.vs && ps == o.ps && color_mask == o.color_mask &&
             depth_control == o.depth_control && edram_mode == o.edram_mode &&
             ps_writes == o.ps_writes;
    }
  };
  struct NoTargetKeyHash {
    size_t operator()(const NoTargetKey& k) const {
      return size_t(k.vs ^ (k.ps * 31) ^ (uint64_t(k.color_mask) << 7) ^ k.depth_control);
    }
  };
  std::unordered_map<NoTargetKey, uint64_t, NoTargetKeyHash> no_target_counts;
  std::chrono::steady_clock::time_point no_target_report{};
  uint32_t no_target_reports = 0;
  void CountNoTargetSkip();
  uint32_t last_initiator_value = 0;
  // Mercenaries 2 water diagnostic (p2_native_water_debug_cycle, HANDOFF section 57):
  // once the water appears, every 8 s the water draw switches mode and a screenshot is taken
  // 4 s in: 0 normal, 1 no depth/stencil test, 2 solid magenta pixel shader, 3 both. Two
  // cycles, then back to normal.
  uint32_t water_debug_mode = 0;
  uint32_t water_debug_step = 0;
  std::chrono::steady_clock::time_point water_debug_next{};
  bool water_debug_shot_taken = false;
  VkShaderModule water_debug_ps = VK_NULL_HANDLE;
  bool water_debug_ps_attempted = false;
  void UpdateWaterDebug();
  // Textures the traced (water) shader samples; resolves into them are traced too.
  std::unordered_set<uint32_t> water_texture_addresses;

  struct TextureBindings {
    bool valid = false;
    uint64_t frame = 0;
    uint64_t submission = 0;
    uint64_t views_epoch = 0;
    const VulkanShader* vs = nullptr;
    const VulkanShader* ps = nullptr;
    const PipelineLayout* layout = nullptr;
    uint32_t used_mask = 0;
    uint32_t slots = 0;
    VkDescriptorSet sets[2] = {};
    // Reusable in later frames through this one: no texture behind it is due
    // a content check before then. The textures are marked used on reuse.
    uint64_t valid_until = 0;
    uint64_t set_generation = 0;
    uint64_t answers_epoch = 0;
    // The views the sets hold, to check them alive when views_epoch moved.
    uint32_t view_count = 0;
    VkImageView views[24] = {};
    uint64_t view_ids[24] = {};
    uint32_t texture_count = 0;
    void* textures[24] = {};
  };
  FlatMap64<TextureBindings> tb_cache;

  // What each fetch slot last resolved to, this frame and submission: most
  // binding rebuilds change one or two slots of several.
  struct SlotViewEntry {
    uint64_t frame = 0;
    uint64_t submission = 0;
    uint64_t views_epoch = 0;
    uint64_t answers_epoch = 0;
    uint32_t words[6] = {};
    uint32_t variant = 0;
    VkImageView view = VK_NULL_HANDLE;
    void* texture = nullptr;
    uint64_t valid_until = 0;
  };
  SlotViewEntry slot_views[32][8];
  // Set by SlotView: the texture behind its answer and how long it holds.
  void* slot_texture = nullptr;
  uint64_t slot_valid_until = 0;
  struct SlotSamplerEntry {
    bool valid = false;
    uint32_t words[6] = {};
    uint32_t binding_bits = 0;
    VkSampler sampler = VK_NULL_HANDLE;
  };
  SlotSamplerEntry slot_samplers[32][4];

  VkImageView SlotView(uint32_t slot, xenos::FetchOpDimension dimension, bool is_signed) {
    const uint32_t* words = &(*regs)[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                     slot * 6];
    uint32_t variant = uint32_t(dimension) | (uint32_t(is_signed) << 2);
    SlotViewEntry& e = slot_views[slot][variant & 7];
    if (e.frame == frame && e.submission == submission_number &&
        e.views_epoch == textures.views_epoch() &&
        e.answers_epoch == textures.answers_epoch() && e.variant == variant &&
        !std::memcmp(e.words, words, sizeof(e.words))) {
      slot_texture = e.texture;
      slot_valid_until = e.valid_until;
      return e.view;
    }
    VkImageView view =
        textures.GetView(regs->GetTextureFetch(slot), dimension, is_signed, upload);
    e.texture = slot_texture = textures.last_texture();
    e.valid_until = slot_valid_until = textures.last_valid_until();
    e.frame = frame;
    e.submission = submission_number;
    e.views_epoch = textures.views_epoch();
    e.answers_epoch = textures.answers_epoch();
    e.variant = variant;
    std::memcpy(e.words, words, sizeof(e.words));
    e.view = view;
    return view;
  }

  VkSampler SlotSampler(const SpirvShader::SamplerBinding& binding) {
    uint32_t slot = binding.fetch_constant;
    const uint32_t* words = &(*regs)[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                     slot * 6];
    uint32_t bits = uint32_t(binding.mag_filter) | (uint32_t(binding.min_filter) << 2) |
                    (uint32_t(binding.mip_filter) << 4) |
                    (uint32_t(binding.aniso_filter) << 6);
    SlotSamplerEntry* entries = slot_samplers[slot];
    for (uint32_t i = 0; i < 4; ++i) {
      SlotSamplerEntry& e = entries[i];
      if (e.valid && e.binding_bits == bits && !std::memcmp(e.words, words, sizeof(e.words))) {
        return e.sampler;
      }
    }
    VkSampler sampler = textures.GetSampler(regs->GetTextureFetch(slot), binding);
    // Samplers are never destroyed, so these stay valid across frames.
    SlotSamplerEntry& e = entries[(bits ^ words[0]) & 3];
    e.valid = true;
    e.binding_bits = bits;
    std::memcpy(e.words, words, sizeof(e.words));
    e.sampler = sampler;
    return sampler;
  }
  uint64_t tb_cache_frame = 0;

  // The fetch constants as the shaders get them: the guest's, with vertex
  // buffer addresses pointing into the stream.
  std::array<uint32_t, 6 * 32> fetch_words = {};
  std::array<uint64_t, 96> fetch_resolved_frame = {};
  uint64_t fetch_words_frame = 0;
  uint64_t fetch_words_submission = 0;
  uint32_t last_fetch_words[6 * 32] = {};

  uint32_t bound_offsets[SpirvShaderTranslator::kConstantBufferCount] = {};
  bool bound_offsets_valid = false;
  VkDescriptorSet bound_texture_sets[2] = {};
  VkDeviceSize bound_index_offset = 0;
  VkIndexType bound_index_type = VK_INDEX_TYPE_UINT16;
  bool bound_index_valid = false;

  // The last upload of each constant buffer, reused by the next draws while
  // the registers behind it are unchanged (the ring counts register writes)
  // and the stream still holds it (same submission).
  struct ReusedUpload {
    uint64_t submission = 0;
    uint64_t generation = 0;
    const void* shader = nullptr;
    uint64_t frame = 0;
    VkDeviceSize offset = 0;
  };
  ReusedUpload last_fetch, last_vs_float, last_ps_float, last_bool_loop;
  SpirvShaderTranslator::SystemConstants last_system_constants = {};
  ReusedUpload last_system;
  bool Reusable(const ReusedUpload& u, uint64_t generation, const void* shader,
                bool per_frame = false) const {
    return u.submission == submission_number && u.generation == generation &&
           u.shader == shader && (!per_frame || u.frame == frame);
  }

  const VulkanDevice::Functions& dfn() const { return device->functions(); }
  // Not in ReXGlue's device function table.
  PFN_vkCmdClearDepthStencilImage vkCmdClearDepthStencilImage = nullptr;
  PFN_vkCmdResolveImage vkCmdResolveImage = nullptr;
  PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
  Submission& current() { return submissions[submission_number % kSubmissionSlots]; }
  // Waits for queued draws first: the recording thread and this one never
  // record into the command buffer at the same time.
  VkCommandBuffer cmd() {
    DrainRecorder();
    return current().main;
  }

  // --- Recording thread ------------------------------------------------------------
  // A draw's Vulkan calls (binds, dynamic state, the draw), decided on the
  // ring thread and made on the recording thread (p2_native_record_thread)
  // or straight away. In the busiest areas the driver's share of a draw was
  // about a quarter of the ring thread's time; on another core it overlaps
  // with the ring preparing the next draw. Everything else recorded into the
  // main command buffer goes through cmd(), which waits for the queue to
  // empty, and the texture uploads go into the setup command buffer, which
  // has a pool of its own so the two threads never share one.
  struct DrawCommand {
    VkCommandBuffer commands;
    uint32_t flags;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSet set_shared;
    VkDescriptorSet set_constants;
    uint32_t offsets[SpirvShaderTranslator::kConstantBufferCount];
    VkDescriptorSet texture_sets[2];
    VkViewport viewport;
    VkRect2D scissor;
    float bias_constant, bias_slope;
    float blend[4];
    uint32_t stencil[6];
    VkBuffer index_buffer;
    VkDeviceSize index_offset;
    VkIndexType index_type;
    uint32_t count;
    uint32_t occlusion_index;
  };
  enum : uint32_t {
    kDrawPipeline = 1u << 0,
    kDrawSharedSet = 1u << 1,
    kDrawConstants = 1u << 2,
    kDrawTexturesVertex = 1u << 3,
    kDrawTexturesPixel = 1u << 4,
    kDrawViewport = 1u << 5,
    kDrawScissor = 1u << 6,
    kDrawDepthBias = 1u << 7,
    kDrawBlend = 1u << 8,
    kDrawStencilCompare = 1u << 9,
    kDrawStencilWrite = 1u << 10,
    kDrawStencilReference = 1u << 11,
    kDrawIndexBuffer = 1u << 12,
    kDrawIndexed = 1u << 13,
    kDrawOcclusion = 1u << 14,
  };
  void ExecuteDrawCommand(const DrawCommand& d) const {
    VkCommandBuffer c = d.commands;
    const auto& f = device->functions();
    if (d.flags & kDrawPipeline) {
      f.vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline);
    }
    if (d.flags & kDrawSharedSet) {
      f.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout,
                                SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram, 1,
                                &d.set_shared, 0, nullptr);
    }
    if (d.flags & kDrawConstants) {
      f.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout,
                                SpirvShaderTranslator::kDescriptorSetConstants, 1,
                                &d.set_constants, SpirvShaderTranslator::kConstantBufferCount,
                                d.offsets);
    }
    if (d.flags & kDrawTexturesVertex) {
      f.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout,
                                SpirvShaderTranslator::kDescriptorSetTexturesVertex, 1,
                                &d.texture_sets[0], 0, nullptr);
    }
    if (d.flags & kDrawTexturesPixel) {
      f.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout,
                                SpirvShaderTranslator::kDescriptorSetTexturesPixel, 1,
                                &d.texture_sets[1], 0, nullptr);
    }
    if (d.flags & kDrawViewport) f.vkCmdSetViewport(c, 0, 1, &d.viewport);
    if (d.flags & kDrawScissor) f.vkCmdSetScissor(c, 0, 1, &d.scissor);
    if (d.flags & kDrawDepthBias) f.vkCmdSetDepthBias(c, d.bias_constant, 0.0f, d.bias_slope);
    if (d.flags & kDrawBlend) f.vkCmdSetBlendConstants(c, d.blend);
    if (d.flags & kDrawStencilCompare) {
      f.vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, d.stencil[0]);
      f.vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, d.stencil[1]);
    }
    if (d.flags & kDrawStencilWrite) {
      f.vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, d.stencil[2]);
      f.vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, d.stencil[3]);
    }
    if (d.flags & kDrawStencilReference) {
      f.vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, d.stencil[4]);
      f.vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, d.stencil[5]);
    }
    if (d.flags & kDrawIndexBuffer) {
      f.vkCmdBindIndexBuffer(c, d.index_buffer, d.index_offset, d.index_type);
    }
    if (d.flags & kDrawOcclusion) f.vkCmdBeginQuery(c, occlusion_pool, d.occlusion_index, occlusion_flags);
    if (d.flags & kDrawIndexed) {
      f.vkCmdDrawIndexed(c, d.count, 1, 0, 0, 0);
    } else {
      f.vkCmdDraw(c, d.count, 1, 0, 0);
    }
    if (d.flags & kDrawOcclusion) f.vkCmdEndQuery(c, occlusion_pool, d.occlusion_index);
  }
  static constexpr uint32_t kRecordQueueSize = 4096;
  BlockingRecordQueue<DrawCommand,kRecordQueueSize,32> record_work;
  BlockingRecordQueue<DrawCommand,kRecordQueueSize,32>::WaitStats record_reported{};
  std::thread record_thread;
  bool record_threaded = false;

  void WakeRecorder() { record_work.WakeConsumer(); }
  void RecordThread() {
    rex::thread::set_current_thread_name("GPU record");
#if defined(__SWITCH__)
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
    {
      const uint32_t host_mask = REXCVAR_GET(p2_host_core_mask) & 0xFu;
      const uint32_t mask = host_mask ? host_mask : 0x3u;
      svcSetThreadCoreMask(CUR_THREAD_HANDLE, __builtin_ctz(mask), mask);
    }
#endif
    while (const DrawCommand* draw=record_work.FrontOrWait()) {
      ExecuteDrawCommand(*draw);
      record_work.CompleteFront();
    }
  }
  void SubmitDraw(const DrawCommand& d) {
    if (!record_threaded) { ExecuteDrawCommand(d); return; }
    if (!record_work.Push(d)) REX_FATAL("Native draw recorder closed before submission");
  }
  void KickRecorder() {
    if (record_threaded && record_work.Pending()) WakeRecorder();
  }
  void DrainRecorder() {
  p2::gpuwait::Scope gpu_wait_scope(p2::gpuwait::RecorderDrain);
    if (!record_threaded || !record_work.Pending()) return;
    const uint64_t start=NowNs();
    record_work.Drain();
    frame_drain_wait_ns += NowNs()-start;
  }
  RendererStats& stats() { return owner->stats_; }

  // --- Setup ---------------------------------------------------------------------

  bool Initialize();
  void Shutdown();
  bool CreateSwapResources();

  // --- Submissions and the stream ----------------------------------------------

  // A frame is complete when every submission up to it is: a frame can be
  // split over several submissions (when the stream fills up), and counting it
  // complete when its first one finished freed textures and staging memory the
  // rest still used, an MMU fault that lost the GPU. The frame being recorded
  // is never complete.
  void UpdateCompletedFrame() {
    uint64_t oldest = frame;
    for (const Submission& other : submissions) {
      if (other.pending) oldest = std::min(oldest, other.frame);
    }
    if (oldest > 0) {
      completed_frame = std::max(completed_frame, oldest - 1);
    }
  }

  void Completed(Submission& s) {
    s.pending = false;
    UpdateCompletedFrame();
    CollectOcclusion(s);
    ProfileCollect(uint32_t(&s - submissions));
    if (s.timed && timestamps) {
      uint32_t slot = uint32_t(&s - submissions);
      uint64_t ticks[2] = {};
      if (Extra(device).vkGetQueryPoolResults(vk, timestamps, slot * 2, 2, sizeof(ticks), ticks,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
          ticks[1] > ticks[0]) {
        stats().gpu_ns += uint64_t(double(ticks[1] - ticks[0]) * timestamp_period);
        ++stats().gpu_submissions;
      }
      s.timed = false;
    }
  }

  uint64_t occlusion_abandoned = 0;
  void WriteOcclusionReady(uint32_t id) {
    QueryTracker::Result result;
    if (!occlusion.TakeReady(id, result)) return;
    auto* counts = memory->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(result.address);
    const uint32_t finished = rex::byte_swap(UINT32_C(0xFFFFFEED));
    // The game may have reused/destroyed the query while the GPU was working.
    // Do not overwrite a new begin counter or unrelated data.
    const bool awaiting = !result.superseded && counts && (counts->ZPass_A == finished || counts->ZPass_B == finished ||
                                     counts->ZFail_A == finished || counts->ZFail_B == finished);
    const uint32_t samples = result.fallback ? 1000u : result.samples;
    if (awaiting) {
      std::memset(counts, 0, sizeof(*counts));
      counts->Total_A = samples; counts->ZPass_A = samples;
    }
    if (++occlusion_completed <= 16 || occlusion_completed % 120 == 0) {
      REXLOG_INFO("[native-occlusion] query {:08X} samples={} fallback={} written={}",
                  result.address, samples, result.fallback, awaiting);
    }
  }
  void CollectOcclusion(Submission& s) {
    if (!occlusion_pool || !s.occlusion_count) return;
    const uint32_t first = uint32_t(&s - submissions) * kOcclusionPerSubmission;
    std::array<uint64_t,kOcclusionPerSubmission> samples{};
    // Completed() is reached only after this slot's fence signals. No WAIT_BIT
    // and no extra CPU/GPU wait: collect before resetting or reusing this pool range.
    const VkResult status = Extra(device).vkGetQueryPoolResults(vk, occlusion_pool, first,
      s.occlusion_count, sizeof(uint64_t) * s.occlusion_count, samples.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    for (uint32_t i=0;i<s.occlusion_count;++i) {
      const auto& owners=s.occlusion_owners[i];
      for(uint32_t j=0;j<owners.count;++j){
        const uint32_t id=owners.ids[j];
        occlusion.Complete(id, samples[i], status==VK_SUCCESS);
        WriteOcclusionReady(id);
      }
    }
    s.occlusion_count=0;
  }
  uint64_t occlusion_address_ends = 0, occlusion_overlaps = 0, occlusion_unmatched_ends = 0;
  uint64_t occlusion_demand_submits = 0, occlusion_poll_ns = 0;
  bool OcclusionEvent(uint32_t address, bool sentinel_end) {
    if (!occlusion_pool || !REXCVAR_GET(p2_native_occlusion_queries)) return false;
    // D3D Issue (822DB378): begin snapshot = base+32, end snapshot = base.
    // Match the command-stream addresses before inspecting mutable guest memory.
    const bool paired_end=occlusion.IsEndAddress(address);
    if (paired_end || sentinel_end) {
      const uint32_t id=occlusion.EndMatching(address);
      if(id==QueryTracker::invalid){++occlusion_unmatched_ends;return false;}
      if(!sentinel_end)++occlusion_address_ends;
      // No submission or fence scan here. Swap submits normally; an actual wait
      // or idle command producer flushes ended queries to guarantee GetData progress.
      WriteOcclusionReady(id);
      return true;
    }
    const uint32_t previous=occlusion.FindBegin(address);
    if(previous!=QueryTracker::invalid){
      // Re-issuing the same query abandons its old generation, not other objects.
      occlusion.FailRecord(previous);
      const uint32_t abandoned=occlusion.EndRecord(previous,address-32u);
      // Its pieces must still retire, but never publish an incomplete interval.
      // A new Begin below supersedes this record before completion is polled.
      if (++occlusion_abandoned <= 8 || occlusion_abandoned % 4096 == 0)
        REXLOG_WARN("[native-occlusion] unmatched query begin; abandoned previous query ({} so far), begin={:08X}",
                    occlusion_abandoned,address);
      (void)abandoned;
    }else if(occlusion.open_count())++occlusion_overlaps;
    const bool begun=occlusion.BeginOverlapping(address);
    if(begun){
      auto* counts=memory->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
      std::memset(counts,0,sizeof(*counts));
    }
    if(previous!=QueryTracker::invalid)WriteOcclusionReady(previous);
    return begun;
  }
  void AppendOcclusion(DrawCommand& draw) {
    if(!occlusion.open_count())return;
    Submission& s=current();
    if(s.occlusion_count>=kOcclusionPerSubmission){occlusion.FailOpen();return;}
    const auto owners=occlusion.AddOpenPiece();
    if(!owners.count)return;
    const uint32_t local=s.occlusion_count++;
    // One Vulkan query per draw, shared by every guest interval covering it.
    // Vulkan queries themselves never overlap in the command buffer.
    s.occlusion_owners[local]=owners;
    draw.occlusion_index=uint32_t(submission_number % kSubmissionSlots)*kOcclusionPerSubmission+local;
    draw.flags|=kDrawOcclusion;
  }
  void PollOcclusionQueries(bool demand) {
    if(!occlusion.pending())return;
    if(demand && recording){
      const auto& s=current();
      bool ended=false;
      for(uint32_t i=0;i<s.occlusion_count && !ended;++i)
        for(uint32_t j=0;j<s.occlusion_owners[i].count;++j)
          if(occlusion.IsEnded(s.occlusion_owners[i].ids[j])){ended=true;break;}
      if(ended){++occlusion_demand_submits;Submit();}
    }
    // Packet polling is frequent; avoid scanning all eight fences every 64 packets.
    const uint64_t now=NowNs();
    if(now-occlusion_poll_ns<1000000)return;
    occlusion_poll_ns=now;PollCompleted();
  }

  // This frame's waits and texture time (RendererStats::frame_*).
  uint64_t frame_upload_wait_ns = 0, frame_fence_wait_ns = 0, frame_drain_wait_ns = 0,
           frame_texture_ns = 0, frame_draw_count = 0, frame_stream_wait_ns = 0;
  static uint64_t NowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
  }

  void WaitSubmission(Submission& s) {
  p2::gpuwait::Scope gpu_wait_scope(p2::gpuwait::SlotWait);
    if (s.pending) {
      uint64_t start = NowNs();
      dfn().vkWaitForFences(vk, 1, &s.fence, VK_TRUE, UINT64_MAX);
      frame_fence_wait_ns += NowNs() - start;
      Completed(s);
    }
  }

  void PollCompleted() {
    for (Submission& s : submissions) {
      if (s.pending && dfn().vkGetFenceStatus(vk, s.fence) == VK_SUCCESS) {
        Completed(s);
      }
    }
    UpdateCompletedFrame();
  }

  void BeginRecording() {
    if (recording) {
      return;
    }
    ++submission_number;
    Submission& s = current();
    WaitSubmission(s);
    dfn().vkResetFences(vk, 1, &s.fence);
    dfn().vkResetCommandPool(vk, s.pool, 0);
    dfn().vkResetCommandPool(vk, s.main_pool, 0);
    for (VkDescriptorPool pool : s.descriptor_pools) {
      dfn().vkResetDescriptorPool(vk, pool, 0);
    }
    s.descriptor_pool_used = 0;
    s.number = submission_number;
    s.frame = frame;
    s.stream_oldest = stream_head;
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn().vkBeginCommandBuffer(s.setup, &begin);
    dfn().vkBeginCommandBuffer(s.main, &begin);
    if (timestamps) {
      uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
      dfn().vkCmdResetQueryPool(s.setup, timestamps, slot * 2, 2);
      Extra(device).vkCmdWriteTimestamp(s.setup, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, slot * 2);
      s.timed = true;
    }
    if (!null_images_initialized) {
      textures.InitializeNullImages(s.setup);
      null_images_initialized = true;
    }
    {
      uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
      profile_segments[slot].clear();
      profile_closed[slot] = false;
      if (profile_frame && profile_pool) {
        dfn().vkCmdResetQueryPool(s.setup, profile_pool, slot * kProfileQueries,
                                  kProfileQueries);
      }
    }
    // Everything the setup command buffer uploads is visible to the draws.
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                            VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    dfn().vkCmdPipelineBarrier(s.main, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
                               nullptr);
    if (occlusion_pool) {
      const uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
      dfn().vkCmdResetQueryPool(s.setup, occlusion_pool, slot * kOcclusionPerSubmission, kOcclusionPerSubmission);
      s.occlusion_count = 0;
    }
    recording = true;
    bound_pipeline = VK_NULL_HANDLE;
    bound_layout = VK_NULL_HANDLE;
    bound_static_sets = false;
    std::memset(&bound_viewport, 0, sizeof(bound_viewport));
    std::memset(&bound_scissor, 0, sizeof(bound_scissor));
    bound_depth_bias[0] = bound_depth_bias[1] = NAN;
    bound_offsets_valid = false;
    bound_texture_sets[0] = bound_texture_sets[1] = VK_NULL_HANDLE;
    bound_index_valid = false;
    for (float& f : bound_blend_constants) f = NAN;
    for (uint32_t& v : bound_stencil) v = ~0u;
  }

  void Submit() {
  p2::gpuwait::Scope gpu_wait_scope(p2::gpuwait::Submit);
    if (!recording) {
      return;
    }
    DrainRecorder();
    EndPass(PassEnd::Submit);
    // The setup command buffer copies staging data the upload worker fills.
    {
      uint64_t start = NowNs();
      p2::gpuwait::Scope gpu_upload_wait(p2::gpuwait::TextureUploads);
      textures.WaitForUploads();
      frame_upload_wait_ns += NowNs() - start;
    }
    // Submission boundaries order everything anyway.
    resolve_barrier_pending = false;
    Submission& s = current();
    {
      uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
      if (profile_pool && !profile_segments[slot].empty()) {
        Extra(device).vkCmdWriteTimestamp(s.main, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profile_pool,
                                  slot * kProfileQueries +
                                      uint32_t(profile_segments[slot].size()));
        profile_closed[slot] = true;
      }
    }
    if (s.timed) {
      Extra(device).vkCmdWriteTimestamp(s.main, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps,
                                uint32_t(submission_number % kSubmissionSlots) * 2 + 1);
    }
    dfn().vkEndCommandBuffer(s.setup);
    dfn().vkEndCommandBuffer(s.main);
    VkCommandBuffer buffers[] = {s.setup, s.main};
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 2;
    submit.pCommandBuffers = buffers;
    {
      auto queue = p2::gpuwait::Measure(p2::gpuwait::QueueLock,[&]{return device->AcquireQueue(device->queue_family_graphics_compute(), 0);});
      if (p2::gpuwait::Measure(p2::gpuwait::QueueSubmit,[&]{return dfn().vkQueueSubmit(queue.queue(), 1, &submit, s.fence);}) != VK_SUCCESS) {
        REXLOG_ERROR("[native] vkQueueSubmit failed");
      }
    }
    s.pending = true;
    recording = false;
    ++stats().submissions;
  }

  // Stream memory for this submission; stalls on older submissions if the
  // ring would run over data they still read.
  // The start of the stream's next lap. Not rex::align: that rounds with a
  // mask, right only for powers of two, and the stream is 160 MB. Wrapping
  // with it put allocations at arbitrary places, far enough ahead that the
  // stream looked full and the renderer waited for the GPU to finish all its
  // work (30-45 ms, every wrap), or across the end of the buffer.
  static uint64_t NextStreamLap(uint64_t position) {
    return (position / kStreamSize + 1) * kStreamSize;
  }

  uint8_t* Allocate(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset_out) {
    if (size > kStreamSize / 4) {
      return nullptr;
    }
    BeginRecording();
    // Allocations stay kStreamGuard short of the end, so a constant binding's
    // fixed range starting at any allocation stays inside the buffer.
    uint64_t start = rex::align(stream_head, uint64_t(alignment));
    if ((start % kStreamSize) + size > kStreamSize - kStreamGuard) {
      start = NextStreamLap(start);
    }
    uint64_t end = start + size;
    // The current submission itself must not wrap onto what it still reads.
    const uint64_t fence_before = frame_fence_wait_ns;
    if (end - current().stream_oldest > kStreamSize) {
      Submit();
      WaitSubmission(submissions[submission_number % kSubmissionSlots]);
      BeginRecording();
      start = rex::align(stream_head, uint64_t(alignment));
      if ((start % kStreamSize) + size > kStreamSize - kStreamGuard) {
        start = NextStreamLap(start);
      }
      end = start + size;
    }
    for (Submission& s : submissions) {
      if (&s != &current() && s.pending && end - s.stream_oldest > kStreamSize) {
        WaitSubmission(s);
      }
    }
    frame_stream_wait_ns += frame_fence_wait_ns - fence_before;
    stream_head = end;
    offset_out = VkDeviceSize(start % kStreamSize);
    return stream_mapping + offset_out;
  }

  VkDescriptorSet AllocatePersistentSet(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    while (true) {
      if (set_pool_used >= set_pools.size()) {
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 16384},
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 4096;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &pool) != VK_SUCCESS) {
          return VK_NULL_HANDLE;
        }
        set_pools.push_back(pool);
      }
      info.descriptorPool = set_pools[set_pool_used];
      if (dfn().vkAllocateDescriptorSets(vk, &info, &set) == VK_SUCCESS) {
        return set;
      }
      ++set_pool_used;
    }
  }

  // Forgets every cached set; the pools are freed once the GPU is done with
  // the frames that may use them.
  // Bumped when the cached texture sets are dropped: their pools are freed
  // once the GPU is done, so bindings kept from earlier frames must not name
  // them any more.
  uint64_t set_cache_generation = 1;
  void FlushSetCache() {
    ++set_cache_generation;
    texture_set_cache.clear();
    if (!set_pools.empty()) {
      retired_set_pools.push_back({frame, std::move(set_pools)});
      set_pools.clear();
    }
    set_pool_used = 0;
  }

  void FreeRetiredSetPools() {
    for (auto it = retired_set_pools.begin(); it != retired_set_pools.end();) {
      if (it->frame <= completed_frame) {
        for (VkDescriptorPool pool : it->pools) {
          dfn().vkDestroyDescriptorPool(vk, pool, nullptr);
        }
        it = retired_set_pools.erase(it);
      } else {
        ++it;
      }
    }
  }

  VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout) {
    Submission& s = current();
    VkDescriptorSetAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    while (true) {
      if (s.descriptor_pool_used >= s.descriptor_pools.size()) {
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 16384},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 4096;
        pool_info.poolSizeCount = 3;
        pool_info.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &pool) != VK_SUCCESS) {
          return VK_NULL_HANDLE;
        }
        s.descriptor_pools.push_back(pool);
      }
      info.descriptorPool = s.descriptor_pools[s.descriptor_pool_used];
      if (dfn().vkAllocateDescriptorSets(vk, &info, &set) == VK_SUCCESS) {
        return set;
      }
      ++s.descriptor_pool_used;
    }
  }

  // --- Shaders, layouts and pipelines ----------------------------------------------

  VkDescriptorSetLayout GetTextureSetLayout(bool vertex, uint32_t textures, uint32_t samplers);
  const PipelineLayout* GetPipelineLayout(uint32_t tex_vs, uint32_t samp_vs, uint32_t tex_ps,
                                          uint32_t samp_ps);
  bool EnsureTranslated(VulkanShader::VulkanTranslation* translation, bool* pending_out);
  void DrainTranslations();
  void TranslationThread();
  std::unique_ptr<SpirvShaderTranslator> worker_translator;
  // 1 = being translated on the worker, 2 = done.
  std::unordered_map<const void*, uint8_t> translation_state;
  std::thread translation_thread;
  std::mutex translation_mutex;
  std::condition_variable translation_cv;
  std::deque<VulkanShader::VulkanTranslation*> translation_jobs;
  std::vector<VulkanShader::VulkanTranslation*> translations_done;
  std::atomic<uint32_t> translations_done_count{0};
  bool translation_stop = false;
  struct PipelineJob {
    uint64_t hash = 0;
    PipelineKey key = {};
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule pixel = VK_NULL_HANDLE;
    VkShaderModule geometry = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    // Tessellated draws: the helper vertex and control stages; `vertex` is then the
    // guest shader translated as a domain shader (evaluation stage).
    VkShaderModule tess_vertex = VK_NULL_HANDLE;
    VkShaderModule tess_control = VK_NULL_HANDLE;
    uint32_t patch_points = 0;
  };
  VkPipeline GetPipeline(const PipelineKey& key, VulkanShader::VulkanTranslation* vs,
                         VulkanShader::VulkanTranslation* ps, VkShaderModule geometry,
                         VkPipelineLayout layout, bool* pending_out);
  VkPipeline CreatePipeline(const PipelineJob& job);
  void DrainCompletedPipelines();
  void PipelineThread();
  std::unordered_set<uint64_t> pending_pipelines;
  std::thread pipeline_thread;
  std::mutex pipeline_mutex;
  std::condition_variable pipeline_cv;
  std::deque<PipelineJob> pipeline_jobs;
  std::vector<std::pair<uint64_t, VkPipeline>> pipelines_completed;
  std::atomic<uint32_t> pipelines_completed_count{0};
  bool pipeline_stop = false;

  // Pipeline prewarm (p2_native_pipeline_prewarm, HANDOFF B:93), after masseffect-nx's
  // masseffect_native_pipelines_prewarm. The ring records every pipeline it creates (its key and
  // the microcode of its shaders); at the next start a low-priority thread translates those shaders
  // with translators of its own, publishes them, and creates the pipelines with CreatePipeline.
  // The ring adopts a published shader instead of making and translating a new one, and takes the
  // pipelines from a queue. Same SPIR-V, same pipeline function: the image does not change.
  struct PrewarmShader {
    uint64_t hash = 0;
    uint32_t type = 0;
    std::vector<uint32_t> ucode;  // native byte order
  };
  static constexpr size_t kPrewarmMaxKeys = 8192;
  bool prewarm_enabled = false;
  std::filesystem::path prewarm_path;
  // Ring only: the file's lists followed by this session's new entries.
  std::vector<PrewarmShader> prewarm_shader_list;
  std::unordered_set<uint64_t> prewarm_shader_hashes;
  std::vector<PipelineKey> prewarm_keys;
  std::unordered_set<uint64_t> prewarm_key_hashes;
  size_t prewarm_saved_keys = 0;
  std::thread prewarm_thread;
  std::atomic<bool> prewarm_stop{false};
  std::mutex prewarm_mutex;
  // Translated by the thread, waiting for the ring to load the same microcode.
  std::unordered_map<uint64_t, std::unique_ptr<VulkanShader>> prewarmed_shaders;
  std::atomic<bool> prewarm_published{false};
  std::vector<std::pair<uint64_t, VkPipeline>> prewarm_completed;
  std::atomic<uint32_t> prewarm_completed_count{0};
  std::thread prewarm_writer;
  std::atomic<bool> prewarm_writing{false};
  // Water texture dump (p2_native_water_dump, HANDOFF B:95): requested by Draw, copied
  // at the start of Swap, read back once that submission has completed.
  struct WaterDump {
    std::string name;  // file name in logs/
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t texel_bytes = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t submission = 0;
  };
  static uint32_t DumpTexelBytes(VkFormat format);
  // Records a copy of mip 0 / layer 0 (width x height from 0,0) into the current submission.
  bool RecordImageDump(VkImage image, VkFormat format, uint32_t width, uint32_t height,
                       const std::string& name);
  bool water_dump_enabled = false;
  // One-frame trace of the water simulation (p2_native_water_sim_trace, HANDOFF B:96).
  bool water_sim_trace_enabled = false;
  uint64_t water_sim_trace_frame = 0;  // the frame being traced, 0 = none
  uint32_t water_sim_trace_lines = 0;
  bool WaterSimTracing() const {
    return water_sim_trace_frame && frame == water_sim_trace_frame &&
           water_sim_trace_lines < (capture_all ? 6000u : post_trace_seconds ? 2000u : 600u);
  }
  // HANDOFF B:106 (p2_native_post_trace_seconds): the same trace, every N seconds.
  int32_t post_trace_seconds = 0;
  uint32_t post_trace_count = 0;
  std::chrono::steady_clock::time_point post_trace_next{};
  void ArmPostTrace();
  // HANDOFF B:107: on the second traced frame, images and constants of the depth edge blur
  // (PS 3E0AB1A6A7CD8CFB) and of the shadow receiver (PS 3C496074AFFA65E3).
  bool post_dumped_edge = false, post_dumped_shadow = false, post_dump_next_resolve = false;
  bool post_dumped_motion = false;
  bool post_dumped_fog = false;
  uint32_t post_dumped_receivers = 0, post_dumped_casters = 0;
  std::string post_caster_last;
  void PostDump(uint64_t ps_hash);
  std::string ConstantsText(uint32_t first, uint32_t count) const;
  std::string FetchText(uint32_t index) const;
  void TraceSimDraw(uint32_t draw_count);
  // Raw guest bytes of the simulation's input textures, saved once with the dump (HANDOFF B:97).
  bool water_raw_pending = false;
  bool packet_trace_request = false;
  // HANDOFF B:102 (p2_native_edram_depth_alias_clear).
  bool edram_depth_alias_clear = false;
  void AliasDepthClearToColor(xenos::MsaaSamples msaa_samples);
  // Step-by-step capture of one frame's small targets (HANDOFF B:101).
  uint64_t water_seq_frame = 0;
  uint32_t water_seq_count = 0;
  void WaterSequenceDump(uint32_t draw_count);
  void DumpWaterSimInputs();
  std::chrono::steady_clock::time_point water_dump_first{};
  std::vector<std::pair<uint32_t, uint32_t>> water_dump_requests;  // address, fetch
  std::unordered_set<uint32_t> water_dump_seen;
  std::vector<WaterDump> water_dumps;
  void RequestWaterDump();
  void RecordWaterDumps();
  void FinishWaterDumps(bool wait);
  void LoadPrewarm(const std::filesystem::path& directory, uint32_t title_id);
  void PrewarmThread(std::vector<PrewarmShader> list_shaders, std::vector<PipelineKey> keys);
  void DrainPrewarmed();
  void RecordPrewarm(const PipelineKey& key);
  void SavePrewarm();

  // --- Render targets ------------------------------------------------------------

  RenderTarget* GetRenderTarget(const RenderTargetKey& key, uint32_t min_height);
  enum class PassEnd : uint32_t { Submit, TargetCreate, TargetChange, Resolve,
                                 ClearBegin, ClearEnd, Present, Count };
  bool resolve_probe_enabled = false;
  bool binding_revalidate = false;
  native_probe::Histogram<> resolve_probe;
  uint64_t resolve_probe_last_ns = 0, resolve_probe_frames = 0;
  uint64_t probe_compute_ok = 0, probe_compute_unavailable = 0, probe_compute_descriptor_failed = 0;
  uint64_t probe_copy_specialized = 0, probe_copy_universal = 0;
  TextureCacheStats probe_texture_previous{};
  void ReportResolveProbe() {
    if (!resolve_probe_enabled) return;
    ++resolve_probe_frames;
    const uint64_t now = NowNs();
    if (!resolve_probe_last_ns) resolve_probe_last_ns = now;
    if (now - resolve_probe_last_ns < 10000000000ull) return;
    const auto& t = textures.stats();
    REXLOG_INFO("[native] RESOLVE_PROBE frames={} buckets={} overflow={} overflow_pixels={} "
                "compute_ok={} compute_unavailable={} compute_descriptor_failed={} "
                "texture_uploads={} texture_pending={} texture_replaced={} resolved_hits={} "
                "texture_unsupported={} copy_specialized={} copy_universal={}",
                resolve_probe_frames, resolve_probe.used, resolve_probe.overflow_count,
                resolve_probe.overflow_pixels, probe_compute_ok, probe_compute_unavailable,
                probe_compute_descriptor_failed, t.uploads - probe_texture_previous.uploads,
                t.pending - probe_texture_previous.pending, t.replaced - probe_texture_previous.replaced,
                t.resolved_hits - probe_texture_previous.resolved_hits,
                t.unsupported - probe_texture_previous.unsupported,
                probe_copy_specialized, probe_copy_universal);
    for (size_t i = 0; i < resolve_probe.used; ++i) {
      const auto& b = resolve_probe.buckets[i]; const auto& k = b.key;
      REXLOG_INFO("[native] RESOLVE_PATH path={} width={} height={} src_fmt={} dst_fmt={} "
                  "sx={} sy={} msaa={} scale_bits={} clamp={} address={} clear={} count={} pixels={}",
                  k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], k[8], k[9], k[10], k[11],
                  b.count, b.pixels);
    }
    resolve_probe.Reset(); resolve_probe_frames = 0; resolve_probe_last_ns = now;
    probe_compute_ok = probe_compute_unavailable = probe_compute_descriptor_failed = 0;
    probe_copy_specialized = probe_copy_universal = 0;
    probe_texture_previous = t;
  }
  bool work_stats_enabled = false;
  std::array<uint64_t, size_t(PassEnd::Count)> pass_end_counts{};
  uint64_t work_frames = 0, work_resolves = 0, work_resolve_pixels = 0;
  uint64_t work_sized_copies = 0, work_sized_pixels = 0, work_sized_empty = 0;
  uint64_t work_compute_copies = 0, work_compute_pixels = 0;
  uint64_t work_last_report_ns = 0;
  void CountSizedCopies(const ResolveDestination& dest) {
    if (!work_stats_enabled) return;
    if (dest.sized.empty()) ++work_sized_empty;
    for (const auto& sized : dest.sized) {
      ++work_sized_copies;
      work_sized_pixels += uint64_t(std::min(sized->width, dest.width)) *
                           std::min(sized->height, dest.height);
    }
  }
  void ReportWorkStats() {
    if (!work_stats_enabled) return;
    ++work_frames;
    const uint64_t now = NowNs();
    if (!work_last_report_ns) work_last_report_ns = now;
    if (now - work_last_report_ns < 10000000000ull) return;
    REXLOG_INFO("[native] work stats: {} frames; pass ends submit={} create={} target={} "
                "resolve={} clear_begin={} clear_end={} present={}; resolves={} pixels={}; "
                "sized copies={} pixels={} empty_calls={}; compute copies={} pixels={} "
                "compute barriers={}; atlas direct={}; staggered cascade draws={} resolves={}; "
                "prepass skipped={}; reflection skipped={}",
                work_frames, pass_end_counts[0], pass_end_counts[1], pass_end_counts[2],
                pass_end_counts[3], pass_end_counts[4], pass_end_counts[5], pass_end_counts[6],
                work_resolves, work_resolve_pixels, work_sized_copies, work_sized_pixels,
                work_sized_empty, work_compute_copies, work_compute_pixels, work_compute_copies * 2,
                work_atlas_direct, work_shadow_skipped_draws, work_shadow_skipped_resolves,
                work_prepass_skipped, work_reflection_skipped);
    if (record_threaded) {
      const auto q=record_work.GetWaitStats();
      REXLOG_INFO("[native] draw recorder waits: {} producer parks, {} consumer parks, {} completion notifications; periodic polls=0",
        q.producer_parks-record_reported.producer_parks,
        q.consumer_parks-record_reported.consumer_parks,
        q.completion_notifications-record_reported.completion_notifications);
      record_reported=q;
    }
    REXLOG_INFO("[native] buffer CPU scans: full_hash_bytes={} avoided_hash_bytes={} fresh_ranges={}",
                vertex_hash_bytes-hash_bytes_reported, dynamic_hash_bytes_skipped-skip_bytes_reported,
                dynamic_ranges-ranges_reported);
    hash_bytes_reported=vertex_hash_bytes;skip_bytes_reported=dynamic_hash_bytes_skipped;
    ranges_reported=dynamic_ranges;
    work_atlas_direct = 0;
    work_prepass_skipped = work_reflection_skipped = 0;
    work_shadow_skipped_draws = work_shadow_skipped_resolves = 0;
    pass_end_counts.fill(0);
    work_frames = work_resolves = work_resolve_pixels = 0;
    work_sized_copies = work_sized_pixels = work_sized_empty = 0;
    work_compute_copies = work_compute_pixels = 0;
    work_last_report_ns = now;
  }
  void EndPass(PassEnd reason) {
    if (in_pass) {
      if (work_stats_enabled) ++pass_end_counts[size_t(reason)];
      dfn().vkCmdEndRendering(cmd());
      in_pass = false;
    }
  }
  bool BeginPass(const PassTargets& targets);

  // --- Draws ---------------------------------------------------------------------

  void Draw(const uint32_t* immediate_indices, uint32_t immediate_index_words);
  void UpdateSystemConstants(bool primitive_polygonal, const draw_util::ViewportInfo& viewport,
                             uint32_t used_texture_mask, uint32_t vertex_index_endian_flags,
                             const VulkanShader* vs, const VulkanShader* ps);
  void Resolve();
  bool resolve_barrier_pending = false;
  void FlushResolveBarrier();
  void Swap(rex::ui::Presenter* presenter, uint32_t frontbuffer, uint32_t width, uint32_t height);
  void SaveDriverCache();
};

// --- Setup -------------------------------------------------------------------------

bool Renderer::Impl::Initialize() {
  device = provider->vulkan_device();
  vk = device->device();
  auto get_proc = device->vulkan_instance()->functions().vkGetDeviceProcAddr;
  vkCmdClearDepthStencilImage = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(
      get_proc(vk, "vkCmdClearDepthStencilImage"));
  vkCmdResolveImage =
      reinterpret_cast<PFN_vkCmdResolveImage>(get_proc(vk, "vkCmdResolveImage"));
  vkDeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(get_proc(vk, "vkDeviceWaitIdle"));
  if (!vkCmdClearDepthStencilImage || !vkCmdResolveImage || !vkDeviceWaitIdle) {
    REXLOG_ERROR("[native] Missing Vulkan device functions");
    return false;
  }
  const auto& properties = device->properties();
  if (!properties.dynamicRendering) {
    REXLOG_ERROR("[native] The native renderer needs dynamic rendering");
    return false;
  }
  specialize_signs = REXCVAR_GET(gpu_specialize_texture_signs);
  exact_vertex_check = REXCVAR_GET(p2_native_exact_vertex_check);
  dynamic_buffer_upload = REXCVAR_GET(p2_native_dynamic_buffer_upload);
  sampled_stable_ranges = REXCVAR_GET(p2_native_sampled_stable_ranges);
  skin_write_log = REXCVAR_GET(p2_skin_write_log);
  sampled_min_bytes = REXCVAR_GET(p2_native_sampled_min_bytes);
  settled_every = std::max<uint32_t>(1, REXCVAR_GET(p2_native_settled_check_every));
  settled_frames = REXCVAR_GET(p2_native_settled_frames);
  settled_min_bytes = REXCVAR_GET(p2_native_settled_min_bytes);
  skin_unique = REXCVAR_GET(p2_native_skin_unique);
  REXLOG_INFO("[native] dynamic buffer fresh uploads={}", dynamic_buffer_upload);
  work_stats_enabled = REXCVAR_GET(p2_native_work_stats);
  resolve_probe_enabled = REXCVAR_GET(p2_native_resolve_probe);
  binding_revalidate = REXCVAR_GET(p2_native_binding_revalidate);
  REXLOG_INFO("[native] binding revalidation={}", binding_revalidate);
  specialize_copy = REXCVAR_GET(p2_native_copy_specialization);
  REXLOG_INFO("[native] resolve probe={}", resolve_probe_enabled);
  REXLOG_INFO("[native] shader stats requested={} device_ready={}",
              REXCVAR_GET(p2_native_shader_stats),
              rex::ui::vulkan::switch_shader_stats_device.load(std::memory_order_acquire) == vk);
  multisample = REXCVAR_GET(p2_native_msaa);
  ubo_alignment = std::max<VkDeviceSize>(properties.minUniformBufferOffsetAlignment, 16);

  translator = std::make_unique<SpirvShaderTranslator>(SpirvShaderTranslator::Features(device),
                                                       true, true, false);
  translator->SetTextureSignSpecializationEnabled(specialize_signs);
  structured_jumps = REXCVAR_GET(p2_native_structured_jumps);
  translator->SetStructuredForwardJumpsEnabled(structured_jumps);
  REXLOG_INFO("[native] texture sign specialization={}, structured jumps={}", specialize_signs,
              structured_jumps);
  water_dump_enabled = REXCVAR_GET(p2_native_water_dump);
  edram_depth_alias_clear = REXCVAR_GET(p2_native_edram_depth_alias_clear);
  water_sim_trace_enabled = water_dump_enabled && REXCVAR_GET(p2_native_water_sim_trace);
  post_trace_seconds = std::max(0, int32_t(REXCVAR_GET(p2_native_post_trace_seconds)));

  VkShaderStageFlags guest_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                    (properties.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
  // Tessellated draws run the guest vertex shader as the evaluation stage, and the helper
  // stages read the system constants.
  tessellation_supported = properties.tessellationShader;
  if (tessellation_supported) {
    guest_stages |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  }

  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &layout_empty) != VK_SUCCESS) {
    return false;
  }
  // Set 0: the translator's shared memory, here the stream. With a small
  // maxStorageBufferRange the translator splits it into several bindings.
  shared_binding_count = 1u << SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
                             properties.maxStorageBufferRange);
  VkDescriptorSetLayoutBinding shared_binding = {};
  shared_binding.binding = 0;
  shared_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_binding.descriptorCount = shared_binding_count;
  shared_binding.stageFlags = guest_stages;
  layout_info.bindingCount = 1;
  layout_info.pBindings = &shared_binding;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &layout_shared) != VK_SUCCESS) {
    return false;
  }
  // Set 1: the five constant buffers, dynamic so one set serves every draw.
  VkDescriptorSetLayoutBinding constant_bindings[SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_bindings[i].binding = i;
    constant_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    constant_bindings[i].descriptorCount = 1;
    constant_bindings[i].stageFlags = guest_stages;
  }
  constant_bindings[SpirvShaderTranslator::kConstantBufferFloatVertex].stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT |
      (tessellation_supported ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : 0);
  constant_bindings[SpirvShaderTranslator::kConstantBufferFloatPixel].stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT;
  layout_info.bindingCount = SpirvShaderTranslator::kConstantBufferCount;
  layout_info.pBindings = constant_bindings;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &layout_constants) !=
      VK_SUCCESS) {
    return false;
  }

  // The stream.
  VkDeviceSize stream_allocation = 0;
  uint32_t stream_memory_type = 0;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, kStreamSize + kStreamSlack,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          rex::ui::vulkan::util::MemoryPurpose::kUpload, stream_buffer, stream_memory,
          &stream_memory_type, &stream_allocation)) {
    REXLOG_ERROR("[native] Could not create the {} MB stream buffer", kStreamSize >> 20);
    return false;
  }
  if (!(device->memory_types().host_coherent & (1u << stream_memory_type))) {
    REXLOG_WARN("[native] The stream memory is not host-coherent");
  }
  void* mapping = nullptr;
  if (dfn().vkMapMemory(vk, stream_memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    return false;
  }
  stream_mapping = static_cast<uint8_t*>(mapping);

  VkDescriptorPoolSize static_sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, SpirvShaderTranslator::kConstantBufferCount},
  };
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 2;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = static_sizes;
  if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &static_pool) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayout static_layouts[] = {layout_shared, layout_constants};
  VkDescriptorSet static_sets[2];
  VkDescriptorSetAllocateInfo allocate = {};
  allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate.descriptorPool = static_pool;
  allocate.descriptorSetCount = 2;
  allocate.pSetLayouts = static_layouts;
  if (dfn().vkAllocateDescriptorSets(vk, &allocate, static_sets) != VK_SUCCESS) {
    return false;
  }
  set_shared = static_sets[0];
  set_constants = static_sets[1];
  // With several shared memory bindings each covers a slice of guest memory;
  // the stream is far smaller than the first slice, so all of them can point at
  // the whole stream.
  std::vector<VkDescriptorBufferInfo> shared_infos(shared_binding_count);
  VkDeviceSize slice = VkDeviceSize(512u << 20) / shared_binding_count;
  for (uint32_t i = 0; i < shared_binding_count; ++i) {
    // Binding i serves the addresses from i * slice, so it starts there in
    // the stream (clamped for the slices past the end of the stream, which
    // are never addressed).
    VkDeviceSize offset = std::min(VkDeviceSize(i) * slice, kStreamSize - 256);
    offset &= ~(properties.minStorageBufferOffsetAlignment - 1);
    shared_infos[i].buffer = stream_buffer;
    shared_infos[i].offset = offset;
    shared_infos[i].range = kStreamSize + kStreamSlack - offset;
  }
  VkDescriptorBufferInfo constant_infos[SpirvShaderTranslator::kConstantBufferCount];
  VkDeviceSize constant_ranges[] = {sizeof(SpirvShaderTranslator::SystemConstants),
                                    kFloatConstantsBytes, kFloatConstantsBytes,
                                    kBoolLoopConstantsBytes, kFetchConstantsBytes};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_infos[i].buffer = stream_buffer;
    constant_infos[i].offset = 0;
    constant_infos[i].range = constant_ranges[i];
  }
  VkWriteDescriptorSet writes[2] = {};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = set_shared;
  writes[0].descriptorCount = shared_binding_count;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = shared_infos.data();
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = set_constants;
  writes[1].descriptorCount = SpirvShaderTranslator::kConstantBufferCount;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  writes[1].pBufferInfo = constant_infos;
  dfn().vkUpdateDescriptorSets(vk, 2, writes, 0, nullptr);

  // Submissions.
  for (Submission& s : submissions) {
    VkCommandPoolCreateInfo command_pool = {};
    command_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    command_pool.queueFamilyIndex = device->queue_family_graphics_compute();
    if (dfn().vkCreateCommandPool(vk, &command_pool, nullptr, &s.pool) != VK_SUCCESS) {
      return false;
    }
    if (dfn().vkCreateCommandPool(vk, &command_pool, nullptr, &s.main_pool) != VK_SUCCESS) {
      return false;
    }
    VkCommandBufferAllocateInfo buffers = {};
    buffers.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    buffers.commandPool = s.pool;
    buffers.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    buffers.commandBufferCount = 1;
    if (dfn().vkAllocateCommandBuffers(vk, &buffers, &s.setup) != VK_SUCCESS) {
      return false;
    }
    buffers.commandPool = s.main_pool;
    if (dfn().vkAllocateCommandBuffers(vk, &buffers, &s.main) != VK_SUCCESS) {
      return false;
    }
    VkFenceCreateInfo fence = {};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (dfn().vkCreateFence(vk, &fence, nullptr, &s.fence) != VK_SUCCESS) {
      return false;
    }
  }

  VkQueryPoolCreateInfo query_info = {};
  query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  query_info.queryCount = kSubmissionSlots * 2;
  if (Extra(device).timestampPeriod > 0.0f &&
      dfn().vkCreateQueryPool(vk, &query_info, nullptr, &timestamps) == VK_SUCCESS) {
    timestamp_period = Extra(device).timestampPeriod;
  } else {
    timestamps = VK_NULL_HANDLE;
  }

  if (REXCVAR_GET(p2_native_occlusion_queries)) {
    VkQueryPoolCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_OCCLUSION;
    info.queryCount = kSubmissionSlots * kOcclusionPerSubmission;
    if (dfn().vkCreateQueryPool(vk, &info, nullptr, &occlusion_pool) != VK_SUCCESS) {
      occlusion_pool=VK_NULL_HANDLE;
      REXLOG_WARN("[native-occlusion] query pool unavailable; retaining legacy fallback");
    } else {
      if(device->properties().occlusionQueryPrecise)occlusion_flags=VK_QUERY_CONTROL_PRECISE_BIT;
      REXLOG_INFO("[native-occlusion] GPU visibility queries enabled, precise={}", occlusion_flags!=0);
    }
  }

  VkPipelineCacheCreateInfo cache_info = {};
  cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  Extra(device).vkCreatePipelineCache(vk, &cache_info, nullptr, &driver_cache);

  if (!textures.Initialize(device, memory)) {
    return false;
  }
  upload.buffer = stream_buffer;
  upload.allocate = [this](size_t size, size_t alignment, VkDeviceSize& offset) {
    uint8_t* result = Allocate(size, alignment, offset);
    // An allocation may have started a new submission.
    upload.setup_commands = current().setup;
    upload.submission = submission_number;
    return result;
  };
  if (!CreateSwapResources()) {
    return false;
  }
  pipeline_thread = std::thread([this]() { PipelineThread(); });
  if (REXCVAR_GET(p2_native_record_thread)) {
    record_thread = std::thread([this]() { RecordThread(); });
    record_threaded = true;
  }
  worker_translator = std::make_unique<SpirvShaderTranslator>(
      SpirvShaderTranslator::Features(device), true, true, false);
  worker_translator->SetTextureSignSpecializationEnabled(specialize_signs);
  worker_translator->SetStructuredForwardJumpsEnabled(structured_jumps);
  translation_thread = std::thread([this]() { TranslationThread(); });
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, kDepthScratchSize,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, depth_scratch,
          depth_scratch_memory)) {
    depth_scratch = VK_NULL_HANDLE;
    REXLOG_WARN("[native] No scratch buffer for depth resolves");
  }
  if (depth_scratch && !CreateDepthConversion()) {
    REXLOG_WARN("[native] No depth resolve pipeline; depth resolves are not done");
    if (depth_convert_pipeline) dfn().vkDestroyPipeline(vk, depth_convert_pipeline, nullptr);
    depth_convert_pipeline = VK_NULL_HANDLE;
  }
  if (copy_color_pipeline) {
    textures.SetComputeCopy([this](VkCommandBuffer c, VkImageView source, VkImageView dest,
                                   int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t width,
                                   uint32_t height, VkFormat attachment_format) {
      return ComputeColorCopy(c, source, dest, sx, sy, dx, dy, width, height, 1, 1, 1.0f, 0,
                              attachment_format);
    });
  }
  if (REXCVAR_GET(p2_native_depth24)) {
    VkFormatProperties format_properties = {};
    device->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
        device->physical_device(), VK_FORMAT_D24_UNORM_S8_UINT, &format_properties);
    if ((format_properties.optimalTilingFeatures &
         (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) ==
            (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) &&
        depth_scratch && depth_convert_pipeline) {
      depth24 = true;
    } else {
      REXLOG_WARN("[native] 24-bit depth buffers unavailable; using 32-bit float");
    }
  }
  if (REXCVAR_GET(p2_native_gpu_profile) && timestamps) {
    VkQueryPoolCreateInfo profile_info = {};
    profile_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    profile_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    profile_info.queryCount = kSubmissionSlots * kProfileQueries;
    if (dfn().vkCreateQueryPool(vk, &profile_info, nullptr, &profile_pool) != VK_SUCCESS) {
      profile_pool = VK_NULL_HANDLE;
    }
    profile_draws = profile_pool && REXCVAR_GET(p2_native_gpu_profile_draws);
    profile_draw_kinds = profile_draws && REXCVAR_GET(p2_native_gpu_profile_draw_kinds);
  }
  // The default linear 256-entry gamma ramp (CommandProcessor::Initialize).
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    gamma_ramp[i] = value | (value << 10) | (value << 20);
  }
  REXLOG_INFO("[native] Renderer ready: {} MB stream, {} shared memory binding(s)",
              kStreamSize >> 20, shared_binding_count);
  return true;
}

bool Renderer::Impl::CreateDepthConversion() {
  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = 1;
  layout_info.pBindings = &binding;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &depth_convert_set_layout) !=
      VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) * 5};
  VkPipelineLayoutCreateInfo pipeline_layout = {};
  pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout.setLayoutCount = 1;
  pipeline_layout.pSetLayouts = &depth_convert_set_layout;
  pipeline_layout.pushConstantRangeCount = 1;
  pipeline_layout.pPushConstantRanges = &push;
  if (dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &depth_convert_layout) !=
      VK_SUCCESS) {
    return false;
  }
  depth_convert_pipeline = rex::ui::vulkan::util::CreateComputePipeline(
      device, depth_convert_layout, shaders::kDepthResolveCs, sizeof(shaders::kDepthResolveCs));
  if (!depth_convert_pipeline) {
    return false;
  }
  VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &size;
  if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &depth_convert_pool) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetAllocateInfo allocate = {};
  allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate.descriptorPool = depth_convert_pool;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &depth_convert_set_layout;
  if (dfn().vkAllocateDescriptorSets(vk, &allocate, &depth_convert_set) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorBufferInfo buffer = {depth_scratch, 0, kDepthScratchSize};
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = depth_convert_set;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &buffer;
  dfn().vkUpdateDescriptorSets(vk, 1, &write, 0, nullptr);

  VkDescriptorSetLayoutBinding copy_bindings[2] = {};
  copy_bindings[0].binding = 0;
  copy_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  copy_bindings[0].descriptorCount = 1;
  copy_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  copy_bindings[1] = copy_bindings[0];
  copy_bindings[1].binding = 1;
  copy_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  layout_info.bindingCount = 2;
  layout_info.pBindings = copy_bindings;
  VkPushConstantRange copy_push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) * 9};
  pipeline_layout.pSetLayouts = &depth_copy_set_layout;
  pipeline_layout.pPushConstantRanges = &copy_push;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &depth_copy_set_layout) ==
          VK_SUCCESS &&
      dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &depth_copy_layout) ==
          VK_SUCCESS) {
    depth_copy_pipeline = rex::ui::vulkan::util::CreateComputePipeline(
        device, depth_copy_layout, shaders::kDepthToFloatCs, sizeof(shaders::kDepthToFloatCs));
  }
  if (!depth_copy_pipeline) {
    REXLOG_WARN("[native] No direct depth resolve; resolving depth through a buffer");
  }
  shadow_atlas_direct = REXCVAR_GET(p2_native_shadow_atlas_direct);
  shadow_stagger = shadow_atlas_direct && REXCVAR_GET(p2_native_shadow_stagger);
  shadow_max_cascades = std::clamp<uint32_t>(REXCVAR_GET(p2_native_shadow_max_cascades), 1, 4);
  REXLOG_INFO("[native] shadow cascades drawn: {}", shadow_max_cascades);
  shadow_empty_lit = shadow_atlas_direct && REXCVAR_GET(p2_native_shadow_empty_lit);
  shadow_depth_clamp = shadow_atlas_direct && REXCVAR_GET(p2_native_shadow_depth_clamp);
  REXLOG_INFO("[native] shadow cascades depth clamp {}", shadow_depth_clamp);
  if (shadow_atlas_direct && REXCVAR_GET(p2_native_shadow_atlas_fragment)) {
    CreateAtlasFragmentPipeline();
  }
  fragment_copies = (REXCVAR_GET(p2_native_fragment_copies) || REXCVAR_GET(p2_native_copy_ab)) && EnsureFragmentSetLayout();
  if (fragment_copies) {
    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t) * 8};
    VkPipelineLayoutCreateInfo layout = {};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &atlas_fragment_set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (dfn().vkCreatePipelineLayout(vk, &layout, nullptr, &copy_fragment_layout) != VK_SUCCESS) {
      copy_fragment_layout = VK_NULL_HANDLE;
      fragment_copies = false;
    }
  }
  if (REXCVAR_GET(p2_native_copy_ab)) fragment_copies = false;
  REXLOG_INFO("[native] colour resolves by fragment pass {}", fragment_copies);
  REXLOG_INFO("[native] shadow atlas by fragment pass {}", atlas_fragment_pipeline != VK_NULL_HANDLE);
  REXLOG_INFO("[native] empty shadow map texels lit {}", shadow_empty_lit);
  REXLOG_INFO("[native] far shadow cascades on alternate frames {}", shadow_stagger);
  motion_blur_off = !REXCVAR_GET(p2_native_motion_blur);
  edge_blur_off = !REXCVAR_GET(p2_native_edge_blur);
  dof_off = !REXCVAR_GET(p2_native_dof);
  skip_depth_prepass = REXCVAR_GET(p2_native_skip_depth_prepass);
  reflection_half_rate = REXCVAR_GET(p2_native_reflection_half_rate);
  REXLOG_INFO("[native] skip depth prepass {}, water reflection on alternate frames {}",
              skip_depth_prepass, reflection_half_rate);
  {
    std::string list = REXCVAR_GET(p2_native_skip_ps);
    size_t start = 0;
    while (start < list.size()) {
      size_t end = list.find(',', start);
      if (end == std::string::npos) end = list.size();
      std::string item = list.substr(start, end - start);
      if (!item.empty()) {
        skip_ps.insert(std::strtoull(item.c_str(), nullptr, 16));
        REXLOG_INFO("[native] pixel shader {} not drawn (p2_native_skip_ps)", item);
      }
      start = end + 1;
    }
  }
  g_float7e3_as_r11g11b10 = REXCVAR_GET(p2_native_scene_r11g11b10);
  REXLOG_INFO("[native] HDR scene target {}",
              g_float7e3_as_r11g11b10 ? "R11G11B10 float (32 bpp)" : "RGBA16 float (64 bpp)");
  capture_combo = REXCVAR_GET(p2_native_capture_combo);
  auto_capture_draws = REXCVAR_GET(p2_native_auto_capture_draws);
  REXLOG_INFO("[native] game depth of field {}, capture combo (L3+R3) {}", dof_off ? "OFF" : "on",
              capture_combo ? "on" : "off");
  REXLOG_INFO("[native] game edge blur {}", edge_blur_off ? "OFF" : "on");
  REXLOG_INFO("[native] game motion blur {}", motion_blur_off ? "OFF" : "on");
  REXLOG_INFO("[native] shadow atlas direct={}", shadow_atlas_direct);
  if (REXCVAR_GET(p2_native_depth_words_direct)) {
    VkDescriptorSetLayoutBinding words_bindings[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
      words_bindings[i].binding = i;
      words_bindings[i].descriptorType =
          i < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      words_bindings[i].descriptorCount = 1;
      words_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    layout_info.bindingCount = 4;
    layout_info.pBindings = words_bindings;
    VkPushConstantRange words_push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) * 5};
    pipeline_layout.pSetLayouts = &depth_words_set_layout;
    pipeline_layout.pPushConstantRanges = &words_push;
    if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &depth_words_set_layout) ==
            VK_SUCCESS &&
        dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &depth_words_layout) ==
            VK_SUCCESS) {
      depth_words_pipeline = rex::ui::vulkan::util::CreateComputePipeline(
          device, depth_words_layout, shaders::kDepthWordsCs, sizeof(shaders::kDepthWordsCs));
    }
    REXLOG_INFO("[native] depth words direct={}", depth_words_pipeline != VK_NULL_HANDLE);
  }

  VkPhysicalDeviceFeatures features = {};
  device->vulkan_instance()->functions().vkGetPhysicalDeviceFeatures(device->physical_device(),
                                                                     &features);
  if (depth_copy_set_layout && features.shaderStorageImageWriteWithoutFormat) {
    VkPushConstantRange color_push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t) * 10};
    pipeline_layout.pSetLayouts = &depth_copy_set_layout;
    pipeline_layout.pPushConstantRanges = &color_push;
    if (dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &copy_color_layout) ==
        VK_SUCCESS) {
      copy_color_pipeline = rex::ui::vulkan::util::CreateComputePipeline(
          device, copy_color_layout, shaders::kCopyColorCs, sizeof(shaders::kCopyColorCs));
      if (copy_color_pipeline && specialize_copy) {
        const VkSpecializationMapEntry entry = {0, 0, sizeof(uint32_t)};
        for (uint32_t clamp = 0; clamp < copy_color_specialized.size(); ++clamp) {
          const VkSpecializationInfo info = {1, &entry, sizeof(clamp), &clamp};
          copy_color_specialized[clamp] = rex::ui::vulkan::util::CreateComputePipeline(
              device, copy_color_layout, shaders::kCopyColorSpecializedCs,
              sizeof(shaders::kCopyColorSpecializedCs), &info);
        }
      }
    }
  }
  REXLOG_INFO("[native] color copy specialization={} variants={}", specialize_copy,
              std::count_if(copy_color_specialized.begin(), copy_color_specialized.end(),
                            [](VkPipeline pipeline) { return pipeline != VK_NULL_HANDLE; }));
  if (!copy_color_pipeline) {
    REXLOG_WARN("[native] No compute color copy; resolving color with vkCmdCopyImage");
  }
  return true;
}

bool Renderer::Impl::DepthWordsDirect(VkCommandBuffer c, const RenderTarget& source,
                                      ResolveDestination& dest, uint32_t dest_base,
                                      uint32_t dest_width, uint32_t dest_height, int32_t x0,
                                      int32_t y0, uint32_t width, uint32_t height) {
  if (!width || !height || dest.format != VK_FORMAT_R32_SFLOAT) return false;
  // Created (or found) before anything is recorded, so a failure leaves no partial work.
  ResolveDestination* words = textures.GetResolveDestination(
      (dest_base & 0x1FFFFFFF) | TextureCache::kDepthWordsTag, dest_width, dest_height,
      VK_FORMAT_R8G8B8A8_UNORM, uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA), c);
  if (!words) return false;
  VkDescriptorSet set = AllocateSet(depth_words_set_layout);
  if (!set) return false;
  VkDescriptorImageInfo images[4] = {
      {VK_NULL_HANDLE, source.sample_view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, source.stencil_view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, dest.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, words->view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType =
        i < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  dfn().vkUpdateDescriptorSets(vk, 4, writes, 0, nullptr);
  // The same barriers as the direct depth copy in Resolve.
  VkMemoryBarrier to_compute = {
      VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
          VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(
      c,
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &to_compute, 0, nullptr, 0, nullptr);
  const bool unorm = source.format == VK_FORMAT_D24_UNORM_S8_UINT;
  uint32_t params[5] = {uint32_t(x0), uint32_t(y0), width, height, unorm ? 1u : 0u};
  dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_words_pipeline);
  dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_words_layout, 0, 1, &set,
                                0, nullptr);
  dfn().vkCmdPushConstants(c, depth_words_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params),
                           params);
  dfn().vkCmdDispatch(c, (width + 7) / 8, (height + 7) / 8, 1);
  VkMemoryBarrier after_compute = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(
      c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
      0, 1, &after_compute, 0, nullptr, 0, nullptr);
  textures.CopyToSized(*words, c);
  return true;
}

bool Renderer::Impl::ComputeColorCopy(VkCommandBuffer c, VkImageView source, VkImageView dest,
                                      int32_t sx, int32_t sy, int32_t dx, int32_t dy,
                                      uint32_t width, uint32_t height, uint32_t sample_x,
                                      uint32_t sample_y, float scale, uint32_t clamp_mode,
                                      VkFormat dest_format) {
  if (fragment_copies && dest_format != VK_FORMAT_UNDEFINED && width && height &&
      FragmentColorCopy(c, source, dest, dest_format, sx, sy, dx, dy, width, height, sample_x,
                        sample_y, scale, clamp_mode)) {
    return true;
  }
  if (!copy_color_pipeline || !width || !height) {
    if (resolve_probe_enabled) ++probe_compute_unavailable;
    return false;
  }
  VkDescriptorSet set = AllocateSet(depth_copy_set_layout);
  if (!set) {
    if (resolve_probe_enabled) ++probe_compute_descriptor_failed;
    return false;
  }
  if (resolve_probe_enabled) ++probe_compute_ok;
  if (work_stats_enabled) {
    ++work_compute_copies;
    work_compute_pixels += uint64_t(width) * height;
  }
  VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_GENERAL},
                                     {VK_NULL_HANDLE, dest, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType =
        i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  dfn().vkUpdateDescriptorSets(vk, 2, writes, 0, nullptr);
  // Earlier writes of the source (rendering, copies, compute) before the
  // reads, and earlier reads of the destination before the writes.
  VkMemoryBarrier before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                VK_ACCESS_SHADER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(
      c,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
  int32_t params[10] = {sx, sy, dx, dy, int32_t(width), int32_t(height),
                        int32_t(sample_x), int32_t(sample_y), 0, int32_t(clamp_mode)};
  std::memcpy(&params[8], &scale, sizeof(scale));
  VkPipeline copy_pipeline = copy_color_pipeline;
  if (specialize_copy && sample_x == 1 && sample_y == 1 &&
      clamp_mode < copy_color_specialized.size() && copy_color_specialized[clamp_mode]) {
    copy_pipeline = copy_color_specialized[clamp_mode];
  }
  if (resolve_probe_enabled) {
    if (copy_pipeline == copy_color_pipeline) ++probe_copy_universal;
    else ++probe_copy_specialized;
  }
  dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, copy_pipeline);
  dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, copy_color_layout, 0, 1, &set,
                                0, nullptr);
  dfn().vkCmdPushConstants(c, copy_color_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params),
                           params);
  dfn().vkCmdDispatch(c, (width + 7) / 8, (height + 7) / 8, 1);
  VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(
      c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      0, 1, &after, 0, nullptr, 0, nullptr);
  return true;
}

void Renderer::Impl::ProfileCollect(uint32_t slot) {
  auto& segments = profile_segments[slot];
  if (!profile_pool || !profile_closed[slot] || segments.empty()) {
    segments.clear();
    return;
  }
  profile_closed[slot] = false;
  std::vector<uint64_t> ticks(segments.size() + 1);
  if (Extra(device).vkGetQueryPoolResults(vk, profile_pool, slot * kProfileQueries, uint32_t(ticks.size()),
                                  ticks.size() * sizeof(uint64_t), ticks.data(), sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
    for (size_t i = 0; i < segments.size(); ++i) {
      if (ticks[i + 1] < ticks[i]) continue;
      double ns = double(ticks[i + 1] - ticks[i]) * timestamp_period;
      ProfileTotal& total = profile_totals[segments[i].key];
      total.ns += ns;
      ++total.count;
      total.draws += segments[i].draws;
      total.elements += segments[i].elements;
      profiled_ns += ns;
    }
  }
  segments.clear();
}

void Renderer::Impl::ProfileReport() {
  if (!profile_pool || !profile_frame) return;
  // One profiled frame in 16; report about every 10 seconds at 30 fps.
  if (++profiled_frames < 20) return;
  std::vector<std::pair<uint64_t, ProfileTotal>> sorted(profile_totals.begin(),
                                                        profile_totals.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
  double frames = double(profiled_frames);
  std::string line = fmt::format("[native] GPU by pass, ms per frame (sampled, as the driver "
                                 "reports it): total {:.1f}",
                                 profiled_ns / 1e6 / frames);
  auto target = [](uint64_t bits) -> std::string {
    if (bits == 0xFFFFFF) return "-";
    return fmt::format("t{}p{}{}", (bits >> 12) & 0xFFF, (bits >> 1) & 0x7FF,
                       (bits & 1) ? "f" : "");
  };
  for (size_t i = 0; i < sorted.size() && i < (profile_draws ? 30u : 14u); ++i) {
    uint64_t key = sorted[i].first;
    const ProfileTotal& t = sorted[i].second;
    std::string what;
    switch (key >> 60) {
      case kProfilePass:
        what = fmt::format("pass c={} d={}", target((key >> 24) & 0xFFFFFF), target(key & 0xFFFFFF));
        break;
      case kProfileResolve:
        what = fmt::format("resolve{} p{} {}x{}", ((key >> 56) & 1) ? " depth" : "",
                           (key >> 32) & 0x7FF, (key >> 16) & 0xFFFF, key & 0xFFFF);
        break;
      case kProfileDraw:
        if ((key >> 59) & 1) {
          what = fmt::format("draw kind{}{}{}{}{}{}", (key & kDrawPipeline) ? " pipeline" : "",
                             (key & kDrawSharedSet) ? " shared" : "",
                             (key & kDrawConstants) ? " consts" : "",
                             (key & kDrawTexturesVertex) ? " vtex" : "",
                             (key & kDrawTexturesPixel) ? " ptex" : "",
                             (key & kDrawIndexed) ? " indexed" : " direct");
          break;
        }
        // The low 30 bits of each ucode hash (match them against the end of the full hash).
        what = fmt::format("draw VS ..{:08X} PS ..{:08X}", (key >> 30) & 0x3FFFFFFF,
                           key & 0x3FFFFFFF);
        break;
      default:
        what = "other";
        break;
    }
    line += fmt::format(" | {}: {:.2f} ({:.1f}x, {:.0f} draws, {:.1f}k elements)", what,
                        t.ns / 1e6 / frames, double(t.count) / frames, double(t.draws) / frames,
                        double(t.elements) / 1e3 / frames);
  }
  REXLOG_INFO("{}", line);
  profile_totals.clear();
  profiled_frames = 0;
  profiled_ns = 0;
}

bool Renderer::Impl::CreateSwapResources() {
  // The same layout as VulkanCommandProcessor's apply-gamma compute pipeline:
  // set 0 the ramp (uniform texel buffer), set 1 the source (sampled image),
  // set 2 the destination (storage image), and the size as a push constant.
  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = 1;
  layout_info.pBindings = &binding;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &swap_layout_ramp) !=
      VK_SUCCESS) {
    return false;
  }
  binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &swap_layout_source) !=
      VK_SUCCESS) {
    return false;
  }
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &swap_layout_dest) !=
      VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayout set_layouts[] = {swap_layout_ramp, swap_layout_source, swap_layout_dest};
  VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) * 2};
  VkPipelineLayoutCreateInfo pipeline_layout = {};
  pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout.setLayoutCount = 3;
  pipeline_layout.pSetLayouts = set_layouts;
  pipeline_layout.pushConstantRangeCount = 1;
  pipeline_layout.pPushConstantRanges = &push;
  if (dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &swap_pipeline_layout) !=
      VK_SUCCESS) {
    return false;
  }
  swap_pipeline = rex::ui::vulkan::util::CreateComputePipeline(
      device, swap_pipeline_layout, shaders::apply_gamma_table_cs,
      sizeof(shaders::apply_gamma_table_cs));
  if (swap_pipeline == VK_NULL_HANDLE) {
    REXLOG_ERROR("[native] Could not create the swap pipeline");
    return false;
  }

  VkDeviceSize ramp_size = 256 * sizeof(uint32_t) * kSubmissionSlots;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, ramp_size, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
          rex::ui::vulkan::util::MemoryPurpose::kUpload, ramp_buffer, ramp_memory)) {
    return false;
  }
  void* mapping = nullptr;
  dfn().vkMapMemory(vk, ramp_memory, 0, VK_WHOLE_SIZE, 0, &mapping);
  ramp_mapping = static_cast<uint8_t*>(mapping);

  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, kSubmissionSlots},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSubmissionSlots},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSubmissionSlots},
  };
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 3 * kSubmissionSlots;
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = sizes;
  if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &swap_pool) != VK_SUCCESS) {
    return false;
  }
  for (uint32_t i = 0; i < kSubmissionSlots; ++i) {
    VkDescriptorSetAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = swap_pool;
    allocate.descriptorSetCount = 3;
    allocate.pSetLayouts = set_layouts;
    if (dfn().vkAllocateDescriptorSets(vk, &allocate, swap_sets[i]) != VK_SUCCESS) {
      return false;
    }
    VkBufferViewCreateInfo view = {};
    view.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    view.buffer = ramp_buffer;
    view.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    view.offset = 256 * sizeof(uint32_t) * i;
    view.range = 256 * sizeof(uint32_t);
    if (dfn().vkCreateBufferView(vk, &view, nullptr, &ramp_views[i]) != VK_SUCCESS) {
      return false;
    }
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = swap_sets[i][0];
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    write.pTexelBufferView = &ramp_views[i];
    dfn().vkUpdateDescriptorSets(vk, 1, &write, 0, nullptr);
  }
  return true;
}

void Renderer::Impl::Shutdown() {
  if (!device) {
    return;
  }
  if (record_thread.joinable()) {
    DrainRecorder();
    record_work.Close();
    record_thread.join();
    record_threaded = false;
  }
  if (driver_cache_writer.joinable()) {
    driver_cache_writer.join();
  }
  if (!water_dumps.empty()) {
    Submit();
    FinishWaterDumps(true);
  }
  if (prewarm_thread.joinable()) {
    prewarm_stop.store(true, std::memory_order_relaxed);
    prewarm_thread.join();
  }
  if (prewarm_writer.joinable()) {
    prewarm_writer.join();
  }
  DrainPrewarmed();
  if (translation_thread.joinable()) {
    {
      std::lock_guard<std::mutex> lock(translation_mutex);
      translation_stop = true;
    }
    translation_cv.notify_all();
    translation_thread.join();
  }
  if (pipeline_thread.joinable()) {
    {
      std::lock_guard<std::mutex> lock(pipeline_mutex);
      pipeline_stop = true;
    }
    pipeline_cv.notify_all();
    pipeline_thread.join();
  }
  DrainCompletedPipelines();
  Submit();
  for (Submission& s : submissions) {
    WaitSubmission(s);
  }
  vkDeviceWaitIdle(vk);
  textures.Shutdown();
  for (auto& [key, rt] : render_targets) {
    if (rt->sample_view) dfn().vkDestroyImageView(vk, rt->sample_view, nullptr);
    if (rt->stencil_view) dfn().vkDestroyImageView(vk, rt->stencil_view, nullptr);
    if (rt->view) dfn().vkDestroyImageView(vk, rt->view, nullptr);
    if (rt->image) dfn().vkDestroyImage(vk, rt->image, nullptr);
    if (rt->memory) dfn().vkFreeMemory(vk, rt->memory, nullptr);
  }
  render_targets.clear();
  for (auto& retired : retired_render_targets) {
    if (retired.sample_view) dfn().vkDestroyImageView(vk, retired.sample_view, nullptr);
    if (retired.stencil_view) dfn().vkDestroyImageView(vk, retired.stencil_view, nullptr);
    dfn().vkDestroyImageView(vk, retired.view, nullptr);
    dfn().vkDestroyImage(vk, retired.image, nullptr);
    dfn().vkFreeMemory(vk, retired.memory, nullptr);
  }
  retired_render_targets.clear();
  if (depth_convert_pipeline) dfn().vkDestroyPipeline(vk, depth_convert_pipeline, nullptr);
  if (depth_convert_layout) dfn().vkDestroyPipelineLayout(vk, depth_convert_layout, nullptr);
  if (depth_copy_pipeline) dfn().vkDestroyPipeline(vk, depth_copy_pipeline, nullptr);
  if (atlas_fragment_pipeline) dfn().vkDestroyPipeline(vk, atlas_fragment_pipeline, nullptr);
  for (auto& [format, pipeline] : copy_fragment_pipelines) {
    if (pipeline) dfn().vkDestroyPipeline(vk, pipeline, nullptr);
  }
  if (copy_fragment_layout) dfn().vkDestroyPipelineLayout(vk, copy_fragment_layout, nullptr);
  if (atlas_fragment_layout) dfn().vkDestroyPipelineLayout(vk, atlas_fragment_layout, nullptr);
  if (atlas_fragment_set_layout) {
    dfn().vkDestroyDescriptorSetLayout(vk, atlas_fragment_set_layout, nullptr);
  }
  if (depth_words_pipeline) dfn().vkDestroyPipeline(vk, depth_words_pipeline, nullptr);
  if (depth_words_layout) dfn().vkDestroyPipelineLayout(vk, depth_words_layout, nullptr);
  if (depth_words_set_layout) {
    dfn().vkDestroyDescriptorSetLayout(vk, depth_words_set_layout, nullptr);
  }
  for (VkPipeline pipeline : copy_color_specialized) {
    if (pipeline) dfn().vkDestroyPipeline(vk, pipeline, nullptr);
  }
  if (copy_color_pipeline) dfn().vkDestroyPipeline(vk, copy_color_pipeline, nullptr);
  if (copy_color_layout) dfn().vkDestroyPipelineLayout(vk, copy_color_layout, nullptr);
  if (depth_copy_layout) dfn().vkDestroyPipelineLayout(vk, depth_copy_layout, nullptr);
  if (depth_copy_set_layout) {
    dfn().vkDestroyDescriptorSetLayout(vk, depth_copy_set_layout, nullptr);
  }
  if (depth_convert_pool) dfn().vkDestroyDescriptorPool(vk, depth_convert_pool, nullptr);
  if (depth_convert_set_layout) {
    dfn().vkDestroyDescriptorSetLayout(vk, depth_convert_set_layout, nullptr);
  }
  if (profile_pool) dfn().vkDestroyQueryPool(vk, profile_pool, nullptr);
  if (depth_scratch) dfn().vkDestroyBuffer(vk, depth_scratch, nullptr);
  if (depth_scratch_memory) dfn().vkFreeMemory(vk, depth_scratch_memory, nullptr);
  for (auto& [key, pipeline] : pipelines) {
    if (pipeline) dfn().vkDestroyPipeline(vk, pipeline, nullptr);
  }
  pipelines.clear();
  for (auto& [key, module] : geometry_shaders) {
    if (module) dfn().vkDestroyShaderModule(vk, module, nullptr);
  }
  geometry_shaders.clear();
  for (auto& [key, layout] : pipeline_layouts) {
    dfn().vkDestroyPipelineLayout(vk, layout.layout, nullptr);
  }
  pipeline_layouts.clear();
  for (auto& [key, layout] : texture_set_layouts) {
    if (layout != layout_empty) dfn().vkDestroyDescriptorSetLayout(vk, layout, nullptr);
  }
  texture_set_layouts.clear();
  shaders.clear();
  prewarmed_shaders.clear();
  FlushSetCache();
  for (auto& retired : retired_set_pools) {
    for (VkDescriptorPool pool : retired.pools) dfn().vkDestroyDescriptorPool(vk, pool, nullptr);
  }
  retired_set_pools.clear();
  for (Submission& s : submissions) {
    for (VkDescriptorPool pool : s.descriptor_pools) {
      dfn().vkDestroyDescriptorPool(vk, pool, nullptr);
    }
    s.descriptor_pools.clear();
    if (s.fence) dfn().vkDestroyFence(vk, s.fence, nullptr);
    if (s.pool) dfn().vkDestroyCommandPool(vk, s.pool, nullptr);
    if (s.main_pool) dfn().vkDestroyCommandPool(vk, s.main_pool, nullptr);
    s = Submission();
  }
  for (VkBufferView view : ramp_views) {
    if (view) dfn().vkDestroyBufferView(vk, view, nullptr);
  }
  if (swap_pool) dfn().vkDestroyDescriptorPool(vk, swap_pool, nullptr);
  if (swap_pipeline) dfn().vkDestroyPipeline(vk, swap_pipeline, nullptr);
  if (swap_pipeline_layout) dfn().vkDestroyPipelineLayout(vk, swap_pipeline_layout, nullptr);
  for (VkDescriptorSetLayout l : {swap_layout_ramp, swap_layout_source, swap_layout_dest,
                                  layout_empty, layout_shared, layout_constants}) {
    if (l) dfn().vkDestroyDescriptorSetLayout(vk, l, nullptr);
  }
  if (ramp_buffer) dfn().vkDestroyBuffer(vk, ramp_buffer, nullptr);
  if (ramp_memory) dfn().vkFreeMemory(vk, ramp_memory, nullptr);
  if (static_pool) dfn().vkDestroyDescriptorPool(vk, static_pool, nullptr);
  if (stream_buffer) dfn().vkDestroyBuffer(vk, stream_buffer, nullptr);
  if (stream_memory) dfn().vkFreeMemory(vk, stream_memory, nullptr);
  if (driver_cache) Extra(device).vkDestroyPipelineCache(vk, driver_cache, nullptr);
  if (occlusion_pool) {dfn().vkDestroyQueryPool(vk, occlusion_pool, nullptr);occlusion_pool=VK_NULL_HANDLE;}
  if (timestamps) dfn().vkDestroyQueryPool(vk, timestamps, nullptr);
  device = nullptr;
}

// --- Layouts and pipelines -----------------------------------------------------------

VkDescriptorSetLayout Renderer::Impl::GetTextureSetLayout(bool vertex, uint32_t texture_count,
                                                          uint32_t sampler_count) {
  if (!texture_count && !sampler_count) {
    return layout_empty;
  }
  uint64_t key = (uint64_t(vertex) << 40) | (uint64_t(texture_count) << 20) | sampler_count;
  auto it = texture_set_layouts.find(key);
  if (it != texture_set_layouts.end()) {
    return it->second;
  }
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  VkShaderStageFlags stages =
      vertex ? VK_SHADER_STAGE_VERTEX_BIT |
                   (tessellation_supported ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : 0)
             : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (uint32_t i = 0; i < texture_count; ++i) {
    bindings.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr});
  }
  for (uint32_t i = 0; i < sampler_count; ++i) {
    bindings.push_back({texture_count + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, nullptr});
  }
  VkDescriptorSetLayoutCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  info.bindingCount = uint32_t(bindings.size());
  info.pBindings = bindings.data();
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  if (dfn().vkCreateDescriptorSetLayout(vk, &info, nullptr, &layout) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  texture_set_layouts.emplace(key, layout);
  return layout;
}

const PipelineLayout* Renderer::Impl::GetPipelineLayout(uint32_t tex_vs, uint32_t samp_vs,
                                                        uint32_t tex_ps, uint32_t samp_ps) {
  uint64_t key = (uint64_t(tex_vs) << 48) | (uint64_t(samp_vs) << 32) | (uint64_t(tex_ps) << 16) |
                 samp_ps;
  auto it = pipeline_layouts.find(key);
  if (it != pipeline_layouts.end()) {
    return &it->second;
  }
  PipelineLayout layout;
  layout.textures_vertex = GetTextureSetLayout(true, tex_vs, samp_vs);
  layout.textures_pixel = GetTextureSetLayout(false, tex_ps, samp_ps);
  if (!layout.textures_vertex || !layout.textures_pixel) {
    return nullptr;
  }
  VkDescriptorSetLayout sets[SpirvShaderTranslator::kDescriptorSetCount];
  sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = layout_shared;
  sets[SpirvShaderTranslator::kDescriptorSetConstants] = layout_constants;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = layout.textures_vertex;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = layout.textures_pixel;
  VkPipelineLayoutCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  info.setLayoutCount = SpirvShaderTranslator::kDescriptorSetCount;
  info.pSetLayouts = sets;
  if (dfn().vkCreatePipelineLayout(vk, &info, nullptr, &layout.layout) != VK_SUCCESS) {
    return nullptr;
  }
  return &pipeline_layouts.emplace(key, layout).first->second;
}

// Translation runs on one worker thread with a translator of its own. Being one
// thread matters: the first completed translation of a shader fills the
// shader's binding lists, and every later one leaves them alone, so a
// translation reported done means its shader's bindings are final.
bool Renderer::Impl::EnsureTranslated(VulkanShader::VulkanTranslation* translation,
                                      bool* pending_out) {
  *pending_out = false;
  DrainTranslations();
  auto state = translation_state.find(translation);
  if (state == translation_state.end()) {
    if (translation->is_translated()) {
      // Translated by the pipeline prewarm before the ring adopted its shader (HANDOFF B:93).
      translation_state[translation] = 2;
      return translation->is_valid() && translation->shader_module() != VK_NULL_HANDLE;
    }
    if (!translation->shader().is_ucode_analyzed()) {
      translation->shader().AnalyzeUcode(disasm);
    }
    if (!REXCVAR_GET(p2_native_async_pipelines) || !translation_thread.joinable()) {
      const uint64_t translate_start = NowNs();
      bool ok = TranslatorFor(*translation, false)->TranslateAnalyzedShader(*translation);
      if (ok) translation->GetOrCreateShaderModule();
      stats().translate_ns += NowNs() - translate_start;
      ++stats().shaders_translated;
      translation_state[translation] = 2;
      if (!ok) {
        REXLOG_ERROR("[native] Shader {:016X} did not translate",
                     translation->shader().ucode_data_hash());
        return false;
      }
    } else {
      translation_state[translation] = 1;
      {
        std::lock_guard<std::mutex> lock(translation_mutex);
        translation_jobs.push_back(translation);
      }
      translation_cv.notify_one();
      *pending_out = true;
      return false;
    }
  } else if (state->second == 1) {
    *pending_out = true;
    return false;
  }
  return translation->is_valid() && translation->shader_module() != VK_NULL_HANDLE;
}

void Renderer::Impl::DrainTranslations() {
  if (!translations_done_count.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<VulkanShader::VulkanTranslation*> done;
  {
    std::lock_guard<std::mutex> lock(translation_mutex);
    done.swap(translations_done);
    translations_done_count.store(0, std::memory_order_release);
  }
  for (auto* t : done) {
    translation_state[t] = 2;
    ++stats().shaders_translated;
  }
}

void Renderer::Impl::TranslationThread() {
  rex::thread::set_current_thread_name("GPU translate");
#if defined(__SWITCH__)
  const auto priority_result = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3D);
  REXLOG_INFO("[native] translate priority request 0x3D: result 0x{:08X}", priority_result);
#endif
  while (true) {
    VulkanShader::VulkanTranslation* translation;
    {
      std::unique_lock<std::mutex> lock(translation_mutex);
      translation_cv.wait(lock, [&] { return translation_stop || !translation_jobs.empty(); });
      if (translation_stop) return;
      translation = translation_jobs.front();
      translation_jobs.pop_front();
    }
    if (TranslatorFor(*translation, true)->TranslateAnalyzedShader(*translation)) {
      translation->GetOrCreateShaderModule();
    } else {
      REXLOG_ERROR("[native] Shader {:016X} did not translate",
                   translation->shader().ucode_data_hash());
    }
    std::lock_guard<std::mutex> lock(translation_mutex);
    translations_done.push_back(translation);
    translations_done_count.fetch_add(1, std::memory_order_release);
  }
}

VkPipeline Renderer::Impl::GetPipeline(const PipelineKey& key,
                                       VulkanShader::VulkanTranslation* vs,
                                       VulkanShader::VulkanTranslation* ps,
                                       VkShaderModule geometry, VkPipelineLayout layout,
                                       bool* pending_out) {
  *pending_out = false;
  DrainCompletedPipelines();
  DrainPrewarmed();
  uint64_t hash = XXH3_64bits(&key, sizeof(key));
  auto it = pipelines.find(hash);
  if (it != pipelines.end()) {
    return it->second;
  }
  if (pending_pipelines.count(hash)) {
    *pending_out = true;
    return VK_NULL_HANDLE;
  }
  PipelineJob job;
  job.hash = hash;
  job.key = key;
  job.vertex = vs->shader_module();
  job.pixel = ps ? ps->shader_module() : VK_NULL_HANDLE;
  if (key.tessellation_reserved == 1) {
    if (!water_debug_ps_attempted) {
      water_debug_ps_attempted = true;
      std::vector<uint32_t> spirv;
      if (tessellation::CompileFragmentShader(
              "#version 450\nlayout(location = 0) out vec4 c;\n"
              "void main() { c = vec4(1.0, 0.0, 1.0, 1.0); }\n",
              spirv)) {
        VkShaderModuleCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = spirv.size() * sizeof(uint32_t);
        info.pCode = spirv.data();
        dfn().vkCreateShaderModule(vk, &info, nullptr, &water_debug_ps);
      }
    }
    if (water_debug_ps) job.pixel = water_debug_ps;
  }
  job.geometry = geometry;
  job.layout = layout;
  if (key.tessellation) {
    const auto type = tessellation::HostType(key.tessellation & 0xFF);
    const auto mode = xenos::TessellationMode((key.tessellation >> 8) & 0xFF);
    job.tess_vertex = GetTessellationModule(false, type, mode);
    job.tess_control = GetTessellationModule(true, type, mode);
    job.patch_points = key.tessellation >> 16;
    if (!job.tess_vertex || !job.tess_control) {
      pipelines.emplace(hash, VK_NULL_HANDLE);
      return VK_NULL_HANDLE;
    }
  }
  if (REXCVAR_GET(p2_native_async_pipelines) && pipeline_thread.joinable()) {
    // Compiled on the worker. Until it is ready the draws that need it are
    // skipped, a frame or two of missing objects instead of a frozen frame.
    pending_pipelines.insert(hash);
    {
      std::lock_guard<std::mutex> lock(pipeline_mutex);
      pipeline_jobs.push_back(job);
    }
    pipeline_cv.notify_one();
    *pending_out = true;
    return VK_NULL_HANDLE;
  }
  const uint64_t pipeline_start = NowNs();
  VkPipeline pipeline = CreatePipeline(job);
  stats().pipeline_ns += NowNs() - pipeline_start;
  if (pipeline) {
    ++stats().pipelines_created;
    last_pipeline_time = std::chrono::steady_clock::now();
    RecordPrewarm(key);
  }
  pipelines.emplace(hash, pipeline);
  return pipeline;
}

void Renderer::Impl::DrainCompletedPipelines() {
  if (!pipelines_completed_count.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<std::pair<uint64_t, VkPipeline>> done;
  {
    std::lock_guard<std::mutex> lock(pipeline_mutex);
    done.swap(pipelines_completed);
    pipelines_completed_count.store(0, std::memory_order_release);
  }
  for (auto& [hash, pipeline] : done) {
    pending_pipelines.erase(hash);
    if (!pipelines.emplace(hash, pipeline).second) {
      if (pipeline) dfn().vkDestroyPipeline(vk, pipeline, nullptr);
      continue;
    }
    if (pipeline) {
      ++stats().pipelines_created;
      last_pipeline_time = std::chrono::steady_clock::now();
    }
  }
}

std::string Renderer::Impl::ConstantsText(uint32_t first, uint32_t count) const {
  const RegisterFile& r = *regs;
  std::string text;
  for (uint32_t c = first; c < first + count; ++c) {
    const uint32_t index = rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4;
    text += fmt::format(" c{}=({:g},{:g},{:g},{:g})", c >= 256 ? c - 256 : c,
                        r.Get<float>(index), r.Get<float>(index + 1), r.Get<float>(index + 2),
                        r.Get<float>(index + 3));
  }
  return text;
}

std::string Renderer::Impl::FetchText(uint32_t index) const {
  const auto fetch = (*regs).GetTextureFetch(index);
  uint32_t words[6];
  std::memcpy(words, &fetch, sizeof(words));
  return fmt::format(" tf{}={:08X},{:08X},{:08X},{:08X},{:08X},{:08X} swizzle {:03o} endian {}",
                     index, words[0], words[1], words[2], words[3], words[4], words[5],
                     uint32_t(fetch.swizzle), uint32_t(fetch.endianness));
}

void Renderer::Impl::PostDump(uint64_t ps_hash) {
  const RegisterFile& r = *regs;
  auto dump_fetch = [&](uint32_t index, const char* what) {
    const uint32_t address = (r.GetTextureFetch(index).base_address << 12) & 0x1FFFFFFF;
    if (const ResolveDestination* dest = textures.FindResolveDestination(address)) {
      RecordImageDump(dest->image, dest->format, dest->width, dest->height,
                      fmt::format("cap{}_{}_t{}_{:08X}", capture_count, what, index, address));
      for (const auto& sized : dest->sized) {
        RecordImageDump(sized->image, dest->format, sized->width, sized->height,
                        fmt::format("cap{}_{}_t{}_{:08X}_sized", capture_count, what, index,
                                    address));
      }
    }
    if (const ResolveDestination* words =
            textures.FindResolveDestination(address | TextureCache::kDepthWordsTag)) {
      RecordImageDump(words->image, words->format, words->width, words->height,
                      fmt::format("cap{}_{}_t{}_{:08X}_words", capture_count, what, index,
                                  address));
    }
  };
  if (ps_hash == UINT64_C(0x3E0AB1A6A7CD8CFB) && !post_dumped_edge) {
    post_dumped_edge = true;
    REXLOG_INFO("[native] POSTDUMP edge blur PS consts{} |{}{}{}",
                ConstantsText(256, 16) + ConstantsText(256 + 96, 4) +
                    ConstantsText(256 + 252, 4),
                FetchText(0), FetchText(1), "");
    dump_fetch(0, "edge");
    dump_fetch(1, "edge");
    post_dump_next_resolve = true;
  }
  if (ps_hash == kMotionBlurPs && !post_dumped_motion) {
    post_dumped_motion = true;
    REXLOG_INFO("[native] POSTDUMP motion blur PS consts{} |{}{}",
                ConstantsText(256, 3) + ConstantsText(256 + 132, 4) +
                    ConstantsText(256 + 246, 10),
                FetchText(6), FetchText(0));
    if (capture_all) {
      for (uint32_t index : {0u, 2u, 4u, 5u, 6u}) dump_fetch(index, "composite");
      post_dump_next_resolve = true;  // the composite's result
      REXLOG_INFO("[native] CAPTURE composite consts{} |{}{}{}{}",
                  ConstantsText(256 + 96, 4) + ConstantsText(256 + 3, 9), FetchText(2),
                  FetchText(4), FetchText(5), FetchText(1));
    } else {
      dump_fetch(6, "motion");
    }
  }
  if (capture_all && ps_hash == UINT64_C(0xE91970DAA18976C2) && !post_dumped_fog) {
    // HANDOFF B:113: the half-resolution cloud / fog layer drawn over the scene.
    post_dumped_fog = true;
    dump_fetch(0, "fog");
    REXLOG_INFO("[native] CAPTURE fog layer consts{} |{}", ConstantsText(256, 4) +
                ConstantsText(256 + 132, 4), FetchText(0));
  }
  // HANDOFF B:115: shadows vanish for the whole frame at some camera angles while the atlas and the
  // PS cascade constants stay the same; the receivers take their cascade index and fade from the VS
  // (o0.z = dp4(world, c5), o0.w = o0.z * c15.z + c15.w), so log the VS constants of the receivers.
  // B:116: run66 only logged the water reflection pass (pitch 320, drawn first); log the main pass
  // (any other pitch) and the shadow pass (pitch 1040, depth-only PS A4A965C1) as well.
  const uint32_t surface_pitch = uint32_t(r.Get<reg::RB_SURFACE_INFO>().surface_pitch);
  if (capture_all && post_dumped_receivers < 8 && surface_pitch != 320 &&
      (ps_hash == UINT64_C(0x28F3BB7339AA0FE1) || ps_hash == UINT64_C(0x3C496074AFFA65E3) ||
       ps_hash == UINT64_C(0x76DDF06E7DDCB9C9))) {
    ++post_dumped_receivers;
    REXLOG_INFO("[native] CAPTURE receiver PS {:016X} pitch {} VS consts{} | PS{}", ps_hash,
                surface_pitch, ConstantsText(0, 32),
                ConstantsText(256 + 9, 2) + ConstantsText(256 + 19, 21) +
                    ConstantsText(256 + 253, 3));
  }
  if (capture_all && post_dumped_casters < 12 && surface_pitch == 1040 &&
      ps_hash == UINT64_C(0xA4A965C189287B99)) {
    // One line per distinct constant set (each cascade sets its own light matrix).
    std::string text = ConstantsText(0, 12);
    if (text != post_caster_last) {
      post_caster_last = text;
      ++post_dumped_casters;
      REXLOG_INFO("[native] CAPTURE caster VS consts{}", text);
    }
  }
  if (ps_hash == UINT64_C(0x3C496074AFFA65E3) && !post_dumped_shadow) {
    post_dumped_shadow = true;
    REXLOG_INFO("[native] POSTDUMP shadow receiver PS consts{} |{}",
                ConstantsText(256, 40) + ConstantsText(256 + 90, 10) +
                    ConstantsText(256 + 252, 4),
                FetchText(15));
    dump_fetch(15, "shadow");
  }
}

bool Renderer::Impl::EnsureFragmentSetLayout() {
  if (atlas_fragment_set_layout) return true;
  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = 1;
  layout_info.pBindings = &binding;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &atlas_fragment_set_layout) !=
      VK_SUCCESS) {
    atlas_fragment_set_layout = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

VkPipeline Renderer::Impl::CreateFullscreenPipeline(VkPipelineLayout layout,
                                                    const uint32_t* fs_code, size_t fs_bytes,
                                                    VkFormat format,
                                                    VkColorComponentFlags write_mask) {
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkShaderModule vs = rex::ui::vulkan::util::CreateShaderModule(device, shaders::kFullscreenVs,
                                                                sizeof(shaders::kFullscreenVs));
  VkShaderModule fs = rex::ui::vulkan::util::CreateShaderModule(device, fs_code, fs_bytes);
  if (vs && fs) {
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment = {};
    attachment.colorWriteMask = write_mask;
    VkPipelineColorBlendStateCreateInfo blend = {};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering = {};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    if (dfn().vkCreateGraphicsPipelines(vk, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS) {
      pipeline = VK_NULL_HANDLE;
    }
  }
  if (vs) dfn().vkDestroyShaderModule(vk, vs, nullptr);
  if (fs) dfn().vkDestroyShaderModule(vk, fs, nullptr);
  return pipeline;
}

bool Renderer::Impl::CreateAtlasFragmentPipeline() {
  if (!EnsureFragmentSetLayout()) return false;
  VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t) * 5};
  VkPipelineLayoutCreateInfo pipeline_layout = {};
  pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout.setLayoutCount = 1;
  pipeline_layout.pSetLayouts = &atlas_fragment_set_layout;
  pipeline_layout.pushConstantRangeCount = 1;
  pipeline_layout.pPushConstantRanges = &push;
  if (dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &atlas_fragment_layout) !=
      VK_SUCCESS) {
    return false;
  }
  atlas_fragment_pipeline = CreateFullscreenPipeline(
      atlas_fragment_layout, shaders::kDepthToFloatFs, sizeof(shaders::kDepthToFloatFs),
      VK_FORMAT_R32_SFLOAT, VK_COLOR_COMPONENT_R_BIT);
  if (!atlas_fragment_pipeline) {
    REXLOG_WARN("[native] shadow atlas fragment pipeline not created; using compute");
  }
  return atlas_fragment_pipeline != VK_NULL_HANDLE;
}

VkPipeline Renderer::Impl::CopyFragmentPipeline(VkFormat format) {
  auto it = copy_fragment_pipelines.find(uint32_t(format));
  if (it != copy_fragment_pipelines.end()) return it->second;
  VkPipeline pipeline = CreateFullscreenPipeline(
      copy_fragment_layout, shaders::kCopyColorFs, sizeof(shaders::kCopyColorFs), format,
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
          VK_COLOR_COMPONENT_A_BIT);
  copy_fragment_pipelines.emplace(uint32_t(format), pipeline);
  REXLOG_INFO("[native] fragment copy pipeline for format {}: {}", uint32_t(format),
              pipeline ? "made" : "FAILED (compute stays)");
  return pipeline;
}

bool Renderer::Impl::FragmentColorCopy(VkCommandBuffer c, VkImageView source, VkImageView dest,
                                       VkFormat dest_format, int32_t sx, int32_t sy, int32_t dx,
                                       int32_t dy, uint32_t width, uint32_t height,
                                       uint32_t sample_x, uint32_t sample_y, float scale,
                                       uint32_t clamp_mode) {
  if (dx < 0 || dy < 0) return false;
  VkPipeline pipeline = CopyFragmentPipeline(dest_format);
  if (!pipeline) return false;
  VkDescriptorSet set = AllocateSet(atlas_fragment_set_layout);
  if (!set) return false;
  if (work_stats_enabled) {
    ++work_compute_copies;
    work_compute_pixels += uint64_t(width) * height;
  }
  VkDescriptorImageInfo image = {VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &image;
  dfn().vkUpdateDescriptorSets(vk, 1, &write, 0, nullptr);
  // Earlier writes of the source before the reads, earlier reads of the destination before the
  // writes.
  VkMemoryBarrier before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(
      c,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1,
      &before, 0, nullptr, 0, nullptr);
  VkRenderingAttachmentInfo color = {};
  color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  color.imageView = dest;
  color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  info.renderArea = {{dx, dy}, {width, height}};
  info.layerCount = 1;
  info.colorAttachmentCount = 1;
  info.pColorAttachments = &color;
  dfn().vkCmdBeginRendering(c, &info);
  dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, copy_fragment_layout, 0, 1,
                                &set, 0, nullptr);
  int32_t params[8] = {sx, sy, dx, dy, int32_t(sample_x), int32_t(sample_y), 0,
                       int32_t(clamp_mode)};
  std::memcpy(&params[6], &scale, sizeof(scale));
  dfn().vkCmdPushConstants(c, copy_fragment_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(params), params);
  VkViewport viewport = {float(dx), float(dy), float(width), float(height), 0.0f, 1.0f};
  dfn().vkCmdSetViewport(c, 0, 1, &viewport);
  dfn().vkCmdSetScissor(c, 0, 1, &info.renderArea);
  dfn().vkCmdDraw(c, 3, 1, 0, 0);
  dfn().vkCmdEndRendering(c);
  ForgetBoundGraphicsState();
  VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(
      c, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      0, 1, &after, 0, nullptr, 0, nullptr);
  return true;
}

bool Renderer::Impl::AtlasFragmentPass(VkCommandBuffer c, const RenderTarget& source,
                                       ResolveDestination::Sized& atlas, int32_t x0, int32_t y0,
                                       uint32_t width, uint32_t height, uint32_t row) {
  if (!atlas.storage_view || row + height > atlas.height || width > atlas.width) return false;
  VkDescriptorSet set = AllocateSet(atlas_fragment_set_layout);
  if (!set) return false;
  VkDescriptorImageInfo image = {VK_NULL_HANDLE, source.sample_view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &image;
  dfn().vkUpdateDescriptorSets(vk, 1, &write, 0, nullptr);
  // The depth the shadow pass wrote, and the main pass's reads of the atlas in the previous frame.
  VkMemoryBarrier before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(
      c,
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1,
      &before, 0, nullptr, 0, nullptr);
  VkRenderingAttachmentInfo color = {};
  color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  color.imageView = atlas.storage_view;
  color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  info.renderArea = {{0, int32_t(row)}, {width, height}};
  info.layerCount = 1;
  info.colorAttachmentCount = 1;
  info.pColorAttachments = &color;
  dfn().vkCmdBeginRendering(c, &info);
  dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, atlas_fragment_pipeline);
  dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, atlas_fragment_layout, 0, 1,
                                &set, 0, nullptr);
  const int32_t params[5] = {x0, y0, 0, int32_t(row), shadow_empty_lit ? 1 : 0};
  dfn().vkCmdPushConstants(c, atlas_fragment_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(params), params);
  VkViewport viewport = {0.0f, float(row), float(width), float(height), 0.0f, 1.0f};
  dfn().vkCmdSetViewport(c, 0, 1, &viewport);
  dfn().vkCmdSetScissor(c, 0, 1, &info.renderArea);
  dfn().vkCmdDraw(c, 3, 1, 0, 0);
  dfn().vkCmdEndRendering(c);
  ForgetBoundGraphicsState();
  // Readers: the main pass's shadow lookups, and the next cascade drawn into the source.
  VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(
      c, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0, 1, &after, 0, nullptr, 0, nullptr);
  return true;
}

void Renderer::Impl::PollCaptureCombo() {
#if defined(__SWITCH__)
  if (capture_count >= 6) return;
  uint64_t buttons = 0;
  HidNpadHandheldState handheld = {};
  if (hidGetNpadStatesHandheld(HidNpadIdType_Handheld, &handheld, 1)) {
    buttons |= handheld.buttons;
  }
  HidNpadFullKeyState full = {};
  if (hidGetNpadStatesFullKey(HidNpadIdType_No1, &full, 1)) buttons |= full.buttons;
  HidNpadJoyDualState dual = {};
  if (hidGetNpadStatesJoyDual(HidNpadIdType_No1, &dual, 1)) buttons |= dual.buttons;
  const uint64_t combo = HidNpadButton_StickL | HidNpadButton_StickR;
  const auto now = std::chrono::steady_clock::now();
  if ((buttons & combo) != combo) {
    capture_held_since = {};
    capture_latched = false;
    return;
  }
  if (capture_latched) return;
  capture_held_since = now;
  capture_latched = true;
  ArmCapture("L3+R3");
#endif
}

void Renderer::Impl::ArmCapture(const char* why) {
  ++capture_count;
  capture_all = true;
  capture_resolve_dumps = 0;
  post_dumped_edge = post_dumped_shadow = post_dumped_motion = post_dumped_fog = false;
  post_dumped_receivers = post_dumped_casters = 0;
  post_caster_last.clear();
  water_sim_trace_frame = frame + 1;
  water_sim_trace_lines = 0;
  REXLOG_INFO("[native] CAPTURE {} armed for frame {} ({})", capture_count,
              water_sim_trace_frame, why);
}

void Renderer::Impl::ArmPostTrace() {
  const auto now = std::chrono::steady_clock::now();
  if (post_trace_next == std::chrono::steady_clock::time_point{}) {
    post_trace_next = now + std::chrono::seconds(post_trace_seconds);
    REXLOG_INFO("[native] post trace: level started, a frame traced every {} s",
                post_trace_seconds);
    return;
  }
  if (now < post_trace_next || water_sim_trace_frame > frame) return;
  post_trace_next = now + std::chrono::seconds(post_trace_seconds);
  water_sim_trace_frame = frame + 1;
  water_sim_trace_lines = 0;
  ++post_trace_count;
  REXLOG_INFO("[native] SIMTRACE armed for frame {} (post trace {} of 6)", water_sim_trace_frame,
              post_trace_count);
}

void Renderer::Impl::TraceSimDraw(uint32_t draw_count) {
  const DrawSetup& st = setup;
  bool small = false;
  for (const RenderTarget* rt : st.targets.color) {
    if (rt && rt->width <= 320) small = true;
  }
  if (st.targets.depth && st.targets.depth->width <= 320) small = true;
  // The game's clear shader (screen-space rectangle), whatever the target (HANDOFF B:107).
  if (!small && post_trace_seconds && st.active_vs &&
      st.active_vs->ucode_data_hash() == UINT64_C(0x1E6883FCCDE1F688)) {
    small = true;
  }
  if (capture_all) small = true;
  if (!small && post_trace_seconds) {
    for (const VulkanShader* shader : {st.active_vs, st.active_ps}) {
      if (!shader) continue;
      for (const auto& b : shader->GetTextureBindingsAfterTranslation()) {
        auto fetch = (*regs).GetTextureFetch(b.fetch_constant);
        if (textures.FindResolveDestination((fetch.base_address << 12) & 0x1FFFFFFF)) small = true;
      }
    }
  }
  if (!small) return;
  ++water_sim_trace_lines;
  const RegisterFile& r = *regs;
  auto surface = r.Get<reg::RB_SURFACE_INFO>();
  std::string targets;
  for (uint32_t i = 0; i < 4; ++i) {
    if (const RenderTarget* rt = st.targets.color[i]) {
      targets += fmt::format(" C{}:fmt{}@{}/{}x{}", i, rt->key.format, rt->key.base_tiles,
                             rt->width, rt->height);
      // Prototype 2: the target's exponent bias (shader output * 2^bias on the console).
      targets += fmt::format("{{x{}}}", int32_t(r.Get<reg::RB_COLOR_INFO>(
                                            reg::RB_COLOR_INFO::rt_register_indices[i])
                                            .color_exp_bias));
    }
  }
  if (st.targets.depth) {
    targets += fmt::format(" D:fmt{}@{}/{}x{}", st.targets.depth->key.format,
                           st.targets.depth->key.base_tiles, st.targets.depth->width,
                           st.targets.depth->height);
  }
  std::string fetches;
  for (const VulkanShader* shader : {st.active_vs, st.active_ps}) {
    if (!shader) continue;
    for (const auto& b : shader->GetTextureBindingsAfterTranslation()) {
      auto fetch = r.GetTextureFetch(b.fetch_constant);
      fetches += fmt::format(" {}t{}:{:08X}/{}x{}f{}", shader == st.active_vs ? "V" : "P",
                             b.fetch_constant, fetch.base_address << 12,
                             fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                             uint32_t(fetch.format));
      // Prototype 2: how the fetch reads it - exponent adjust, number format (0 fraction,
      // 1 integer), signs XYZW, swizzle, dimension.
      fetches += fmt::format("{{e{} n{} s{}{}{}{} w{:03X} d{}}}", int32_t(fetch.exp_adjust),
                             uint32_t(fetch.num_format), uint32_t(fetch.sign_x),
                             uint32_t(fetch.sign_y), uint32_t(fetch.sign_z),
                             uint32_t(fetch.sign_w), uint32_t(fetch.swizzle),
                             uint32_t(fetch.dimension));
      // A resolve destination: its size and how many frames ago it was last written (0 = this
      // frame, so a value above 0 means the draw reads an older frame's picture).
      if (const ResolveDestination* rd =
              textures.FindResolveDestination((fetch.base_address << 12) & 0x1FFFFFFF)) {
        fetches += fmt::format("[R{}x{} age{}]", rd->width, rd->height,
                               frame >= rd->last_written_frame ? frame - rd->last_written_frame
                                                               : 0);
      }
    }
  }
  // Prototype 2: the primitive and the vertex streams (format, stride, first words), for draws
  // whose geometry comes out wrong (the tonemap and sky grids, run11).
  {
    // The current draw's initiator (last_initiator_value is only set when a setup is built,
    // so it is stale for draws that reuse a cached setup).
    auto initiator = r.Get<reg::VGT_DRAW_INITIATOR>();
    fetches += fmt::format(" | prim {} src {} isz {}", uint32_t(initiator.prim_type),
                           uint32_t(initiator.source_select), uint32_t(initiator.index_size));
    if (st.active_vs) {
      for (const auto& binding : st.active_vs->vertex_bindings()) {
        const auto vf = r.GetVertexFetch(binding.fetch_constant);
        const uint32_t address = (uint32_t(vf.address) << 2) & 0x1FFFFFFF;
        fetches += fmt::format(" vf{}:{:08X}/{}w stride{} e{} [", binding.fetch_constant, address,
                               uint32_t(vf.size), binding.stride_words, uint32_t(vf.endian));
        for (const auto& a : binding.attributes) {
          fetches += fmt::format("o{}f{}{}{} ", a.fetch_instr.attributes.offset,
                                 uint32_t(a.fetch_instr.attributes.data_format),
                                 a.fetch_instr.attributes.is_signed ? "s" : "",
                                 a.fetch_instr.attributes.is_integer ? "i" : "");
        }
        fetches += "] words";
        if (address && address + 96u <= 0x20000000u) {
          const uint32_t* w = memory->TranslatePhysical<const uint32_t*>(address);
          for (uint32_t i = 0; i < std::min<uint32_t>(binding.stride_words * 3, 24); ++i) {
            fetches += fmt::format(" {:08X}", __builtin_bswap32(w[i]));
          }
        }
      }
    }
  }
  REXLOG_INFO("[native] SIMTRACE draw VS {:016X} PS {:016X} | {} elements | msaa {} pitch {} | "
              "guest vport scale {:.2f},{:.2f} offset {:.2f},{:.2f} vte {:08X} | window scissor "
              "{:08X}-{:08X} screen {:08X}-{:08X} | host scissor {},{} {}x{} viewport {},{} {}x{} | "
              "mask {:04X} depthctl {:08X} clip {:08X} vz {:g},{:g} clamp {} |{} |{}",
              st.active_vs ? st.active_vs->ucode_data_hash() : 0,
              st.active_ps ? st.active_ps->ucode_data_hash() : 0, draw_count,
              uint32_t(surface.msaa_samples), uint32_t(surface.surface_pitch),
              r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_XSCALE),
              r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_YSCALE),
              r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_XOFFSET),
              r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_YOFFSET),
              r[rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL],
              r[rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
              r[rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
              r[rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL],
              r[rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR], st.scissor.offset[0],
              st.scissor.offset[1], st.scissor.extent[0], st.scissor.extent[1],
              st.viewport.xy_offset[0], st.viewport.xy_offset[1], st.viewport.xy_extent[0],
              st.viewport.xy_extent[1], r[rex::graphics::XE_GPU_REG_RB_COLOR_MASK] & 0xFFFF,
              r[rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL],
              r[rex::graphics::XE_GPU_REG_PA_CL_CLIP_CNTL],
              r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE),
              r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET),
              uint32_t(r.Get<reg::PA_CL_CLIP_CNTL>().clip_disable), targets, fetches);
}

// The Xbox 360 game clears its 128x128 water simulation targets in two halves: the right 48
// columns with a colour clear at 1x, the left 80 with a depth clear of a 4x MSAA depth buffer
// placed on the same EDRAM tiles (HANDOFF B:102). Here colour and depth are separate images, so
// a depth clear also clears the colour target sharing its tiles and layout, over the cleared
// rectangle, with the depth/stencil word read back as that colour format.
void Renderer::Impl::AliasDepthClearToColor(xenos::MsaaSamples msaa_samples) {
  const RegisterFile& r = *regs;
  const RenderTarget* depth_rt = setup.targets.depth;
  if (!setup.active_vs || setup.active_vs->vertex_bindings().empty()) return;
  // Screen-space rectangle (VTE: no viewport scale/offset, xy and z not divided).
  if ((r[rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL] & 0x33F) != 0x300) return;
  auto depth_control = draw_util::GetNormalizedDepthControl(r);
  if (!depth_control.z_write_enable && !depth_control.stencil_enable) return;
  // Every colour target on those tiles: in EDRAM they are the same memory, so all of them see
  // the cleared word. (run14: taking only the first match picked a 64bpp fmt5 target at the same
  // base, which is not reinterpreted, so the heat-haze map's left part was never cleared.)
  std::vector<RenderTarget*> color_rts;
  for (auto& [key, rt] : render_targets) {
    if (!key.is_depth && key.base_tiles == depth_rt->key.base_tiles &&
        key.pitch == depth_rt->key.pitch && key.msaa == depth_rt->key.msaa) {
      color_rts.push_back(rt.get());
    }
  }
  if (color_rts.empty()) return;
  // The rectangle and depth from the clear's three vertices (position = first three words).
  const auto& binding = setup.active_vs->vertex_bindings()[0];
  const auto vf = r.GetVertexFetch(binding.fetch_constant);
  const uint32_t address = (uint32_t(vf.address) << 2) & 0x1FFFFFFF;
  const uint32_t stride = binding.stride_words;
  if (stride < 3 || address + uint64_t(stride) * 3 * 4 > 0x20000000u) return;
  const uint8_t* data = memory->TranslatePhysical<const uint8_t*>(address);
  float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f, z = 0.0f;
  for (uint32_t v = 0; v < 3; ++v) {
    float xyz[3];
    for (uint32_t i = 0; i < 3; ++i) {
      uint32_t w;
      std::memcpy(&w, data + (size_t(v) * stride + i) * 4, 4);
      w = xenos::GpuSwap(w, vf.endian);
      std::memcpy(&xyz[i], &w, 4);
    }
    minx = std::min(minx, xyz[0]);
    maxx = std::max(maxx, xyz[0]);
    miny = std::min(miny, xyz[1]);
    maxy = std::max(maxy, xyz[1]);
    if (v == 0) z = xyz[2];
  }
  if (!(maxx > minx) || !(maxy > miny)) return;
  // The 32-bit word the depth clear leaves in EDRAM.
  z = std::min(std::max(z, 0.0f), 1.0f);
  uint32_t depth24 = xenos::DepthRenderTargetFormat(depth_rt->key.format) ==
                             xenos::DepthRenderTargetFormat::kD24FS8
                         ? xenos::Float32To20e4(z, true)
                         : std::min(uint32_t(double(z) * 16777215.0 + 0.5), 0xFFFFFFu);
  uint32_t stencil = depth_control.stencil_enable
                         ? uint32_t(r.Get<reg::RB_STENCILREFMASK>().stencilref) & 0xFF
                         : 0u;
  const uint32_t word = (depth24 << 8) | stencil;
  const uint32_t sx = SampleScaleX(msaa_samples), sy = SampleScaleY(msaa_samples);
  for (RenderTarget* color_rt : color_rts) {
    const int32_t x0 = std::max(0, int32_t(std::floor(minx + 0.5f))) * int32_t(sx);
    const int32_t y0 = std::max(0, int32_t(std::floor(miny + 0.5f))) * int32_t(sy);
    const int32_t x1 = std::min(int32_t(color_rt->width), int32_t(std::floor(maxx + 0.5f)) * int32_t(sx));
    const int32_t y1 = std::min(int32_t(color_rt->height), int32_t(std::floor(maxy + 0.5f)) * int32_t(sy));
    if (x1 <= x0 || y1 <= y0) continue;
    VkClearAttachment attachment = {};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachment.colorAttachment = 0;
    using F = xenos::ColorRenderTargetFormat;
    switch (F(color_rt->key.format)) {
      case F::k_8_8_8_8:
      case F::k_8_8_8_8_GAMMA:
        for (uint32_t j = 0; j < 4; ++j) {
          attachment.clearValue.color.float32[j] = float((word >> (j * 8)) & 0xFF) / 255.0f;
        }
        break;
      case F::k_16_16:
        for (uint32_t j = 0; j < 2; ++j) {
          attachment.clearValue.color.float32[j] = std::max(
              float(int16_t((word >> (j * 16)) & 0xFFFF)) * (32.0f / 32767.0f), -32.0f);
        }
        break;
      case F::k_16_16_FLOAT:
        for (uint32_t j = 0; j < 2; ++j) {
          attachment.clearValue.color.float32[j] =
              rex::xenos_half_to_float(uint16_t((word >> (j * 16)) & 0xFFFF));
        }
        break;
      case F::k_32_FLOAT:
        std::memcpy(&attachment.clearValue.color.float32[0], &word, 4);
        break;
      default:
        // Other layouts (64-bit, 10:10:10:2) are not reinterpreted.
        continue;
    }
    EndPass(PassEnd::ClearBegin);
    VkRenderingAttachmentInfo color = {};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = color_rt->view;
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea.extent = {color_rt->width, color_rt->height};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    VkCommandBuffer c = cmd();
    dfn().vkCmdBeginRendering(c, &info);
    VkClearRect rect = {};
    rect.rect.offset = {x0, y0};
    rect.rect.extent = {uint32_t(x1 - x0), uint32_t(y1 - y0)};
    rect.layerCount = 1;
    dfn().vkCmdClearAttachments(c, 1, &attachment, 1, &rect);
    dfn().vkCmdEndRendering(c);
    // Log each distinct clear (target, rectangle, word) once, up to 48.
    static std::unordered_set<uint64_t> logged;
    const uint64_t log_key = (uint64_t(word) << 32) ^ (uint64_t(color_rt->key.base_tiles) << 20) ^
                             (uint64_t(color_rt->key.format) << 16) ^ uint64_t(x1 * 7919 + y1);
    if (logged.size() < 48 && logged.insert(log_key).second) {
      REXLOG_INFO("[native] EDRAM depth clear also clears colour fmt{}@{} rect {},{}-{},{} word {:08X}",
                  color_rt->key.format, color_rt->key.base_tiles, x0, y0, x1, y1, word);
    }
  }
}

void Renderer::Impl::WaterSequenceDump(uint32_t draw_count) {
  const RenderTarget* rt = setup.targets.color[0];
  if (!rt || rt->width != 160 || rt->height < 128) return;
  const RegisterFile& r = *regs;
  const uint32_t n = water_seq_count++;
  std::string vertices;
  if (setup.active_vs && !setup.active_vs->vertex_bindings().empty()) {
    const auto& binding = setup.active_vs->vertex_bindings()[0];
    const auto vf = r.GetVertexFetch(binding.fetch_constant);
    const uint32_t address = (uint32_t(vf.address) << 2) & 0x1FFFFFFF;
    const uint32_t words = std::min<uint32_t>(uint32_t(vf.size), 21);
    if (address + uint64_t(words) * 4 <= 0x20000000u) {
      const uint8_t* data = memory->TranslatePhysical<const uint8_t*>(address);
      for (uint32_t i = 0; i < words; ++i) {
        uint32_t w;
        std::memcpy(&w, data + i * 4, 4);
        vertices += fmt::format(" {:08X}", w);
      }
    }
    vertices = fmt::format(" vf{} {:08X} stride {} endian {}:", binding.fetch_constant, address,
                           binding.stride_words, uint32_t(vf.endian)) + vertices;
  }
  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = last_initiator_value;
  auto surface = r.Get<reg::RB_SURFACE_INFO>();
  REXLOG_INFO("[native] WATERSEQ {} VS {:016X} PS {:016X} prim {} count {} | msaa {} pitch {} | "
              "C0 fmt{}@{} C1 {} | blend0 {:08X} colorcontrol {:08X} mask {:08X} modecontrol {:08X} "
              "| clip {:08X} vte {:08X} | depth {:08X} |{}",
              n, setup.active_vs ? setup.active_vs->ucode_data_hash() : 0,
              setup.active_ps ? setup.active_ps->ucode_data_hash() : 0,
              uint32_t(initiator.prim_type), draw_count, uint32_t(surface.msaa_samples),
              uint32_t(surface.surface_pitch), rt->key.format, rt->key.base_tiles,
              setup.targets.color[1] ? fmt::format("fmt{}@{}", setup.targets.color[1]->key.format,
                                                   setup.targets.color[1]->key.base_tiles)
                                     : std::string("-"),
              r[rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0],
              r[rex::graphics::XE_GPU_REG_RB_COLORCONTROL], r[rex::graphics::XE_GPU_REG_RB_COLOR_MASK],
              r[rex::graphics::XE_GPU_REG_RB_MODECONTROL], r[rex::graphics::XE_GPU_REG_PA_CL_CLIP_CNTL],
              r[rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL], r[rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL],
              vertices);
  RecordImageDump(rt->image, rt->format, rt->width, 128,
                  fmt::format("water_seq{:02}_fmt{}_{}", n, rt->key.format, rt->key.base_tiles));
}

void Renderer::Impl::DumpWaterSimInputs() {
  water_raw_pending = false;
  packet_trace_request = true;
  const RegisterFile& r = *regs;
  // What the draw just sampled and wrote (HANDOFF B:98): the host image behind t0/t1 (or the
  // resolve destination the cache answered with), and colour target 0 after the draw.
  for (uint32_t fetch_index : {0u, 1u}) {
    const auto fetch = r.GetTextureFetch(fetch_index);
    const uint32_t address = (fetch.base_address << 12) & 0x1FFFFFFF;
    textures.GetView(fetch, xenos::FetchOpDimension::k2D, false, upload);
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = fetch.size_2d.width + 1, height = fetch.size_2d.height + 1;
    const char* source = "none";
    if (textures.DebugTextureImage(textures.last_texture(), &image, &format)) {
      source = "texture";
    } else if (const ResolveDestination* dest = textures.FindResolveDestination(address)) {
      image = dest->image;
      format = dest->format;
      width = dest->width;
      height = dest->height;
      source = "RESOLVE-DESTINATION";
    }
    REXLOG_INFO("[native] water dump: sim t{} {:08X} sampled from {}", fetch_index, address,
                source);
    RecordImageDump(image, format, width, height,
                    fmt::format("water_host_t{}_{:08X}_{}", fetch_index, address, source));
  }
  {
    // The draw's primitive and vertex data (HANDOFF B:99): the quad covers only 80 of 128 columns.
    reg::VGT_DRAW_INITIATOR initiator;
    initiator.value = last_initiator_value;
    REXLOG_INFO("[native] water dump: sim draw prim {} indices {} source {} index size {} | "
                "VGT min {} max {} offset {} | multi-prim reset {}",
                uint32_t(initiator.prim_type), uint32_t(initiator.num_indices),
                uint32_t(initiator.source_select), uint32_t(initiator.index_size),
                r[rex::graphics::XE_GPU_REG_VGT_MIN_VTX_INDX],
                r[rex::graphics::XE_GPU_REG_VGT_MAX_VTX_INDX],
                r[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET],
                r[rex::graphics::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX]);
    if (setup.active_vs) {
      for (const auto& binding : setup.active_vs->vertex_bindings()) {
        const auto vf = r.GetVertexFetch(binding.fetch_constant);
        const uint32_t bytes = std::min<uint32_t>(uint32_t(vf.size) * 4, 4096);
        const uint32_t address = (uint32_t(vf.address) << 2) & 0x1FFFFFFF;
        const std::string path = fmt::format(
            "sdmc:/switch/Prototype2/logs/water_vtx_vf{}_{:08X}_stride{}_endian{}.bin",
            binding.fetch_constant, address, binding.stride_words, uint32_t(vf.endian));
        bool written = false;
        if (bytes && address + uint64_t(bytes) <= 0x20000000u) {
          if (FILE* file = std::fopen(path.c_str(), "wb")) {
            written = std::fwrite(memory->TranslatePhysical<const uint8_t*>(address), bytes, 1,
                                  file) == 1;
            written = std::fclose(file) == 0 && written;
          }
        }
        REXLOG_INFO("[native] water dump: sim vf{} {:08X} size {} words stride {} endian {} "
                    "type {} -> {}{}",
                    binding.fetch_constant, address, uint32_t(vf.size), binding.stride_words,
                    uint32_t(vf.endian), uint32_t(vf.type), path, written ? "" : " (FAILED)");
      }
    }
  }
  if (const RenderTarget* rt = setup.targets.color[0]) {
    RecordImageDump(rt->image, rt->format, std::min(rt->width, 160u), std::min(rt->height, 128u),
                    fmt::format("water_rt0_fmt{}_{}", rt->key.format, rt->key.base_tiles));
  }
  {
    std::string constants;
    for (uint32_t c : {0u, 1u, 2u, 3u, 256u, 257u, 258u, 259u}) {
      uint32_t index = rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4;
      constants += fmt::format(" c{}={:08X},{:08X},{:08X},{:08X}", c, r[index], r[index + 1],
                               r[index + 2], r[index + 3]);
    }
    REXLOG_INFO("[native] water dump: sim constants{} | SQ_PS_CONST {:08X} SQ_VS_CONST {:08X}",
                constants, r[rex::graphics::XE_GPU_REG_SQ_PS_CONST],
                r[rex::graphics::XE_GPU_REG_SQ_VS_CONST]);
  }
  for (uint32_t fetch_index : {0u, 1u}) {
    const auto fetch = r.GetTextureFetch(fetch_index);
    uint32_t words[6];
    std::memcpy(words, &fetch, sizeof(words));
    uint32_t w, h, depth, base, mip, min_level, max_level;
    rex::graphics::texture_util::GetSubresourcesFromFetchConstant(
        fetch, &w, &h, &depth, &base, &mip, &min_level, &max_level);
    const auto layout = rex::graphics::texture_util::GetGuestTextureLayout(
        fetch.dimension, fetch.pitch, w + 1, h + 1, depth + 1, fetch.tiled,
        rex::graphics::GetBaseFormat(fetch.format), fetch.packed_mips, base != 0, max_level);
    const uint32_t size = base ? layout.base.level_data_extent_bytes : 0;
    if (!size || (uint64_t(base) << 12) + size > 0x20000000u) continue;
    const uint8_t* data = memory->TranslatePhysical<const uint8_t*>(base << 12);
    const std::string path = fmt::format(
        "sdmc:/switch/Prototype2/logs/water_raw_t{}_{:08X}_{}x{}_fmt{}.bin", fetch_index,
        base << 12, w + 1, h + 1, uint32_t(fetch.format));
    bool written = false;
    if (FILE* file = std::fopen(path.c_str(), "wb")) {
      // Header: the six fetch constant words and the byte count, then the guest bytes as is.
      written = std::fwrite(words, sizeof(words), 1, file) == 1 &&
                std::fwrite(&size, sizeof(size), 1, file) == 1 &&
                std::fwrite(data, size, 1, file) == 1;
      written = std::fclose(file) == 0 && written;
    }
    REXLOG_INFO("[native] water dump: sim input t{} {:08X} {}x{} fmt {} tiled {} endian {} pitch {} "
                "-> {} ({} bytes{})", fetch_index, base << 12, w + 1, h + 1,
                uint32_t(fetch.format), uint32_t(fetch.tiled), uint32_t(fetch.endianness),
                uint32_t(fetch.pitch), path, size, written ? "" : ", FAILED");
  }
}

void Renderer::Impl::RequestWaterDump() {
  const auto now = std::chrono::steady_clock::now();
  if (water_dump_first == std::chrono::steady_clock::time_point{}) {
    water_dump_first = now;
    REXLOG_INFO("[native] water dump: water first drawn, textures saved in 20 s");
  }
  if (now - water_dump_first < std::chrono::seconds(20)) return;
  const RegisterFile& r = *regs;
  for (uint32_t fetch_index : {16u, 5u, 6u}) {
    const auto fetch = r.GetTextureFetch(fetch_index);
    if (fetch.type != xenos::FetchConstantType::kTexture || !fetch.base_address) continue;
    const uint32_t address = fetch.base_address << 12;
    if (water_dump_seen.insert(address).second) {
      water_dump_requests.emplace_back(address, fetch_index);
      water_raw_pending = true;
      if (!water_seq_frame) water_seq_frame = frame + 1;
      if (water_sim_trace_enabled && !water_sim_trace_frame) {
        water_sim_trace_frame = frame + 1;
        REXLOG_INFO("[native] SIMTRACE armed for frame {}", water_sim_trace_frame);
      }
    }
  }
}

uint32_t Renderer::Impl::DumpTexelBytes(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_R8G8_UNORM:
      return 2;
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
      return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R32G32_SFLOAT:
      return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
      return 16;
    case VK_FORMAT_R8_UNORM:
      return 1;
    default:
      return 0;
  }
}

bool Renderer::Impl::RecordImageDump(VkImage image, VkFormat format, uint32_t width,
                                     uint32_t height, const std::string& name) {
  const uint32_t texel_bytes = DumpTexelBytes(format);
  if (!image || !texel_bytes || !width || !height) {
    REXLOG_INFO("[native] water dump: {} not dumped (format {}, {}x{})", name, uint32_t(format),
                width, height);
    return false;
  }
  WaterDump dump;
  dump.name = name;
  dump.width = width;
  dump.height = height;
  dump.format = format;
  dump.texel_bytes = texel_bytes;
  const VkDeviceSize size = VkDeviceSize(width) * height * texel_bytes;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          rex::ui::vulkan::util::MemoryPurpose::kReadback, dump.buffer, dump.memory)) {
    return false;
  }
  BeginRecording();
  EndPass(PassEnd::Resolve);
  VkCommandBuffer c = cmd();
  VkMemoryBarrier before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT};
  dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &before, 0, nullptr, 0, nullptr);
  VkBufferImageCopy region = {};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  dfn().vkCmdCopyImageToBuffer(c, image, VK_IMAGE_LAYOUT_GENERAL, dump.buffer, 1, &region);
  VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_HOST_READ_BIT};
  dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &after, 0, nullptr, 0, nullptr);
  dump.submission = submission_number;
  water_dumps.push_back(dump);
  REXLOG_INFO("[native] water dump: {} {}x{} format {}, copy recorded", name, width, height,
              uint32_t(format));
  return true;
}

void Renderer::Impl::RecordWaterDumps() {
  if (water_dump_requests.empty()) return;
  for (const auto& [address, fetch_index] : water_dump_requests) {
    const ResolveDestination* dest = textures.FindResolveDestination(address & 0x1FFFFFFF);
    const std::string name = fmt::format("water_dump_f{}_{:08X}", fetch_index, address);
    if (!dest) {
      REXLOG_INFO("[native] water dump: {} not a resolve destination", name);
      continue;
    }
    RecordImageDump(dest->image, dest->format, dest->width, dest->height, name);
  }
  water_dump_requests.clear();
}

void Renderer::Impl::FinishWaterDumps(bool wait) {
  p2::gpuwait::Scope gpu_wait_scope(p2::gpuwait::ReadbackWait);
  for (auto it = water_dumps.begin(); it != water_dumps.end();) {
    Submission& s = submissions[it->submission % kSubmissionSlots];
    const bool submitted = submission_number > it->submission || (wait && !recording);
    if (wait && s.number == it->submission) WaitSubmission(s);
    if (!submitted || (s.pending && s.number == it->submission)) {
      ++it;
      continue;
    }
    void* mapping = nullptr;
    const VkDeviceSize size = VkDeviceSize(it->width) * it->height * it->texel_bytes;
    if (dfn().vkMapMemory(vk, it->memory, 0, VK_WHOLE_SIZE, 0, &mapping) == VK_SUCCESS) {
      VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, it->memory, 0,
                                   VK_WHOLE_SIZE};
      dfn().vkInvalidateMappedMemoryRanges(vk, 1, &range);
      const std::string path =
          fmt::format("sdmc:/switch/Prototype2/logs/{}_{}x{}_vk{}.bin", it->name, it->width,
                      it->height, uint32_t(it->format));
      bool written = false;
      if (FILE* file = std::fopen(path.c_str(), "wb")) {
        written = std::fwrite(mapping, size_t(size), 1, file) == 1;
        written = std::fclose(file) == 0 && written;
      }
      dfn().vkUnmapMemory(vk, it->memory);
      REXLOG_INFO("[native] water dump: {} {} ({} bytes)", written ? "saved" : "FAILED to save",
                  path, size);
    }
    dfn().vkDestroyBuffer(vk, it->buffer, nullptr);
    dfn().vkFreeMemory(vk, it->memory, nullptr);
    it = water_dumps.erase(it);
  }
}

void Renderer::Impl::DrainPrewarmed() {
  if (!prewarm_completed_count.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<std::pair<uint64_t, VkPipeline>> done;
  {
    std::lock_guard<std::mutex> lock(prewarm_mutex);
    done.swap(prewarm_completed);
    prewarm_completed_count.store(0, std::memory_order_release);
  }
  for (auto& [hash, pipeline] : done) {
    if (!pipeline) continue;
    if (pipelines.emplace(hash, pipeline).second) {
      ++stats().prewarm_pipelines;
    } else {
      // The ring made this one itself meanwhile.
      dfn().vkDestroyPipeline(vk, pipeline, nullptr);
    }
  }
}

void Renderer::Impl::RecordPrewarm(const PipelineKey& key) {
  if (!prewarm_enabled || key.tessellation || key.tessellation_reserved ||
      prewarm_keys.size() >= kPrewarmMaxKeys) {
    return;
  }
  const uint64_t key_hash = XXH3_64bits(&key, sizeof(key));
  if (prewarm_key_hashes.count(key_hash)) return;
  const VulkanShader* stages[2] = {};
  const uint64_t hashes[2] = {key.vs_hash, key.ps_hash};
  for (uint32_t i = 0; i < 2; ++i) {
    if (!hashes[i]) continue;
    auto it = shaders.find(hashes[i]);
    if (it == shaders.end() || it->second->ucode_data_hash() != hashes[i]) return;
    stages[i] = it->second.get();
  }
  prewarm_key_hashes.insert(key_hash);
  for (const VulkanShader* shader : stages) {
    if (!shader || !prewarm_shader_hashes.insert(shader->ucode_data_hash()).second) continue;
    PrewarmShader entry;
    entry.hash = shader->ucode_data_hash();
    entry.type = uint32_t(shader->type());
    entry.ucode = shader->ucode_data();
    prewarm_shader_list.push_back(std::move(entry));
  }
  prewarm_keys.push_back(key);
}

void Renderer::Impl::PrewarmThread(std::vector<PrewarmShader> list_shaders,
                                   std::vector<PipelineKey> keys) {
  rex::thread::set_current_thread_name("GPU prewarm");
#if defined(__SWITCH__)
  // As the pipeline worker: below the game's threads and the ring.
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3F);
#endif
  const uint64_t start = NowNs();
  std::unordered_map<uint64_t, std::unique_ptr<VulkanShader>> own;
  for (const PrewarmShader& entry : list_shaders) {
    own.emplace(entry.hash, std::make_unique<VulkanShader>(
                                device, xenos::ShaderType(entry.type), entry.hash,
                                entry.ucode.data(), entry.ucode.size(), std::endian::native));
  }
  // Translators of its own, with the ring's options (TranslatorFor).
  rex::string::StringBuffer own_disasm;
  auto base = std::make_unique<SpirvShaderTranslator>(SpirvShaderTranslator::Features(device),
                                                      true, true, false);
  base->SetTextureSignSpecializationEnabled(specialize_signs);
  base->SetStructuredForwardJumpsEnabled(structured_jumps);
  std::unique_ptr<SpirvShaderTranslator> scaled[2][2];
  uint32_t translated = 0, translate_failed = 0;
  auto translate = [&](VulkanShader* shader, uint64_t modification) -> VkShaderModule {
    if (!shader->is_ucode_analyzed()) shader->AnalyzeUcode(own_disasm);
    auto* translation =
        static_cast<VulkanShader::VulkanTranslation*>(shader->GetOrCreateTranslation(modification));
    if (!translation->is_translated()) {
      SpirvShaderTranslator* translator = base.get();
      const uint32_t x2 = (modification & kVposScaleXBit) ? 1 : 0;
      const uint32_t y2 = (modification & kVposScaleYBit) ? 1 : 0;
      if (shader->type() == xenos::ShaderType::kPixel && (x2 || y2)) {
        auto& slot = scaled[x2][y2];
        if (!slot) {
          slot = std::make_unique<SpirvShaderTranslator>(SpirvShaderTranslator::Features(device),
                                                         true, true, false, x2 ? 2 : 1,
                                                         y2 ? 2 : 1);
          slot->SetTextureSignSpecializationEnabled(specialize_signs);
          slot->SetStructuredForwardJumpsEnabled(structured_jumps);
        }
        translator = slot.get();
      }
      if (translator->TranslateAnalyzedShader(*translation)) {
        translation->GetOrCreateShaderModule();
        ++translated;
      } else {
        ++translate_failed;
      }
    }
    return translation->is_valid() ? translation->shader_module() : VK_NULL_HANDLE;
  };
  // Phase 1: every listed translation, before any shader is handed to the ring (after that the
  // ring may add translations to them; this thread then only uses the modules it took here).
  std::vector<std::pair<VkShaderModule, VkShaderModule>> modules(keys.size());
  for (size_t i = 0; i < keys.size() && !prewarm_stop.load(std::memory_order_relaxed); ++i) {
    const PipelineKey& key = keys[i];
    auto vs = own.find(key.vs_hash);
    if (vs == own.end()) continue;
    VkShaderModule vertex = translate(vs->second.get(), key.vs_modification);
    VkShaderModule pixel = VK_NULL_HANDLE;
    if (key.ps_hash) {
      auto ps = own.find(key.ps_hash);
      if (ps == own.end()) continue;
      pixel = translate(ps->second.get(), key.ps_modification);
      if (!pixel) continue;
    }
    modules[i] = {vertex, pixel};
  }
  const uint64_t translated_at = NowNs();
  {
    std::lock_guard<std::mutex> lock(prewarm_mutex);
    for (auto& [hash, shader] : own) {
      prewarmed_shaders.emplace(hash, std::move(shader));
    }
  }
  prewarm_published.store(true, std::memory_order_release);
  // Phase 2: the pipelines, with layouts and geometry shaders of its own made as the ring makes
  // them (compatible layouts); destroyed at the end, which Vulkan allows once pipelines exist.
  std::unordered_map<uint64_t, VkDescriptorSetLayout> set_layouts;
  std::unordered_map<uint64_t, VkPipelineLayout> layouts;
  std::unordered_map<uint32_t, VkShaderModule> geometry;
  auto set_layout = [&](bool vertex, uint32_t texture_count,
                        uint32_t sampler_count) -> VkDescriptorSetLayout {
    if (!texture_count && !sampler_count) return layout_empty;
    const uint64_t id = (uint64_t(vertex) << 40) | (uint64_t(texture_count) << 20) | sampler_count;
    if (auto it = set_layouts.find(id); it != set_layouts.end()) return it->second;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    VkShaderStageFlags stages =
        vertex ? VK_SHADER_STAGE_VERTEX_BIT |
                     (tessellation_supported ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : 0)
               : VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 0; i < texture_count; ++i) {
      bindings.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr});
    }
    for (uint32_t i = 0; i < sampler_count; ++i) {
      bindings.push_back({texture_count + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, nullptr});
    }
    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = uint32_t(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    dfn().vkCreateDescriptorSetLayout(vk, &info, nullptr, &layout);
    set_layouts.emplace(id, layout);
    return layout;
  };
  uint32_t created = 0, skipped = 0;
  for (size_t i = 0; i < keys.size() && !prewarm_stop.load(std::memory_order_relaxed); ++i) {
    const PipelineKey& key = keys[i];
    if (!modules[i].first || key.tessellation || key.tessellation_reserved) {
      ++skipped;
      continue;
    }
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (auto it = layouts.find(key.layout); it != layouts.end()) {
      layout = it->second;
    } else {
      VkDescriptorSetLayout vertex_set =
          set_layout(true, uint32_t(key.layout >> 48) & 0xFFFF, uint32_t(key.layout >> 32) & 0xFFFF);
      VkDescriptorSetLayout pixel_set =
          set_layout(false, uint32_t(key.layout >> 16) & 0xFFFF, uint32_t(key.layout) & 0xFFFF);
      if (vertex_set && pixel_set) {
        VkDescriptorSetLayout sets[SpirvShaderTranslator::kDescriptorSetCount];
        sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = layout_shared;
        sets[SpirvShaderTranslator::kDescriptorSetConstants] = layout_constants;
        sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = vertex_set;
        sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = pixel_set;
        VkPipelineLayoutCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = SpirvShaderTranslator::kDescriptorSetCount;
        info.pSetLayouts = sets;
        dfn().vkCreatePipelineLayout(vk, &info, nullptr, &layout);
      }
      layouts.emplace(key.layout, layout);
    }
    VkShaderModule geometry_module = VK_NULL_HANDLE;
    if (key.geometry_shader) {
      auto it = geometry.find(key.geometry_shader);
      if (it == geometry.end()) {
        GeometryShaderKey geometry_key;
        geometry_key.key = key.geometry_shader;
        it = geometry.emplace(key.geometry_shader, CreateGeometryShader(device, geometry_key)).first;
      }
      geometry_module = it->second;
    }
    if (!layout || (key.geometry_shader && !geometry_module)) {
      ++skipped;
      continue;
    }
    PipelineJob job;
    job.hash = XXH3_64bits(&key, sizeof(key));
    job.key = key;
    job.vertex = modules[i].first;
    job.pixel = modules[i].second;
    job.geometry = geometry_module;
    job.layout = layout;
    VkPipeline pipeline = CreatePipeline(job);
    if (pipeline) ++created;
    std::lock_guard<std::mutex> lock(prewarm_mutex);
    prewarm_completed.emplace_back(job.hash, pipeline);
    prewarm_completed_count.fetch_add(1, std::memory_order_release);
  }
  for (auto& [id, layout] : layouts) {
    if (layout) dfn().vkDestroyPipelineLayout(vk, layout, nullptr);
  }
  for (auto& [id, layout] : set_layouts) {
    if (layout) dfn().vkDestroyDescriptorSetLayout(vk, layout, nullptr);
  }
  for (auto& [id, module] : geometry) {
    if (module) dfn().vkDestroyShaderModule(vk, module, nullptr);
  }
  const uint64_t end = NowNs();
  REXLOG_INFO("[native] pipeline prewarm: {} shaders, {} translations ({} failed) in {} ms; "
              "{} of {} pipelines created, {} skipped, in {} ms{}",
              list_shaders.size(), translated, translate_failed,
              (translated_at - start) / 1000000, created, keys.size(), skipped,
              (end - translated_at) / 1000000,
              prewarm_stop.load(std::memory_order_relaxed) ? " (stopped)" : "");
}

void Renderer::Impl::PipelineThread() {
  rex::thread::set_current_thread_name("GPU pipeline");
#if defined(__SWITCH__)
  // Below the game's threads and the ring: compiling is never urgent enough
  // to take a core from them.
  const auto priority_result = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3F);
  REXLOG_INFO("[native] pipeline priority request 0x3F: result 0x{:08X}", priority_result);
#endif
  while (true) {
    PipelineJob job;
    {
      std::unique_lock<std::mutex> lock(pipeline_mutex);
      pipeline_cv.wait(lock, [&] { return pipeline_stop || !pipeline_jobs.empty(); });
      if (pipeline_stop) {
        return;
      }
      job = pipeline_jobs.front();
      pipeline_jobs.pop_front();
    }
    VkPipeline pipeline = CreatePipeline(job);
    std::lock_guard<std::mutex> lock(pipeline_mutex);
    pipelines_completed.emplace_back(job.hash, pipeline);
    pipelines_completed_count.fetch_add(1, std::memory_order_release);
  }
}

// Builds the pipeline for a key. Only reads immutable state, so it runs on the
// pipeline worker as well as on the ring thread.
VkPipeline Renderer::Impl::CreatePipeline(const PipelineJob& job) {
  const PipelineKey& key = job.key;
  VkShaderModule geometry = job.geometry;
  VkPipelineLayout layout = job.layout;

  VkSpecializationMapEntry sign_entries[8];
  for (uint32_t i = 0; i < 8; ++i) {
    sign_entries[i] = {i, uint32_t(sizeof(uint32_t) * i), sizeof(uint32_t)};
  }
  VkSpecializationInfo signs = {8, sign_entries, sizeof(key.texture_signs), key.texture_signs};
  const VkSpecializationInfo* specialization = specialize_signs ? &signs : nullptr;

  VkPipelineShaderStageCreateInfo stages[4] = {};
  uint32_t stage_count = 0;
  if (job.tess_vertex) {
    stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                             VK_SHADER_STAGE_VERTEX_BIT, job.tess_vertex, "main", nullptr};
    stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                             VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, job.tess_control, "main",
                             nullptr};
    stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                             VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, job.vertex, "main",
                             specialization};
  } else {
    stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                             nullptr,
                             0,
                             VK_SHADER_STAGE_VERTEX_BIT,
                             job.vertex,
                             "main",
                             specialization};
  }
  if (geometry) {
    stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                             nullptr,
                             0,
                             VK_SHADER_STAGE_GEOMETRY_BIT,
                             geometry,
                             "main",
                             nullptr};
  }
  if (job.pixel) {
    stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                             nullptr,
                             0,
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                             job.pixel,
                             "main",
                             specialization};
  }

  VkPipelineVertexInputStateCreateInfo vertex_input = {};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VkPrimitiveTopology(key.topology);
  input_assembly.primitiveRestartEnable = key.primitive_restart ? VK_TRUE : VK_FALSE;
  VkPipelineViewportStateCreateInfo viewport = {};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.depthClampEnable = key.depth_clamp ? VK_TRUE : VK_FALSE;
  raster.polygonMode = key.polygon_line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
  raster.cullMode = key.cull_mode;
  raster.frontFace =
      key.front_face_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.depthBiasEnable = key.depth_format ? VK_TRUE : VK_FALSE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VkSampleCountFlagBits(key.samples);
  VkPipelineDepthStencilStateCreateInfo depth = {};
  depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth.depthTestEnable = key.depth_test ? VK_TRUE : VK_FALSE;
  depth.depthWriteEnable = key.depth_write ? VK_TRUE : VK_FALSE;
  depth.depthCompareOp = VkCompareOp(key.depth_compare);
  depth.stencilTestEnable =
      key.stencil_test && HasStencil(VkFormat(key.depth_format)) ? VK_TRUE : VK_FALSE;
  depth.front.failOp = VkStencilOp(key.stencil_front[0]);
  depth.front.passOp = VkStencilOp(key.stencil_front[1]);
  depth.front.depthFailOp = VkStencilOp(key.stencil_front[2]);
  depth.front.compareOp = VkCompareOp(key.stencil_front[3]);
  depth.back.failOp = VkStencilOp(key.stencil_back[0]);
  depth.back.passOp = VkStencilOp(key.stencil_back[1]);
  depth.back.depthFailOp = VkStencilOp(key.stencil_back[2]);
  depth.back.compareOp = VkCompareOp(key.stencil_back[3]);
  VkPipelineColorBlendAttachmentState attachments[4] = {};
  uint32_t attachment_count = 0;
  VkFormat color_formats[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    color_formats[i] = VkFormat(key.color_formats[i]);
    if (key.color_formats[i]) {
      attachment_count = i + 1;
    }
    VkPipelineColorBlendAttachmentState& a = attachments[i];
    uint32_t blend = key.blend[i];
    a.colorWriteMask = (key.write_masks >> (4 * i)) & 0xF;
    if (blend != 0x00010001) {
      reg::RB_BLENDCONTROL control;
      control.value = blend;
      a.blendEnable = VK_TRUE;
      a.srcColorBlendFactor = BlendFactorToVk(control.color_srcblend);
      a.dstColorBlendFactor = BlendFactorToVk(control.color_destblend);
      a.colorBlendOp = BlendOpToVk(control.color_comb_fcn);
      a.srcAlphaBlendFactor = BlendFactorToVk(control.alpha_srcblend);
      a.dstAlphaBlendFactor = BlendFactorToVk(control.alpha_destblend);
      a.alphaBlendOp = BlendOpToVk(control.alpha_comb_fcn);
    }
  }
  VkPipelineColorBlendStateCreateInfo blend = {};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = attachment_count;
  blend.pAttachments = attachments;
  VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_DEPTH_BIAS,
      VK_DYNAMIC_STATE_BLEND_CONSTANTS,
      VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE,
  };
  VkPipelineDynamicStateCreateInfo dynamic = {};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering = {};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = attachment_count;
  rendering.pColorAttachmentFormats = color_formats;
  rendering.depthAttachmentFormat = VkFormat(key.depth_format);
  rendering.stencilAttachmentFormat = VkFormat(key.depth_format);

  VkGraphicsPipelineCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.pNext = &rendering;
  info.stageCount = stage_count;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  VkPipelineTessellationStateCreateInfo tessellation_state = {};
  tessellation_state.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
  tessellation_state.patchControlPoints = job.patch_points;
  info.pTessellationState = job.tess_vertex ? &tessellation_state : nullptr;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = layout;
  const bool capture_stats = REXCVAR_GET(p2_native_shader_stats) &&
      rex::ui::vulkan::switch_shader_stats_device.load(std::memory_order_acquire) == vk;
  if (capture_stats) info.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult result = dfn().vkCreateGraphicsPipelines(vk, driver_cache, 1, &info, nullptr, &pipeline);
  if (result != VK_SUCCESS && capture_stats) {
    REXLOG_WARN("NATIVE_SHADER_STATS capture creation failed result={} ps={:016X}; retry normal creation",
                int32_t(result), key.ps_hash);
    info.flags &= ~VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    result = dfn().vkCreateGraphicsPipelines(vk, driver_cache, 1, &info, nullptr, &pipeline);
  }
  if (result != VK_SUCCESS) {
    REXLOG_ERROR("[native] Pipeline creation failed (VS {:016X}, PS {:016X})", key.vs_hash,
                 key.ps_hash);
    pipeline = VK_NULL_HANDLE;
  } else if (info.flags & VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR) {
    LogNativeShaderStats(device, pipeline, key.vs_hash, key.vs_modification,
                         key.ps_hash, key.ps_modification);
  }
  return pipeline;
}

// --- Render targets ------------------------------------------------------------------

RenderTarget* Renderer::Impl::GetRenderTarget(const RenderTargetKey& key, uint32_t min_height) {
  min_height = std::clamp(min_height, 16u, 2048u);
  auto it = render_targets.find(key);
  RenderTarget* old = nullptr;
  if (it != render_targets.end()) {
    old = it->second.get();
    old->last_used_frame = frame;
    if (old->height >= min_height) {
      return old;
    }
  }
  auto rt = std::make_unique<RenderTarget>();
  rt->key = key;
  rt->width = std::max(key.pitch, 1u);
  rt->height = rex::align(min_height, 16u);
  rt->samples = VkSampleCountFlagBits(1u << key.msaa);
  rt->format = key.is_depth ? DepthFormatFor(key.format)
                            : ColorFormatToVk(xenos::ColorRenderTargetFormat(key.format));
  rt->last_used_frame = frame;
  VkImageCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = rt->format;
  info.extent = {rt->width, rt->height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = rt->samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = (key.is_depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT
                             : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          device, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, rt->image,
          rt->memory)) {
    REXLOG_ERROR("[native] Could not create a {}x{} render target", rt->width, rt->height);
    return old;
  }
  VkImageAspectFlags aspect =
      key.is_depth ? (HasStencil(rt->format)
                          ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                          : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT))
                   : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageViewCreateInfo view = {};
  view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view.image = rt->image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = rt->format;
  view.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange(aspect);
  dfn().vkCreateImageView(vk, &view, nullptr, &rt->view);
  if (key.is_depth) {
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    dfn().vkCreateImageView(vk, &view, nullptr, &rt->sample_view);
    if (depth_words_pipeline && HasStencil(rt->format)) {
      view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
      dfn().vkCreateImageView(vk, &view, nullptr, &rt->stencil_view);
    }
    view.subresourceRange.aspectMask = aspect;
  }

  // Render targets live in GENERAL. A new one starts cleared, or with the
  // content of the smaller one it replaces.
  BeginRecording();
  EndPass(PassEnd::TargetCreate);
  VkCommandBuffer c = cmd();
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = rt->image;
  barrier.subresourceRange = view.subresourceRange;
  dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
  if (key.is_depth) {
    VkClearDepthStencilValue clear = {1.0f, 0};
    vkCmdClearDepthStencilImage(c, rt->image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                                      &view.subresourceRange);
  } else {
    VkClearColorValue clear = {};
    dfn().vkCmdClearColorImage(c, rt->image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                               &view.subresourceRange);
  }
  if (old) {
    VkImageCopy copy = {};
    copy.srcSubresource = {aspect, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {std::min(old->width, rt->width), old->height, 1};
    VkMemoryBarrier memory_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                      VK_ACCESS_TRANSFER_WRITE_BIT |
                                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory_barrier, 0, nullptr,
                               0, nullptr);
    Extra(device).vkCmdCopyImage(c, old->image, VK_IMAGE_LAYOUT_GENERAL, rt->image,
                         VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    // Destroyed once this frame is done on the GPU (waiting for idle here
    // was a visible hitch).
    retired_render_targets.push_back(
        {frame, old->image, old->view, old->sample_view, old->stencil_view, old->memory});
  }
  VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             0, 1, &after, 0, nullptr, 0, nullptr);
  RenderTarget* result = rt.get();
  render_targets[key] = std::move(rt);
  ++stats().render_targets_created;
  ++rt_generation;
  REXLOG_INFO("[native] Render target {}x{} {} format {} at tile {}, {}x MSAA", result->width,
              result->height, key.is_depth ? "depth" : "color", key.format, key.base_tiles,
              1u << key.msaa);
  return result;
}

bool Renderer::Impl::BeginPass(const PassTargets& targets) {
  if (in_pass) {
    // Keep the scope if every attachment the draw needs is already bound at
    // the same slot.
    bool compatible = true;
    for (uint32_t i = 0; i < 4; ++i) {
      if (targets.color[i] && targets.color[i] != pass.color[i]) compatible = false;
    }
    if (targets.depth && targets.depth != pass.depth) compatible = false;
    if (compatible) {
      return true;
    }
    EndPass(PassEnd::TargetChange);
  }
  FlushResolveBarrier();
  ProfileMark((kProfilePass << 60) | (ProfileTargetBits(targets.color[0]) << 24) |
              ProfileTargetBits(targets.depth));
  pass = targets;
  VkRenderingAttachmentInfo colors[4] = {};
  uint32_t color_count = 0;
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  for (uint32_t i = 0; i < 4; ++i) {
    colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (RenderTarget* rt = targets.color[i]) {
      colors[i].imageView = rt->view;
      colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_count = i + 1;
      width = std::min(width, rt->width);
      height = std::min(height, rt->height);
    }
  }
  VkRenderingAttachmentInfo depth = {};
  depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  if (targets.depth) {
    depth.imageView = targets.depth->view;
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    width = std::min(width, targets.depth->width);
    height = std::min(height, targets.depth->height);
  }
  if (width == UINT32_MAX) {
    return false;
  }
  VkRenderingInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  info.renderArea.extent = {width, height};
  info.layerCount = 1;
  info.colorAttachmentCount = color_count;
  info.pColorAttachments = colors;
  info.pDepthAttachment = targets.depth ? &depth : nullptr;
  info.pStencilAttachment =
      targets.depth && HasStencil(targets.depth->format) ? &depth : nullptr;
  dfn().vkCmdBeginRendering(cmd(), &info);
  in_pass = true;
  pass_extent = {width, height};
  ++stats().render_passes;
  return true;
}

// --- System constants ----------------------------------------------------------------

void Renderer::Impl::UpdateSystemConstants(bool primitive_polygonal,
                                           const draw_util::ViewportInfo& viewport,
                                           uint32_t used_texture_mask, uint32_t,
                                           const VulkanShader* vs, const VulkanShader* ps) {
  // VulkanCommandProcessor::UpdateSystemConstantValues for host render
  // targets, without EDRAM, tessellation or memory export.
  const RegisterFile& r = *regs;
  auto pa_cl_clip_cntl = r.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = r.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_colorcontrol = r.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = r.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = r.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = r.Get<reg::VGT_DRAW_INITIATOR>();
  SpirvShaderTranslator::SystemConstants& sc = system_constants;

  uint32_t flags = 0;
  if (pa_cl_vte_cntl.vtx_xy_fmt) flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  if (pa_cl_vte_cntl.vtx_z_fmt) flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  if (pa_cl_vte_cntl.vtx_w0_fmt) flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  if (primitive_polygonal) flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(r)) flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  flags |= HostMsaa(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  xenos::CompareFunction alpha_test = rb_colorcontrol.alpha_test_enable
                                          ? rb_colorcontrol.alpha_func
                                          : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  for (uint32_t i = 0; i < 4; ++i) {
    auto color_info = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    }
    // Exponent bias.
    int32_t exp_bias = color_info.color_exp_bias;
    // (No -5 for k_16_16 / k_16_16_16_16: they are stored as floats with the guest's
    // [-32, 32] values, see ColorFormatToVk.)
    float scale;
    uint32_t scale_bits = UINT32_C(0x3F800000) + (uint32_t(exp_bias) << 23);
    std::memcpy(&scale, &scale_bits, sizeof(scale));
    sc.color_exp_bias[i] = scale;
  }
  sc.flags = flags;
  sc.vertex_index_endian = xenos::Endian::kNone;
  sc.line_loop_closing_index = 0;
  sc.vertex_base_index = r.Get<int32_t>(rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET);
  sc.vertex_index_min = r[rex::graphics::XE_GPU_REG_VGT_MIN_VTX_INDX];
  sc.vertex_index_max = r[rex::graphics::XE_GPU_REG_VGT_MAX_VTX_INDX];
  sc.tessellation_factor_range_min =
      r.Get<float>(rex::graphics::XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  sc.tessellation_factor_range_max =
      r.Get<float>(rex::graphics::XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  sc.vertex_index_reset = 0;
  sc.compute_memexport_vertex_count = 0;
  if (!pa_cl_clip_cntl.clip_disable) {
    float* planes = sc.user_clip_planes[0];
    uint32_t remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t index;
    while (rex::bit_scan_forward(remaining, &index)) {
      remaining &= ~(1u << index);
      std::memcpy(planes, &r[rex::graphics::XE_GPU_REG_PA_CL_UCP_0_X + index * 4],
                  4 * sizeof(float));
      planes += 4;
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    sc.ndc_scale[i] = viewport.ndc_scale[i];
    sc.ndc_offset[i] = viewport.ndc_offset[i];
  }
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto minmax = r.Get<reg::PA_SU_POINT_MINMAX>();
    auto size = r.Get<reg::PA_SU_POINT_SIZE>();
    sc.point_vertex_diameter_min = float(minmax.min_size) * (2.0f / 16.0f);
    sc.point_vertex_diameter_max = float(minmax.max_size) * (2.0f / 16.0f);
    sc.point_constant_diameter[0] = float(size.width) * (2.0f / 16.0f);
    sc.point_constant_diameter[1] = float(size.height) * (2.0f / 16.0f);
    sc.point_screen_diameter_to_ndc_radius[0] =
        1.0f / float(std::max(viewport.xy_extent[0], 1u));
    sc.point_screen_diameter_to_ndc_radius[1] =
        1.0f / float(std::max(viewport.xy_extent[1], 1u));
  }
  uint32_t remaining = used_texture_mask;
  uint32_t index;
  while (rex::bit_scan_forward(remaining, &index)) {
    remaining &= ~(1u << index);
    uint32_t& word = sc.texture_swizzled_signs[index >> 2];
    uint32_t shift = 8 * (index & 3);
    uint8_t signs = rex::graphics::texture_util::SwizzleSigns(r.GetTextureFetch(index));
    word = (word & ~(0xFFu << shift)) | (uint32_t(signs) << shift);
  }
  sc.textures_resolution_scaled = 0;
  sc.alpha_test_reference = r.Get<float>(rex::graphics::XE_GPU_REG_RB_ALPHA_REF);
  sc.alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                         ? (rb_colorcontrol.value >> 24) | (UINT32_C(1) << 8)
                         : 0;
  (void)vs;
  (void)ps;
}

// --- Draws -------------------------------------------------------------------------------

// The render state of a draw, rebuilt only when a register that can change it
// is written (Renderer::state_generation_), the shaders or the primitive type
// change, or a render target is created. Consecutive draws almost always share
// it, and rebuilding it per draw was most of the renderer's CPU time.
// Mercenaries 2 diagnostic (HANDOFF section 53): logs the setups of draws with one pixel
// shader (p2_native_trace_ps, default the water's), at most 40 lines per run.
// Diagnostic only: setup state, not a GPU readback or proof of actual texel contents.
// Register LOD 1 is established for this captured VS; fetch bias/filter may compensate it.
void Renderer::Impl::TraceWaterLodSetup() {
  if (!REXCVAR_GET(p2_native_water_lod_probe) || !active_vs || !active_ps ||
      active_vs->ucode_data_hash() != UINT64_C(0xFB0614F28843002E)) return;
  const uint64_t ps_hash = active_ps->ucode_data_hash();
  unsigned slot;
  if (ps_hash == UINT64_C(0x9AD799B034BB706F)) slot = 0;
  else if (ps_hash == UINT64_C(0x5715131CCF8FC5A1)) slot = 1;
  else return;
  static std::array<uint32_t, 2> counts{};
  static std::array<std::chrono::steady_clock::time_point, 2> next{};
  if (counts[slot] >= 32) return;
  auto now = std::chrono::steady_clock::now();
  if (now < next[slot]) return;
  next[slot] = now + std::chrono::seconds(2);
  const uint32_t sample = ++counts[slot];
  const RegisterFile& r = *regs;
  const auto fetch = r.GetTextureFetch(16);
  uint32_t raw[6];
  std::memcpy(raw, &fetch, sizeof(raw));
  uint32_t w, h, depth, base, mip, min_level, max_level;
  rex::graphics::texture_util::GetSubresourcesFromFetchConstant(
      fetch, &w, &h, &depth, &base, &mip, &min_level, &max_level);
  const auto* dest = textures.FindResolveDestination(base << 12);
  REXLOG_INFO("[native] WATER_LOD_SETUP frame={} submission={} sample={} ps={:016X} "
              "fetch16={:08X},{:08X},{:08X},{:08X},{:08X},{:08X} "
              "base={:08X} mip={:08X} size={}x{}x{} format={} mip_min={} mip_max={} "
              "fetch_bias={} register_lod=1 pre_sampler_lod={} "
              "resolved={} resolved_size={}x{} resolved_format={} resolved_mips={} written_frame={}",
              frame, submission_number, sample, ps_hash,
              raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
              base << 12, mip << 12, w + 1, h + 1, depth + 1, uint32_t(fetch.format),
              min_level, max_level, float(fetch.lod_bias) / 32.0f,
              1.0f + float(fetch.lod_bias) / 32.0f,
              dest != nullptr, dest ? dest->width : 0, dest ? dest->height : 0,
              dest ? uint32_t(dest->format) : 0, dest ? 1 : 0,
              dest ? dest->last_written_frame : 0);
  for (const auto& binding : active_vs->GetSamplerBindingsAfterTranslation()) {
    if (binding.fetch_constant != 16) continue;
    const auto effective_mip = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                   ? fetch.mip_filter : binding.mip_filter;
    REXLOG_INFO("[native] WATER_LOD_SAMPLER frame={} sample={} ps={:016X} "
                "binding_mag={} binding_min={} binding_mip={} binding_aniso={} "
                "fetch_mag={} fetch_min={} fetch_mip={} fetch_aniso={} "
                "effective_mip={} base_map={} sampler_min_lod={} "
                "base_map_max_lod={} clamp_xyz={},{},{}",
                frame, sample, ps_hash,
                uint32_t(binding.mag_filter), uint32_t(binding.min_filter),
                uint32_t(binding.mip_filter), uint32_t(binding.aniso_filter),
                uint32_t(fetch.mag_filter), uint32_t(fetch.min_filter),
                uint32_t(fetch.mip_filter), uint32_t(fetch.aniso_filter),
                uint32_t(effective_mip), effective_mip == xenos::TextureFilter::kBaseMap,
                min_level, float(min_level) + 0.25f,
                uint32_t(fetch.clamp_x), uint32_t(fetch.clamp_y), uint32_t(fetch.clamp_z));
  }
  std::string constants;
  for (uint32_t c : {0u, 1u, 2u, 3u, 53u, 54u, 55u, 56u, 57u, 58u, 253u, 254u, 255u}) {
    uint32_t index = rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4;
    constants += fmt::format(" c{}={:08X},{:08X},{:08X},{:08X}", c,
                             r[index], r[index + 1], r[index + 2], r[index + 3]);
  }
  REXLOG_INFO("[native] WATER_LOD_CONSTANTS frame={} sample={} ps={:016X} "
              "vs_generation={} guest_raw_hex{}", frame, sample, ps_hash,
              owner->vs_float_generation_, constants);
}

void Renderer::Impl::TraceSetup(uint32_t reason) {
  if (reason == UINT32_MAX) TraceWaterLodSetup();
  static uint64_t traced_ps = 0;
  static bool parsed = false;
  if (!parsed) {
    parsed = true;
    traced_ps = std::strtoull(REXCVAR_GET(p2_native_trace_ps).c_str(), nullptr, 16);
  }
  bool signed_target = false;
  for (uint32_t i = 0; i < 4; ++i) {
    const RenderTarget* rt = setup.targets.color[i];
    if (rt && !rt->key.is_depth &&
        (rt->key.format == uint32_t(xenos::ColorRenderTargetFormat::k_16_16) ||
         rt->key.format == uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16))) {
      signed_target = true;
    }
  }
  const bool water = traced_ps && active_ps && active_ps->ucode_data_hash() == traced_ps;
  static uint64_t traced_vs = 0;
  static bool vs_parsed = false;
  if (!vs_parsed) {
    vs_parsed = true;
    traced_vs = std::strtoull(REXCVAR_GET(p2_native_trace_vs).c_str(), nullptr, 16);
  }
  // Every setup of draws with the traced vertex shader (the other water parts may use
  // other pixel shaders or be skipped), with their own budget.
  const bool vs_match = traced_vs && active_vs && active_vs->ucode_data_hash() == traced_vs;
  if (vs_match && !water) {
    static uint32_t vs_lines = 0;
    if (vs_lines < 60) {
      ++vs_lines;
      reg::VGT_DRAW_INITIATOR initiator;
      initiator.value = last_initiator_value;
      const RegisterFile& rr = *regs;
      REXLOG_INFO("[native] Trace water-VS VS {:016X} PS {:016X}: {} | prim {} count {} source {} "
                  "| color mask {:08X} z {} stencil {} | targets {}{}",
                  traced_vs, active_ps ? active_ps->ucode_data_hash() : 0,
                  reason == UINT32_MAX ? std::string("drawn")
                                       : fmt::format("skipped reason {}", reason),
                  uint32_t(initiator.prim_type), uint32_t(initiator.num_indices),
                  uint32_t(initiator.source_select), rr[rex::graphics::XE_GPU_REG_RB_COLOR_MASK],
                  uint32_t(draw_util::GetNormalizedDepthControl(rr).z_enable),
                  uint32_t(draw_util::GetNormalizedDepthControl(rr).stencil_enable),
                  setup.targets.color[0] ? "C0 " : "", setup.targets.depth ? "D" : "");
    }
  }
  if (!water && !(signed_target && reason == UINT32_MAX)) return;
  static uint32_t lines = 0, signed_lines = 0;
  if (water ? lines >= 40 : signed_lines >= 60) return;
  ++(water ? lines : signed_lines);
  const RegisterFile& r = *regs;
  const DrawSetup& st = setup;
  auto surface = r.Get<reg::RB_SURFACE_INFO>();
  auto depth = draw_util::GetNormalizedDepthControl(r);
  std::string targets;
  for (uint32_t i = 0; i < 4; ++i) {
    if (st.targets.color[i]) {
      targets += fmt::format(" C{}:fmt{}@{}/{}x{}", i, st.targets.color[i]->key.format,
                             st.targets.color[i]->key.base_tiles, st.targets.color[i]->width,
                             st.targets.color[i]->height);
    }
  }
  if (st.targets.depth) {
    targets += fmt::format(" D:fmt{}@{}", st.targets.depth->key.format,
                           st.targets.depth->key.base_tiles);
  }
  std::string fetches;
  auto describe = [&](const char* stage, const VulkanShader* shader) {
    if (!shader) return;
    for (const auto& b : shader->GetTextureBindingsAfterTranslation()) {
      auto fetch = r.GetTextureFetch(b.fetch_constant);
      uint32_t address = fetch.base_address << 12;
      const ResolveDestination* resolved =
          textures.FindResolveDestination(address & 0x1FFFFFFF);
      if (water) water_texture_addresses.insert(address & 0x1FFFFFFF);
      fetches += fmt::format(" {}t{}:fmt{} {:08X} {}x{} exp{} signs{}{}{}{}{}", stage,
                             b.fetch_constant, uint32_t(fetch.format), address,
                             fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                             int32_t(fetch.exp_adjust), uint32_t(fetch.sign_x),
                             uint32_t(fetch.sign_y), uint32_t(fetch.sign_z),
                             uint32_t(fetch.sign_w),
                             resolved ? fmt::format(" resolved(vk{})", uint32_t(resolved->format))
                                      : std::string());
    }
  };
  describe("VS", active_vs);
  describe("PS", active_ps);
  REXLOG_INFO("[native] Trace {} VS {:016X} PS {:016X}: {} | msaa {} pitch {} | z {} write {} func {} stencil {} | "
              "blend0 {:08X} mask {:08X} | scissor {},{} {}x{} viewport {},{} {}x{} window {},{} "
              "on {} | clip cntl {:08X} vte {:08X} ucp {} |{} |{}",
              water ? "water" : "signed-target", active_vs ? active_vs->ucode_data_hash() : 0,
              active_ps ? active_ps->ucode_data_hash() : 0,
              reason == UINT32_MAX ? std::string("drawn")
                                             : fmt::format("skipped reason {}", reason),
              uint32_t(surface.msaa_samples), uint32_t(surface.surface_pitch), uint32_t(depth.z_enable),
              uint32_t(depth.z_write_enable), uint32_t(depth.zfunc), uint32_t(depth.stencil_enable),
              r.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]).value,
              r[rex::graphics::XE_GPU_REG_RB_COLOR_MASK], st.scissor.offset[0],
              st.scissor.offset[1], st.scissor.extent[0], st.scissor.extent[1],
              st.viewport.xy_offset[0], st.viewport.xy_offset[1], st.viewport.xy_extent[0],
              st.viewport.xy_extent[1],
              r.Get<reg::PA_SC_WINDOW_OFFSET>().window_x_offset,
              r.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset,
              uint32_t(r.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable),
              r[rex::graphics::XE_GPU_REG_PA_CL_CLIP_CNTL], r[rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL],
              [&] {
                std::string planes;
                for (uint32_t i = 0; i < 6; ++i) {
                  if (!(r.Get<reg::PA_CL_CLIP_CNTL>().ucp_ena & (1u << i))) continue;
                  planes += fmt::format(" [{}: {:.3f} {:.3f} {:.3f} {:.3f}]", i,
                                        r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_UCP_0_X + i * 4),
                                        r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_UCP_0_X + i * 4 + 1),
                                        r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_UCP_0_X + i * 4 + 2),
                                        r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_UCP_0_X + i * 4 + 3));
                }
                return planes.empty() ? std::string("none") : planes;
              }(),
              targets, fetches);
}

void Renderer::Impl::UpdateWaterDebug() {
  static int enabled = -1;
  if (enabled < 0) enabled = REXCVAR_GET(p2_native_water_debug_cycle) ? 1 : 0;
  if (!enabled || water_debug_step >= 8) return;
  const auto now = std::chrono::steady_clock::now();
  if (water_debug_next == std::chrono::steady_clock::time_point{}) {
    water_debug_next = now + std::chrono::seconds(8);
    REXLOG_INFO("[native] Water debug: mode 0 (normal)");
    return;
  }
  if (!water_debug_shot_taken && now + std::chrono::seconds(4) >= water_debug_next) {
    water_debug_shot_taken = true;
    if (RexSwitchScreenshotRequest) RexSwitchScreenshotRequest();
    REXLOG_INFO("[native] Water debug: screenshot of mode {}", water_debug_mode);
  }
  if (now < water_debug_next) return;
  ++water_debug_step;
  water_debug_mode = water_debug_step >= 8 ? 0 : water_debug_step & 3;
  water_debug_next = now + std::chrono::seconds(8);
  water_debug_shot_taken = false;
  ++rt_generation;  // Rebuild the cached draw setups with the new mode.
  REXLOG_INFO("[native] Water debug: mode {} ({})", water_debug_mode,
              water_debug_mode == 0   ? "normal"
              : water_debug_mode == 1 ? "no depth/stencil test"
              : water_debug_mode == 2 ? "magenta pixel shader"
                                      : "magenta, no depth/stencil test");
}

void Renderer::Impl::CountNoTargetSkip() {
  if (no_target_reports >= 4) return;
  const RegisterFile& r = *regs;
  NoTargetKey key = {};
  key.vs = active_vs ? active_vs->ucode_data_hash() : 0;
  key.ps = active_ps ? active_ps->ucode_data_hash() : 0;
  key.color_mask = r[rex::graphics::XE_GPU_REG_RB_COLOR_MASK];
  key.depth_control = r[rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL];
  key.edram_mode = uint32_t(r.Get<reg::RB_MODECONTROL>().edram_mode);
  key.ps_writes = active_ps ? active_ps->writes_color_targets() : 0xFFFFFFFFu;
  ++no_target_counts[key];
  const auto now = std::chrono::steady_clock::now();
  if (no_target_report == std::chrono::steady_clock::time_point{}) {
    no_target_report = now + std::chrono::seconds(20);
    return;
  }
  if (now < no_target_report) return;
  no_target_report = now + std::chrono::seconds(20);
  ++no_target_reports;
  std::vector<std::pair<NoTargetKey, uint64_t>> top(no_target_counts.begin(),
                                                    no_target_counts.end());
  std::sort(top.begin(), top.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  REXLOG_INFO("[native] No-target skips: {} distinct states", top.size());
  for (size_t i = 0; i < std::min<size_t>(top.size(), 25); ++i) {
    const NoTargetKey& k = top[i].first;
    REXLOG_INFO("[native]   no-target VS {:016X} PS {:016X}: {} setups | color mask {:08X} "
                "depthcontrol {:08X} edram mode {} ps writes {:X}",
                k.vs, k.ps, top[i].second, k.color_mask, k.depth_control, k.edram_mode,
                k.ps_writes);
  }
  no_target_counts.clear();
}

bool Renderer::Impl::BuildDrawSetup(uint32_t initiator_value) {
  DrawSetup& st = setup;
  st = DrawSetup();
  st.valid = true;
  st.state_generation = owner->state_generation_;
  st.fetch_generation = owner->fetch_generation_;
  st.rt_generation = rt_generation;
  st.active_vs = active_vs;
  st.active_ps = active_ps;
  st.initiator_state = initiator_value & 0xFFFF;
  last_initiator_value = initiator_value;
  st.skip_reason = UINT32_MAX;

  const RegisterFile& r = *regs;
  const auto& properties = device->properties();
  auto fail = [&](uint32_t reason) {
    st.skip_reason = reason;
    TraceSetup(reason);
    if (reason == 7) CountNoTargetSkip();
    return false;
  };

  xenos::EdramMode edram_mode = r.Get<reg::RB_MODECONTROL>().edram_mode;
  auto surface_info = r.Get<reg::RB_SURFACE_INFO>();
  if (!surface_info.surface_pitch) return fail(0);
  VulkanShader* vs = active_vs;
  if (!vs) return fail(1);
  if (!vs->is_ucode_analyzed()) vs->AnalyzeUcode(disasm);
  if (vs->memexport_eM_written()) return fail(2);
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(r);
  if (!draw_util::IsRasterizationPotentiallyDone(r, primitive_polygonal)) return fail(3);
  VulkanShader* ps = nullptr;
  if (edram_mode == xenos::EdramMode::kColorDepth && active_ps) {
    ps = active_ps;
    if (!ps->is_ucode_analyzed()) ps->AnalyzeUcode(disasm);
    if (ps->memexport_eM_written()) return fail(2);
    if (!draw_util::IsPixelShaderNeededWithRasterization(*ps, r)) ps = nullptr;
  }
  reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(r);
  uint32_t color_mask = draw_util::GetNormalizedColorMask(r, ps ? ps->writes_color_targets() : 0);

  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = initiator_value;
  xenos::PrimitiveType guest_type = initiator.prim_type;
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
  PipelineGeometryShader geometry = PipelineGeometryShader::kNone;
  // Mercenaries 2: tessellated draws (the water), as PrimitiveProcessor detects them.
  tessellation::HostType host_vs_type = tessellation::HostType::kVertex;
  const xenos::TessellationMode tess_mode = r.Get<reg::VGT_HOS_CNTL>().tess_mode;
  const bool tessellated =
      xenos::IsMajorModeExplicit(initiator.major_mode, initiator.prim_type) &&
      r.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select == xenos::VGTOutputPath::kTessellationEnable;
  uint32_t patch_points = 0;
  if (tessellated) {
    const bool type_ok = tessellation::GetHostVertexShaderType(guest_type, tess_mode, host_vs_type);
    patch_points = type_ok ? tessellation::PatchControlPoints(host_vs_type, tess_mode) : 0;
    const bool adaptive = tess_mode == xenos::TessellationMode::kAdaptive;
    const bool indices_ok =
        !adaptive || (initiator.source_select != xenos::SourceSelect::kAutoIndex &&
                      initiator.index_size == xenos::IndexFormat::kInt32);
    if (tessellation_logged.insert(vs->ucode_data_hash()).second) {
      REXLOG_INFO("[native] Tessellated draw: VS {:016X}, primitive {}, mode {}, host type {}, "
                  "{} control points, {} vertices, {}{}",
                  vs->ucode_data_hash(), uint32_t(guest_type), uint32_t(tess_mode),
                  uint32_t(host_vs_type), patch_points, uint32_t(initiator.num_indices),
                  initiator.source_select == xenos::SourceSelect::kAutoIndex ? "auto-indexed"
                                                                             : "indexed",
                  tessellation_supported && patch_points && indices_ok ? "" : " (unsupported)");
    }
    if (!tessellation_supported || !patch_points || !indices_ok) return fail(14);
  } else switch (guest_type) {
    case xenos::PrimitiveType::kPointList:
      topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      geometry = PipelineGeometryShader::kPointList;
      break;
    case xenos::PrimitiveType::kLineList:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      break;
    case xenos::PrimitiveType::kLineStrip:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      st.strip = true;
      break;
    case xenos::PrimitiveType::kLineLoop:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      st.convert_loop = true;
      break;
    case xenos::PrimitiveType::kTriangleList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      st.strip = true;
      break;
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kPolygon:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      st.convert_fan = true;
      break;
    case xenos::PrimitiveType::kRectangleList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      geometry = PipelineGeometryShader::kRectangleList;
      break;
    case xenos::PrimitiveType::kQuadList:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
      geometry = PipelineGeometryShader::kQuadList;
      break;
    default:
      return fail(4);
  }
  if (geometry != PipelineGeometryShader::kNone && !properties.geometryShader) return fail(5);
  bool indexed = initiator.source_select != xenos::SourceSelect::kAutoIndex;
  st.reset_enabled =
      indexed && st.strip && r.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena;

  // Shader modifications (VulkanPipelineCache::GetCurrent*ShaderModification).
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      ps ? (vs->writes_interpolators() &
            ps->GetInterpolatorInputMask(r.Get<reg::SQ_PROGRAM_CNTL>(),
                                         r.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos))
         : 0;
  SpirvShaderTranslator::Modification vs_mod(translator->GetDefaultVertexShaderModification(
      vs->GetDynamicAddressableRegisterCount(r.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
      host_vs_type));
  vs_mod.vertex.interpolator_mask = interpolator_mask;
  if (tessellated) {
    vs_mod.vertex.tessellation_mode = uint32_t(tess_mode);
  }
  auto pa_cl_clip_cntl = r.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  // Diagnostic (HANDOFF section 61): no user clip planes for the traced (water) shader.
  if (user_clip_planes && REXCVAR_GET(p2_native_water_no_ucp) && ps) {
    static uint64_t water_ps_ucp = 0;
    static bool water_ps_ucp_parsed = false;
    if (!water_ps_ucp_parsed) {
      water_ps_ucp_parsed = true;
      water_ps_ucp = std::strtoull(REXCVAR_GET(p2_native_trace_ps).c_str(), nullptr, 16);
    }
    if (ps->ucode_data_hash() == water_ps_ucp) user_clip_planes = 0;
  }
  if (user_clip_planes) {
    if (pa_cl_clip_cntl.ucp_cull_only_ena && properties.shaderCullDistance) {
      vs_mod.vertex.user_clip_plane_cull = 1;
    } else if (!properties.shaderClipDistance && properties.shaderCullDistance) {
      vs_mod.vertex.user_clip_plane_cull = 1;
    } else if (!properties.shaderClipDistance) {
      user_clip_planes = 0;
    }
  }
  vs_mod.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  vs_mod.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;
  vs_mod.vertex.output_point_parameters =
      uint32_t((vs->writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               guest_type == xenos::PrimitiveType::kPointList);
  vs_mod.vertex.vertex_kill_and =
      uint32_t(properties.shaderCullDistance &&
               (vs->writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !pa_cl_clip_cntl.vtx_kill_or);
  SpirvShaderTranslator::Modification ps_mod(0);
  if (ps) {
    ps_mod = SpirvShaderTranslator::Modification(translator->GetDefaultPixelShaderModification(
        ps->GetDynamicAddressableRegisterCount(r.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));
    ps_mod.pixel.interpolator_mask = interpolator_mask;
    ps_mod.pixel.interpolators_centroid =
        interpolator_mask &
        ~xenos::GetInterpolatorSamplingPattern(surface_info.msaa_samples,
                                               r.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                                               r.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    if (ps_param_gen_pos < xenos::kMaxInterpolators) {
      ps_mod.pixel.param_gen_enable = 1;
      ps_mod.pixel.param_gen_interpolator = ps_param_gen_pos;
      ps_mod.pixel.param_gen_point = uint32_t(guest_type == xenos::PrimitiveType::kPointList);
    }
    using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
    if (ps->implicit_early_z_write_allowed() &&
        (!ps->writes_color_target(0) ||
         !draw_util::DoesCoverageDependOnAlpha(r.Get<reg::RB_COLORCONTROL>()))) {
      ps_mod.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
    } else {
      ps_mod.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
    }
  }
  // Mercenaries 2 (HANDOFF section 63): a pixel shader that reads its pixel position (VPOS)
  // on a surface drawn with samples as pixels (SampleScaleX/Y) must get guest pixel
  // coordinates: such variants are marked with spare modification bits 51 (x / 2) and
  // 52 (y / 2) and translated by a translator with that draw resolution scale.
  if (ps && ps_mod.pixel.param_gen_enable) {
    if (SampleScaleX(surface_info.msaa_samples) > 1) ps_mod.value |= kVposScaleXBit;
    if (SampleScaleY(surface_info.msaa_samples) > 1) ps_mod.value |= kVposScaleYBit;
  }
  auto* vs_translation =
      static_cast<VulkanShader::VulkanTranslation*>(vs->GetOrCreateTranslation(vs_mod.value));
  auto* ps_translation =
      ps ? static_cast<VulkanShader::VulkanTranslation*>(ps->GetOrCreateTranslation(ps_mod.value))
         : nullptr;
  {
    bool vs_pending = false, ps_pending = false;
    bool vs_ok = EnsureTranslated(vs_translation, &vs_pending);
    bool ps_ok = !ps_translation || EnsureTranslated(ps_translation, &ps_pending);
    if (!vs_ok || !ps_ok) {
      // Still being translated: look again on the next draw.
      if (vs_pending || ps_pending) st.valid = false;
      return fail(6);
    }
  }

  // Render targets: the color targets the draw writes and the depth buffer
  // if it is tested or written.
  bool depth_used = depth_control.z_enable || depth_control.stencil_enable;
  // The guest's sample count sizes the EDRAM below; the host's keys the targets.
  uint32_t guest_msaa = uint32_t(surface_info.msaa_samples);
  uint32_t msaa = HostMsaa(surface_info.msaa_samples);
  const uint32_t sample_x = SampleScaleX(surface_info.msaa_samples);
  const uint32_t sample_y = SampleScaleY(surface_info.msaa_samples);
  draw_util::GetScissor(r, st.scissor);
  st.scissor.offset[0] *= sample_x;
  st.scissor.extent[0] *= sample_x;
  st.scissor.offset[1] *= sample_y;
  st.scissor.extent[1] *= sample_y;
  // HANDOFF B:117: full_float24_in_0_to_1 = false. With true, float24 (D24FS8 = the shadow maps)
  // depth went to the host as guest * 0.5 (Xenia's [0, 2) EDRAM round-trip remap), while the shadow
  // atlas copy, the depth clears and the receivers all use the guest [0, 1] value: the atlas held half
  // the depth, so shadows vanished whenever the game's light fit put the ground away from 0 (camera
  // looking down). This renderer never moves float24 data through EDRAM, so the remap is not needed.
  draw_util::GetHostViewportInfo(r, sample_x, sample_y, false, properties.maxViewportDimensions[0],
                                 properties.maxViewportDimensions[1], true, depth_control, false,
                                 false, ps && ps->writes_depth(), st.viewport);
  // Rows the draw can touch: the viewport, cut by the scissor. The scissor
  // alone is often the 8192-pixel default and made every target 2048 tall.
  uint32_t needed_height = st.scissor.offset[1] + st.scissor.extent[1];
  if (st.viewport.xy_extent[1]) {
    needed_height =
        std::min(needed_height, st.viewport.xy_offset[1] + st.viewport.xy_extent[1]);
  }
  PassTargets& targets = st.targets;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!((color_mask >> (4 * i)) & 0xF)) continue;
    auto color_info = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    RenderTargetKey key = {};
    key.base_tiles = color_info.color_base;
    key.pitch = surface_info.surface_pitch * sample_x;
    // Mercenaries 2: keyed by storage format. The scene is drawn as
    // 2_10_10_10_FLOAT_AS_16_16_16_16 and resolved as 2_10_10_10_FLOAT (same EDRAM bits);
    // separate images made the resolve read an empty target: black 3D (run17, HANDOFF Р вЂ™Р’В§49).
    key.format = RtKeyColorFormat(color_info.color_format);
    key.msaa = msaa;
    // The EDRAM below the base limits how tall the target can be.
    uint32_t pitch_tiles = (surface_info.surface_pitch * (guest_msaa >= 2 ? 2 : 1) + 79) / 80 *
                           (IsColorFormat64bpp(color_info.color_format) ? 2 : 1);
    uint32_t rows = pitch_tiles ? (2048 - std::min(key.base_tiles, 2047u)) / pitch_tiles *
                                      (guest_msaa >= 1 ? 8 : 16) * sample_y
                                : 2048;
    targets.color[i] = GetRenderTarget(key, std::min(needed_height, rows));
  }
  if (depth_used) {
    auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
    RenderTargetKey key = {};
    key.base_tiles = depth_info.depth_base;
    key.pitch = surface_info.surface_pitch * sample_x;
    key.format = uint32_t(depth_info.depth_format);
    key.msaa = msaa;
    key.is_depth = 1;
    uint32_t pitch_tiles = (surface_info.surface_pitch * (guest_msaa >= 2 ? 2 : 1) + 79) / 80;
    uint32_t rows = pitch_tiles ? (2048 - std::min(key.base_tiles, 2047u)) / pitch_tiles *
                                      (guest_msaa >= 1 ? 8 : 16) * sample_y
                                : 2048;
    targets.depth = GetRenderTarget(key, std::min(needed_height, rows));
  }
  // Creating a render target above bumped rt_generation; this setup is built
  // against the new ones.
  st.rt_generation = rt_generation;
  if (targets.Empty()) return fail(7);

  // Layout.
  st.vs = vs;
  st.ps = ps;
  const VulkanShader* stage_shaders[2] = {vs, ps};
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const VulkanShader* shader = stage_shaders[stage];
    if (!shader) continue;
    st.counts[stage * 2] = uint32_t(shader->GetTextureBindingsAfterTranslation().size());
    st.counts[stage * 2 + 1] = uint32_t(shader->GetSamplerBindingsAfterTranslation().size());
  }
  st.layout = GetPipelineLayout(st.counts[0], st.counts[1], st.counts[2], st.counts[3]);
  if (!st.layout) return fail(10);

  // Pipeline.
  PipelineKey key = {};
  key.vs_hash = vs->ucode_data_hash();
  key.vs_modification = vs_mod.value;
  key.ps_hash = ps ? ps->ucode_data_hash() : 0;
  key.ps_modification = ps ? ps_mod.value : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    key.color_formats[i] = targets.color[i] ? uint32_t(targets.color[i]->format) : 0;
  }
  key.depth_format = targets.depth ? uint32_t(targets.depth->format) : 0;
  key.samples = 1u << msaa;
  key.topology = uint32_t(topology);
  key.primitive_restart = st.reset_enabled ? 1 : 0;
  const bool water_draw = [&] {
    static uint64_t water_ps = 0;
    static bool parsed = false;
    if (!parsed) {
      parsed = true;
      water_ps = std::strtoull(REXCVAR_GET(p2_native_trace_ps).c_str(), nullptr, 16);
    }
    return water_ps && ps && ps->ucode_data_hash() == water_ps;
  }();
  if (water_draw) UpdateWaterDebug();
  key.tessellation_reserved = water_draw && (water_debug_mode & 2) ? 1u : 0u;
  key.tessellation = tessellated ? uint32_t(host_vs_type) | (uint32_t(tess_mode) << 8) |
                                       (patch_points << 16)
                                 : 0;
  auto sc_mode = r.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (primitive_polygonal) {
    key.cull_mode = (sc_mode.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) |
                    (sc_mode.cull_back ? VK_CULL_MODE_BACK_BIT : 0);
    key.front_face_clockwise = sc_mode.face != 0;
    if (properties.fillModeNonSolid &&
        sc_mode.poly_mode == xenos::PolygonModeEnable::kDualMode) {
      xenos::PolygonType type = xenos::PolygonType::kTriangles;
      if (!sc_mode.cull_front) type = std::min(type, sc_mode.polymode_front_ptype);
      if (!sc_mode.cull_back) type = std::min(type, sc_mode.polymode_back_ptype);
      key.polygon_line = type != xenos::PolygonType::kTriangles;
    }
  }
  key.depth_clamp = properties.depthClamp && pa_cl_clip_cntl.clip_disable;
  // HANDOFF B:111 (p2_native_shadow_depth_clamp): the shadow cascades clamp depth instead of
  // clipping it, so casters between the sun and the cascade's near plane (tree tops, roofs, cut
  // off as the game refits the cascade to the camera) still cast their shadow.
  if (shadow_depth_clamp && properties.depthClamp && targets.depth &&
      IsShadowTarget(targets.depth)) {
    key.depth_clamp = 1;
  }
  if (targets.depth) {
    if (depth_control.z_enable) {
      key.depth_test = 1;
      key.depth_write = depth_control.z_write_enable;
      key.depth_compare = uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(depth_control.zfunc);
    }
    if (water_draw && (water_debug_mode & 1)) {
      key.depth_test = 0;
      key.depth_write = 0;
    }
    if (depth_control.stencil_enable && !(water_draw && (water_debug_mode & 1))) {
      key.stencil_test = 1;
      key.stencil_front[0] = uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilfail);
      key.stencil_front[1] = uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilzpass);
      key.stencil_front[2] = uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilzfail);
      key.stencil_front[3] = uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(depth_control.stencilfunc);
      if (primitive_polygonal && depth_control.backface_enable) {
        key.stencil_back[0] =
            uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilfail_bf);
        key.stencil_back[1] =
            uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilzpass_bf);
        key.stencil_back[2] =
            uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilzfail_bf);
        key.stencil_back[3] =
            uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(depth_control.stencilfunc_bf);
      } else {
        std::memcpy(key.stencil_back, key.stencil_front, sizeof(key.stencil_back));
      }
    }
  }
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t mask = (color_mask >> (4 * i)) & 0xF;
    if (!targets.color[i]) mask = 0;
    key.write_masks |= mask << (4 * i);
    uint32_t blend = r[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
    key.blend[i] = mask ? blend : 0x00010001;
  }
  if (specialize_signs) {
    for (const VulkanShader* shader : stage_shaders) {
      if (!shader) continue;
      for (const auto& binding : shader->texture_bindings()) {
        uint32_t fetch = binding.fetch_constant;
        uint8_t signs = rex::graphics::texture_util::SwizzleSigns(r.GetTextureFetch(fetch));
        uint32_t& word = key.texture_signs[fetch >> 2];
        uint32_t shift = 8 * (fetch & 3);
        word = (word & ~(0xFFu << shift)) | (uint32_t(signs) << shift);
      }
    }
  }
  key.layout = (uint64_t(st.counts[0]) << 48) | (uint64_t(st.counts[1]) << 32) |
               (uint64_t(st.counts[2]) << 16) | st.counts[3];
  VkShaderModule geometry_module = VK_NULL_HANDLE;
  if (geometry != PipelineGeometryShader::kNone) {
    GeometryShaderKey gs_key;
    gs_key.type = geometry;
    gs_key.interpolator_count = rex::bit_count(vs_mod.vertex.interpolator_mask);
    gs_key.user_clip_plane_count = vs_mod.vertex.user_clip_plane_count;
    gs_key.user_clip_plane_cull = vs_mod.vertex.user_clip_plane_cull;
    gs_key.has_vertex_kill_and = vs_mod.vertex.vertex_kill_and;
    gs_key.has_point_size = vs_mod.vertex.output_point_parameters;
    gs_key.has_point_coordinates = ps ? ps_mod.pixel.param_gen_point : 0;
    gs_key.point_ps_ucp_mode = vs_mod.vertex.point_ps_ucp_mode;
    key.geometry_shader = gs_key.key;
    auto it = geometry_shaders.find(gs_key.key);
    if (it == geometry_shaders.end()) {
      it = geometry_shaders.emplace(gs_key.key, CreateGeometryShader(device, gs_key)).first;
    }
    geometry_module = it->second;
    if (!geometry_module) return fail(12);
  }
  bool pending = false;
  st.pipeline = GetPipeline(key, vs_translation, ps_translation, geometry_module,
                            st.layout->layout, &pending);
  if (!st.pipeline) {
    // A pipeline being compiled: look again on the next draw.
    if (pending) st.valid = false;
    return fail(13);
  }

  // Dynamic state (VulkanCommandProcessor::UpdateDynamicState): all of it
  // comes from state registers.
  st.primitive_polygonal = primitive_polygonal;
  st.depth_control = depth_control;
  if (st.viewport.xy_extent[0] && st.viewport.xy_extent[1]) {
    st.vp.x = float(st.viewport.xy_offset[0]);
    st.vp.y = float(st.viewport.xy_offset[1]);
    st.vp.width = float(st.viewport.xy_extent[0]);
    st.vp.height = float(st.viewport.xy_extent[1]);
  } else {
    st.vp.x = -1.0f;
    st.vp.y = -1.0f;
    st.vp.width = 1.0f;
    st.vp.height = 1.0f;
  }
  st.vp.minDepth = st.viewport.z_min;
  st.vp.maxDepth = st.viewport.z_max;
  draw_util::GetPreferredFacePolygonOffset(r, primitive_polygonal, st.bias_slope,
                                           st.bias_constant);
  st.bias_constant *= r.Get<reg::RB_DEPTH_INFO>().depth_format ==
                              xenos::DepthRenderTargetFormat::kD24S8
                          ? draw_util::kD3D10PolygonOffsetFactorUnorm24
                          : draw_util::kD3D10PolygonOffsetFactorFloat24;
  st.bias_slope *= xenos::kPolygonOffsetScaleSubpixelUnit;
  st.blend_constants[0] = r.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_RED);
  st.blend_constants[1] = r.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_GREEN);
  st.blend_constants[2] = r.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_BLUE);
  st.blend_constants[3] = r.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_ALPHA);
  auto front = r.Get<reg::RB_STENCILREFMASK>();
  auto back = front;
  if (primitive_polygonal && depth_control.backface_enable) {
    back = r.Get<reg::RB_STENCILREFMASK>(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK_BF);
  }
  uint32_t stencil[6] = {front.stencilmask,      back.stencilmask, front.stencilwritemask,
                         back.stencilwritemask, front.stencilref, back.stencilref};
  std::memcpy(st.stencil, stencil, sizeof(stencil));
  TraceSetup(UINT32_MAX);
  return true;
}

void Renderer::Impl::Draw(const uint32_t* immediate_indices, uint32_t immediate_index_words) {
  const RegisterFile& r = *regs;
  // The reasons, in order: 0 no surface pitch, 1 no vertex shader, 2 memory
  // export, 3 no rasterization, 4 primitive type, 5 no geometry shaders,
  // 6 translation, 7 no render target, 8 index data, 9 stream full,
  // 10 layout, 11 descriptors, 12 geometry shader, 13 pipeline, 14 pass,
  // 15 immediate indices.
  auto skip = [&](uint32_t reason) {
    ++stats().draws_skipped;
    ++stats().skipped_by_reason[reason & 15];
  };

  auto initiator = r.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t guest_count = initiator.num_indices;
  if (!guest_count) {
    return;
  }
  // The render state: the same as the previous draw (nothing changed), or
  // one seen before (looked up by the hash of the state registers, the
  // shaders and the primitive setup), or built now.
  if (!(setup.valid && setup.state_generation == owner->state_generation_ &&
        setup.active_vs == active_vs && setup.active_ps == active_ps &&
        setup.initiator_state == (initiator.value & 0xFFFF) &&
        setup.rt_generation == rt_generation &&
        (!specialize_signs || setup.fetch_generation == owner->fetch_generation_))) {
    if (setup_cache_rt_generation != rt_generation) {
      // Setups hold render target pointers.
      setup_cache.clear();
      setup_cache_rt_generation = rt_generation;
    }
    uint64_t key = owner->state_hash_ ^
                   Renderer::Mix(0x10000, uint32_t(reinterpret_cast<uintptr_t>(active_vs))) ^
                   Renderer::Mix(0x10001, uint32_t(reinterpret_cast<uintptr_t>(active_vs) >> 32)) ^
                   Renderer::Mix(0x10002, uint32_t(reinterpret_cast<uintptr_t>(active_ps))) ^
                   Renderer::Mix(0x10003, uint32_t(reinterpret_cast<uintptr_t>(active_ps) >> 32)) ^
                   Renderer::Mix(0x10004, (initiator.value & 0xFFFF) | 0x80000000u);
    if (specialize_signs) {
      // The pipeline depends on the signs of the textures the shaders read.
      // A slot may appear in both stages and in signed + unsigned bindings.
      // XOR it once: duplicate terms would cancel and reuse a pipeline with stale signs.
      uint32_t sign_fetch_mask = 0;
      for (VulkanShader* shader : {active_vs, active_ps}) {
        if (!shader || !shader->is_ucode_analyzed()) continue;
        for (const auto& binding : shader->texture_bindings())
          sign_fetch_mask |= uint32_t(1) << binding.fetch_constant;
      }
      uint32_t fetch;
      while (rex::bit_scan_forward(sign_fetch_mask, &fetch)) {
        sign_fetch_mask &= ~(uint32_t(1) << fetch);
        key ^= Renderer::Mix(0x10010 + fetch,
                            0x100u | rex::graphics::texture_util::SwizzleSigns(
                                         r.GetTextureFetch(fetch)));
      }
    }
    auto found = setup_cache.find(key);
    if (found != setup_cache.end() && found->second.active_vs == active_vs &&
        found->second.active_ps == active_ps &&
        found->second.initiator_state == (initiator.value & 0xFFFF)) {
      setup = found->second;
      setup.state_generation = owner->state_generation_;
      setup.fetch_generation = owner->fetch_generation_;
      ++setup_generation;
    } else {
      ++stats().setups_built;
      ++setup_generation;
      BuildDrawSetup(initiator.value);
      if (setup.valid) {
        if (setup_cache.size() >= 4096) setup_cache.clear();
        setup_cache[key] = setup;
      }
    }
  }
  DrawSetup& st = setup;
  if (st.skip_reason != UINT32_MAX) {
    return skip(st.skip_reason);
  }
  if (shadow_stagger && !st.targets.color[0] && IsShadowTarget(st.targets.depth) &&
      ShadowCascadeSkipped(shadow_cascade)) {
    ++work_shadow_skipped_draws;
    return;
  }
  if (shadow_max_cascades < 4 && shadow_cascade >= shadow_max_cascades && !st.targets.color[0] &&
      IsShadowTarget(st.targets.depth) &&
      st.depth_control.zfunc != xenos::CompareFunction::kAlways) {
    // A far cascade's casters (its clear, an ALWAYS draw, still runs: the map stays empty).
    ++work_shadow_skipped_draws;
    return;
  }
  if (!skip_ps.empty() && st.active_ps && skip_ps.count(st.active_ps->ucode_data_hash())) {
    return;
  }
  if (reflection_half_rate) {
    if (st.active_ps && st.active_ps->ucode_data_hash() == UINT64_C(0x9AD799B034BB706F)) {
      reflection_texture = ((*regs).GetTextureFetch(0).base_address << 12) & 0x1FFFFFFF;
    }
    if (reflection_color_known && st.targets.color[0] &&
        st.targets.color[0]->key == reflection_color && st.targets.depth) {
      reflection_depth = st.targets.depth->key;
      reflection_depth_known = true;
    }
    if (ReflectionSkipFrame()) {
      const bool to_color = st.targets.color[0] && st.targets.color[0]->key == reflection_color;
      const bool to_depth = !st.targets.color[0] && !st.targets.color[1] && reflection_depth_known &&
                            st.targets.depth && st.targets.depth->key == reflection_depth;
      if (to_color || to_depth) {
        ++work_reflection_skipped;
        return;
      }
    }
  }
  if (skip_depth_prepass && !st.targets.color[0] && !st.targets.color[1] && !st.targets.color[2] &&
      !st.targets.color[3] && st.targets.depth && !IsShadowTarget(st.targets.depth) &&
      st.targets.depth->key.format == uint32_t(xenos::DepthRenderTargetFormat::kD24S8) &&
      !st.depth_control.stencil_enable) {
    const auto func = st.depth_control.zfunc;
    const bool writes_depth = st.depth_control.z_enable && st.depth_control.z_write_enable;
    if (!writes_depth ||
        func == xenos::CompareFunction::kLess || func == xenos::CompareFunction::kLessEqual) {
      ++work_prepass_skipped;
      return;
    }
  }
  if (edge_blur_off && st.active_ps && st.active_ps->ucode_data_hash() == kEdgeBlurPs) {
    // Its target already holds the composite it would blur (resolved from it just before).
    return;
  }
  VulkanShader* vs = st.vs;
  VulkanShader* ps = st.ps;
  const PassTargets& targets = st.targets;
  const PipelineLayout* layout = st.layout;

  // Indices.
  BeginRecording();
  auto dma_size = r.Get<reg::VGT_DMA_SIZE>();
  bool indexed = initiator.source_select != xenos::SourceSelect::kAutoIndex;
  bool index32 = initiator.index_size == xenos::IndexFormat::kInt32;
  bool reset_enabled = st.reset_enabled;
  bool convert_fan = st.convert_fan, convert_loop = st.convert_loop;
  uint32_t reset_index = r.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  const uint8_t* guest_indices = nullptr;
  xenos::Endian index_endian = dma_size.swap_mode;
  uint32_t dma_base = 0;
  if (indexed) {
    if (initiator.source_select == xenos::SourceSelect::kDMA) {
      dma_base = r[rex::graphics::XE_GPU_REG_VGT_DMA_BASE] & 0x1FFFFFFF;
      uint32_t bytes = guest_count * (index32 ? 4 : 2);
      if (uint64_t(dma_base) + bytes > 0x20000000u) {
        return skip(8);
      }
      guest_indices = memory->TranslatePhysical<const uint8_t*>(dma_base);
    } else if (immediate_indices) {
      guest_indices = reinterpret_cast<const uint8_t*>(immediate_indices);
      index_endian = xenos::Endian::kNone;
      guest_count = std::min(guest_count, immediate_index_words * (index32 ? 1u : 2u));
    } else {
      return skip(15);
    }
  }
  auto read_index = [&](uint32_t i) -> uint32_t {
    if (!indexed) {
      return i;
    }
    if (index32) {
      uint32_t v;
      std::memcpy(&v, guest_indices + size_t(i) * 4, 4);
      return xenos::GpuSwap(v, index_endian);
    }
    uint16_t v;
    std::memcpy(&v, guest_indices + size_t(i) * 2, 2);
    return xenos::GpuSwap(v, index_endian);
  };
  uint32_t draw_count = guest_count;
  // Diagnostic (HANDOFF section 62): the index data of the traced water draws.
  {
    static uint64_t water_ps_idx = 0;
    static bool water_ps_idx_parsed = false;
    static uint32_t water_idx_lines = 0;
    if (!water_ps_idx_parsed) {
      water_ps_idx_parsed = true;
      water_ps_idx = std::strtoull(REXCVAR_GET(p2_native_trace_ps).c_str(), nullptr, 16);
    }
    if (water_idx_lines < 12 && water_ps_idx && setup.active_ps &&
        setup.active_ps->ucode_data_hash() == water_ps_idx) {
      ++water_idx_lines;
      uint32_t lo = UINT32_MAX, hi = 0, resets = 0;
      for (uint32_t i = 0; i < guest_count; ++i) {
        uint32_t v = read_index(i);
        if (reset_enabled && v == reset_index) {
          ++resets;
          continue;
        }
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
      std::string fetch_sizes;
      const Shader::ConstantRegisterMap& map = setup.active_vs->constant_register_map();
      for (uint32_t w = 0; w < 3; ++w) {
        uint32_t bits = map.vertex_fetch_bitmap[w];
        uint32_t bit;
        while (rex::bit_scan_forward(bits, &bit)) {
          bits &= ~(1u << bit);
          uint32_t fi = w * 32 + bit;
          uint32_t d0 = r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fi * 2];
          uint32_t d1 = r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fi * 2 + 1];
          fetch_sizes += fmt::format(" vf{}@{:08X}:{}words", fi, d0 & 0x1FFFFFFC,
                                     (d1 >> 2) & 0xFFFFFF);
        }
      }
      REXLOG_INFO("[native] Trace water indices: count {} {}bit endian {} reset {} (index {:X}, "
                  "{} resets) | index range {}..{} | VGT min {} max {} offset {} |{}",
                  guest_count, index32 ? 32 : 16, uint32_t(index_endian), uint32_t(reset_enabled),
                  reset_index, resets, lo, hi, r[rex::graphics::XE_GPU_REG_VGT_MIN_VTX_INDX],
                  r[rex::graphics::XE_GPU_REG_VGT_MAX_VTX_INDX],
                  r.Get<int32_t>(rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET), fetch_sizes);
    }
  }
  bool host_indexed = indexed || convert_fan || convert_loop;
  VkIndexType index_type = index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
  VkDeviceSize index_offset = 0;
  if (host_indexed) {
    if (convert_fan) {
      if (guest_count < 3) return;
      draw_count = (guest_count - 2) * 3;
    } else if (convert_loop) {
      if (guest_count < 2) return;
      draw_count = guest_count + 1;
    }
    if (!indexed) {
      index_type = VK_INDEX_TYPE_UINT32;
    }
    uint32_t index_size = index_type == VK_INDEX_TYPE_UINT32 ? 4 : 2;
    // Index buffers in guest memory are usually static: reuse the converted
    // copy while the guest data is the same (checked once per frame).
    bool reused = false;
    VertexRangeKey index_key = {};
    if (dma_base && !convert_fan && !convert_loop) {
      uint32_t bytes = guest_count * (index32 ? 4 : 2);
      VertexHash& known = vertex_hashes[(uint64_t(dma_base) << 32) | bytes];
      RangeHash(known, guest_indices, bytes, dma_base, true);
      // The conversion also depends on the byte order and the reset index.
      index_key = {dma_base, bytes | 0x80000000u,
                   known.hash ^ (uint64_t(index_endian) << 60) ^
                       (reset_enabled ? (uint64_t(reset_index) << 8) | 1 : 0), known.epoch, known.frame_local};
      uint64_t found = FindRange(index_key);
      if (found != UINT64_MAX && stream_head - found < kStreamSize / 2) {
        index_offset = VkDeviceSize(found % kStreamSize);
        current().stream_oldest = std::min(current().stream_oldest, found);
        reused = true;
      }
    }
    if (!reused) {
      uint8_t* out = Allocate(VkDeviceSize(draw_count) * index_size, 4, index_offset);
      if (!out) {
        return skip(9);
      }
      if (index_key.address) {
        AddRange(index_key, stream_head - VkDeviceSize(draw_count) * index_size);
      }
      uint32_t host_reset = index_size == 4 ? 0xFFFFFFFFu : 0xFFFFu;
      auto put = [&](uint32_t i, uint32_t v) {
        if (reset_enabled && v == reset_index) {
          v = host_reset;
        }
        if (index_size == 4) {
          std::memcpy(out + size_t(i) * 4, &v, 4);
        } else {
          uint16_t h = uint16_t(v);
          std::memcpy(out + size_t(i) * 2, &h, 2);
        }
      };
      if (convert_fan) {
        uint32_t first = read_index(0);
        for (uint32_t t = 0; t + 2 < guest_count; ++t) {
          put(t * 3 + 0, first);
          put(t * 3 + 1, read_index(t + 1));
          put(t * 3 + 2, read_index(t + 2));
        }
      } else if (convert_loop) {
        for (uint32_t i = 0; i < guest_count; ++i) put(i, read_index(i));
        put(guest_count, read_index(0));
      } else if (!reset_enabled && index_endian == xenos::Endian::kNone) {
        std::memcpy(out, guest_indices, size_t(draw_count) * index_size);
      } else if (!reset_enabled && index_size == 2 && index_endian == xenos::Endian::k8in16) {
        rex::memory::copy_and_swap_16_unaligned(out, guest_indices, draw_count);
      } else if (!reset_enabled && index_size == 4 && index_endian == xenos::Endian::k8in32) {
        rex::memory::copy_and_swap_32_unaligned(out, guest_indices, draw_count);
      } else {
        for (uint32_t i = 0; i < draw_count; ++i) put(i, read_index(i));
      }
    }
  }

  // Fetch constants: the vertex buffers the shader reads are pointed at their
  // copies in the stream. Only slots written since they were last resolved,
  // or not yet resolved this frame, are looked at again.
  const Shader::ConstantRegisterMap& vs_constants = vs->constant_register_map();
  if (fetch_words_frame != frame || fetch_words_submission != submission_number) {
    // New frame: vertex data is checked for changes again. New submission:
    // copies in older submissions may be reused, but the bookkeeping is
    // simpler starting over.
    fetch_resolved_frame.fill(0);
    fetch_words_frame = frame;
    fetch_words_submission = submission_number;
  }
  uint32_t dirty_slots = owner->vertex_fetch_dirty_;
  owner->vertex_fetch_dirty_ = 0;
  if (dirty_slots) {
    for (uint32_t i = 0; i < 96; ++i) {
      if (dirty_slots & (1u << (i / 3))) fetch_resolved_frame[i] = 0;
    }
    // Texture words are copied as they are; vertex words get rewritten below.
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (dirty_slots & (1u << slot)) {
        std::memcpy(&fetch_words[slot * 6],
                    &r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6],
                    6 * sizeof(uint32_t));
      }
    }
  }
  for (uint32_t word = 0; word < 3; ++word) {
    uint32_t bits = vs_constants.vertex_fetch_bitmap[word];
    uint32_t bit;
    while (rex::bit_scan_forward(bits, &bit)) {
      bits &= ~(1u << bit);
      uint32_t fetch_index = word * 32 + bit;
      if (fetch_resolved_frame[fetch_index] == frame) {
        continue;
      }
      fetch_resolved_frame[fetch_index] = frame;
      uint32_t guest_dword0 =
          r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_index * 2];
      uint32_t dword1 =
          r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_index * 2 + 1];
      uint32_t& dword0 = fetch_words[fetch_index * 2];
      dword0 = guest_dword0;
      uint32_t address = guest_dword0 & 0x1FFFFFFC;
      uint32_t size = (dword1 >> 2) & 0xFFFFFF;  // in words
      uint32_t bytes = std::min(size * 4, 16u << 20);
      if (!bytes || uint64_t(address) + bytes > 0x20000000u) {
        continue;
      }
      const uint8_t* source = memory->TranslatePhysical<const uint8_t*>(address);
      VertexHash& known = vertex_hashes[(uint64_t(address) << 32) | bytes];
      RangeHash(known, source, bytes, address);
      VertexRangeKey range_key = {address, bytes, known.hash, known.epoch, known.frame_local};
      uint64_t found = FindRange(range_key);
      VkDeviceSize range_offset;
      if (found != UINT64_MAX && stream_head - found < kStreamSize / 2) {
        range_offset = VkDeviceSize(found % kStreamSize);
        current().stream_oldest = std::min(current().stream_oldest, found);
      } else {
        uint8_t* out = Allocate(bytes, 16, range_offset);
        if (!out) continue;
        std::memcpy(out, source, bytes);
        stats().vertex_bytes_copied += bytes;
        AddRange(range_key, stream_head - bytes);
      }
      dword0 = (guest_dword0 & 0x3) | uint32_t(range_offset);
    }
  }
  if (vertex_ranges.size() > 65536) {
    vertex_ranges.clear();
  }
  if (vertex_hashes.size() > 65536) {
    // WORKLOG #97: dropping everything every few seconds (run 347: ~1000 new keys per frame from dynamic
    // allocations) reset every range's unchanged/trusted history, so the settled and sampled shortcuts never
    // got going ("settled small ranges not hashed: 0"). Keep the ranges used in the last 16 frames.
    const uint64_t now_frame = frame;
    // run73: still 52k entries after pruning (then cleared): ranges in per-frame dynamic allocations are
    // seen once at a new address each frame. Keep last-2-frame entries, older ones only if checked again.
    vertex_hashes.retain([now_frame](const VertexHash& v) {
      if (v.frame == UINT64_MAX || v.frame + 16 < now_frame) return false;
      return v.frame + 2 >= now_frame || v.detector.unchanged() > 0 || v.detector.changes() > 0;
    });
    if (vertex_hashes.size() > 49152) vertex_hashes.clear();
    ++vertex_hash_prunes;
  }
  VkDeviceSize fetch_offset;
  if (last_fetch.submission == submission_number &&
      !std::memcmp(last_fetch_words, fetch_words.data(), sizeof(last_fetch_words))) {
    fetch_offset = last_fetch.offset;
  } else {
    uint8_t* fetch_out = Allocate(kFetchConstantsBytes, ubo_alignment, fetch_offset);
    if (!fetch_out) return skip(9);
    std::memcpy(fetch_out, fetch_words.data(), kFetchConstantsBytes);
    std::memcpy(last_fetch_words, fetch_words.data(), sizeof(last_fetch_words));
    last_fetch = {submission_number, 0, nullptr, frame, fetch_offset};
  }

  uint8_t* last_float_out = nullptr;
  const bool motion_blur_draw = motion_blur_off && ps && ps->ucode_data_hash() == kMotionBlurPs;
  const bool dof_draw = dof_off && ps && ps->ucode_data_hash() == kMotionBlurPs;
  auto write_float_constants = [&](const Shader::ConstantRegisterMap& map, uint32_t base_register,
                                   VkDeviceSize& offset) -> bool {
    // Only the constants the shader reads: the binding's fixed range reads on
    // into whatever follows (never used), which the stream's guard keeps
    // inside the buffer. Allocating the full 4 KB per draw filled the stream
    // and made it wait for the GPU.
    VkDeviceSize bytes = VkDeviceSize(std::max(map.float_count, 1u)) * 4 * sizeof(float);
    uint8_t* out = Allocate(bytes, ubo_alignment, offset);
    if (!out) return false;
    last_float_out = out;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = map.float_bitmap[i];
      uint32_t bit;
      while (rex::bit_scan_forward(bits, &bit)) {
        bits &= ~(uint64_t(1) << bit);
        std::memcpy(out, &r[base_register + (i << 8) + (bit << 2)], 4 * sizeof(float));
        out += 4 * sizeof(float);
      }
    }
    return true;
  };
  VkDeviceSize vs_float_offset, ps_float_offset, bool_loop_offset, system_offset;
  if (Reusable(last_vs_float, owner->vs_float_generation_, vs)) {
    vs_float_offset = last_vs_float.offset;
  } else {
    if (!write_float_constants(vs_constants, rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X,
                               vs_float_offset)) {
      return skip(9);
    }
    last_vs_float = {submission_number, owner->vs_float_generation_, vs, frame, vs_float_offset};
  }
  if (ps) {
    if (Reusable(last_ps_float, owner->ps_float_generation_, ps)) {
      ps_float_offset = last_ps_float.offset;
    } else {
      if (!write_float_constants(ps->constant_register_map(),
                                 rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X,
                                 ps_float_offset)) {
        return skip(9);
      }
      if (dof_draw) {
        // The composite's sharpness is max(focus factor, c99.w) (ucode 41-43): 1 = sharp.
        const auto& map = ps->constant_register_map();
        if (map.float_bitmap[1] & (uint64_t(1) << (99 - 64))) {
          uint32_t index = uint32_t(rex::bit_count(map.float_bitmap[0])) +
                           uint32_t(rex::bit_count(map.float_bitmap[1] &
                                                   ((uint64_t(1) << (99 - 64)) - 1)));
          const float one = 1.0f;
          std::memcpy(last_float_out + (index * 4 + 3) * sizeof(float), &one, sizeof(float));
        }
      }
      if (motion_blur_draw) {
        // No velocity (c2.x scales it) and, with the sample loop skipped below, the one tap
        // taken at the pixel itself weighted 1 (c253.w = 1 / taps).
        const auto& map = ps->constant_register_map();
        auto patch = [&](uint32_t constant, uint32_t component, float value) {
          const uint32_t word = constant >> 6, bit = constant & 63;
          if (!(map.float_bitmap[word] & (uint64_t(1) << bit))) return;
          uint32_t index = 0;
          for (uint32_t i = 0; i < word; ++i) index += uint32_t(rex::bit_count(map.float_bitmap[i]));
          index += uint32_t(rex::bit_count(map.float_bitmap[word] & ((uint64_t(1) << bit) - 1)));
          std::memcpy(last_float_out + (index * 4 + component) * sizeof(float), &value,
                      sizeof(float));
        };
        patch(2, 0, 0.0f);
        patch(253, 3, 1.0f);
      }
      last_ps_float = {submission_number, owner->ps_float_generation_, ps, frame,
                       ps_float_offset};
    }
  } else {
    ps_float_offset = vs_float_offset;
  }
  if (Reusable(last_bool_loop, owner->bool_loop_generation_, nullptr)) {
    bool_loop_offset = last_bool_loop.offset;
  } else {
    uint8_t* bool_loop = Allocate(kBoolLoopConstantsBytes, ubo_alignment, bool_loop_offset);
    if (!bool_loop) return skip(9);
    std::memcpy(bool_loop, &r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsBytes);
    last_bool_loop = {submission_number, owner->bool_loop_generation_, nullptr, frame,
                      bool_loop_offset};
  }
  if (motion_blur_draw) {
    // Its own copy with loop constant 31 (the blur's sample loop) at 0 iterations.
    uint8_t* bool_loop = Allocate(kBoolLoopConstantsBytes, ubo_alignment, bool_loop_offset);
    if (!bool_loop) return skip(9);
    std::memcpy(bool_loop, &r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsBytes);
    uint32_t loop31;
    std::memcpy(&loop31, bool_loop + (8 + 31) * sizeof(uint32_t), sizeof(loop31));
    loop31 &= ~uint32_t(0xFF);
    std::memcpy(bool_loop + (8 + 31) * sizeof(uint32_t), &loop31, sizeof(loop31));
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("[native] motion blur off: composite PS {:016X} drawn without its blur",
                  kMotionBlurPs);
    }
  }

  // Textures and samplers, reused while no fetch slot they come from was
  // written, within one frame (content changes are looked for once a frame)
  // and one submission (uploads go in its setup command buffer).
  upload.setup_commands = current().setup;
  upload.submission = submission_number;
  const VulkanShader* stage_shaders[2] = {vs, ps};
  // Keyed by the shaders and the fetch constants of the slots they read,
  // valid for a frame: materials repeat across the draws of a frame, and a hit
  // skips every texture and sampler lookup.
  uint32_t slot_mask = 0;
  for (const VulkanShader* shader : stage_shaders) {
    if (!shader) continue;
    for (const auto& b : shader->GetTextureBindingsAfterTranslation()) {
      slot_mask |= 1u << b.fetch_constant;
    }
    for (const auto& b : shader->GetSamplerBindingsAfterTranslation()) {
      slot_mask |= 1u << b.fetch_constant;
    }
  }
  uint64_t tb_key = XXH3_64bits_withSeed(&layout, sizeof(layout),
                                         uint64_t(reinterpret_cast<uintptr_t>(vs)) ^
                                             (uint64_t(reinterpret_cast<uintptr_t>(ps)) << 1));
  {
    uint32_t words[6 * 32];
    uint32_t count = 0;
    uint32_t remaining = slot_mask;
    uint32_t slot;
    while (rex::bit_scan_forward(remaining, &slot)) {
      remaining &= ~(1u << slot);
      std::memcpy(&words[count],
                  &r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6],
                  6 * sizeof(uint32_t));
      count += 6;
    }
    tb_key = XXH3_64bits_withSeed(words, count * sizeof(uint32_t), tb_key ^ slot_mask);
  }
  if (tb_cache_frame != frame) {
    if (tb_cache.size() > 8192) tb_cache.clear();
    tb_cache_frame = frame;
  }
  TextureBindings& tb = tb_cache[tb_key];
  // Any view destroyed anywhere moves views_epoch, which while a level
  // streams in is most frames: instead of rebuilding everything then, the
  // bindings check that their own views are still there.
  auto views_alive = [&]() {
    if (tb.views_epoch == textures.views_epoch()) return true;
    if (tb.view_count > 24) return false;
    for (uint32_t i = 0; i < tb.view_count; ++i) {
      if (!textures.ViewAlive(tb.views[i], tb.view_ids[i])) return false;
    }
    tb.views_epoch = textures.views_epoch();
    return true;
  };
  const BindingReuse reuse = CheckBindingReuse(
      tb.valid, tb.vs == vs && tb.ps == ps && tb.layout == layout,
      tb.set_generation == set_cache_generation,
      tb.answers_epoch == textures.answers_epoch(), views_alive,
      tb.frame == frame, tb.submission == submission_number, tb.valid_until, frame);
  static_assert(uint32_t(BindingReuse::Count) == 7);
  ++stats().texture_binding_cache[uint32_t(reuse)];
  bool tb_hit = reuse == BindingReuse::Hit;
  if (!tb_hit && binding_revalidate &&
      (reuse == BindingReuse::AnswersEpoch || reuse == BindingReuse::Expired) &&
      tb.view_count <= BindingRefresh::Capacity) {
    TextureTimer texture_timer(frame_texture_ns);
    ++stats().texture_binding_refresh_attempts;
    BindingRefresh refreshed;
    auto refresh_views = [&]() {
      for (uint32_t stage = 0; stage < 2; ++stage) {
        const VulkanShader* shader = stage_shaders[stage];
        if (!shader || (!st.counts[stage * 2] && !st.counts[stage * 2 + 1])) continue;
        for (const auto& binding : shader->GetTextureBindingsAfterTranslation()) {
          const uint32_t i = refreshed.view_count;
          if (i >= tb.view_count) return false;
          VkImageView view = SlotView(binding.fetch_constant, binding.dimension,
                                     bool(binding.is_signed));
          if (!refreshed.Observe(view == tb.views[i], tb.view_ids[i],
                                 textures.ViewId(view), slot_texture, slot_valid_until))
            return false;
        }
      }
      return refreshed.Complete(tb.view_count) && views_alive();
    };
    if (refresh_views()) {
      // Fetch key, shaders, layout and descriptor generation still match.
      // Content refresh (including resolve/composite copies) already ran via GetView.
      tb.texture_count = refreshed.texture_count;
      std::copy_n(refreshed.textures.begin(), refreshed.texture_count, tb.textures);
      tb.valid_until = refreshed.valid_until;
      tb.views_epoch = textures.views_epoch();
      tb.answers_epoch = textures.answers_epoch();
      tb.frame = frame;
      tb.submission = submission_number;
      owner->texture_fetch_dirty_ &= ~tb.slots;
      tb_hit = true;
      ++stats().texture_binding_refresh_kept;
    }
  }
  if (tb_hit && (tb.frame != frame || tb.submission != submission_number)) {
    for (uint32_t i = 0; i < tb.texture_count; ++i) {
      textures.Touch(tb.textures[i], submission_number);
    }
    tb.frame = frame;
    tb.submission = submission_number;
  }
  if (!tb_hit) {
    TextureTimer texture_timer(frame_texture_ns);
    ++stats().texture_bindings_built;
    tb = TextureBindings();
    tb.valid_until = UINT64_MAX;
    for (uint32_t stage = 0; stage < 2; ++stage) {
      const VulkanShader* shader = stage_shaders[stage];
      if (!shader || (!st.counts[stage * 2] && !st.counts[stage * 2 + 1])) continue;
      const auto& texture_bindings = shader->GetTextureBindingsAfterTranslation();
      const auto& sampler_bindings = shader->GetSamplerBindingsAfterTranslation();
      VkDescriptorImageInfo infos[64];
      uint32_t info_count = 0;
      for (const auto& binding : texture_bindings) {
        if (info_count >= 64) break;
        tb.used_mask |= 1u << binding.fetch_constant;
        VkDescriptorImageInfo& info = infos[info_count++];
        info.sampler = VK_NULL_HANDLE;
        info.imageView = SlotView(binding.fetch_constant, binding.dimension,
                                  bool(binding.is_signed));
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        tb.valid_until = std::min(tb.valid_until, slot_valid_until);
        if (tb.view_count < 24) {
          tb.views[tb.view_count] = info.imageView;
          tb.view_ids[tb.view_count] = textures.ViewId(info.imageView);
        }
        ++tb.view_count;
        if (slot_texture) {
          if (tb.texture_count < 24) {
            tb.textures[tb.texture_count++] = slot_texture;
          } else {
            tb.valid_until = frame;
          }
        }
      }
      for (const auto& binding : sampler_bindings) {
        if (info_count >= 64) break;
        tb.slots |= 1u << binding.fetch_constant;
        VkDescriptorImageInfo& info = infos[info_count++];
        info.sampler = SlotSampler(binding);
        info.imageView = VK_NULL_HANDLE;
        info.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      }
      VkDescriptorSetLayout set_layout =
          stage ? layout->textures_pixel : layout->textures_vertex;
      // Keyed by view ids, never reused, rather than handles, which are: the
      // cache was dropped whenever any view was destroyed, which while a level
      // streams in is most frames, and every material then allocated and
      // wrote new sets (10-50 ms in the frames that dropped).
      uint64_t key_words[64];
      for (uint32_t i = 0; i < info_count; ++i) {
        key_words[i] = infos[i].imageView ? textures.ViewId(infos[i].imageView)
                                          : (uint64_t(1) << 63) ^
                                                uint64_t(reinterpret_cast<uintptr_t>(
                                                    infos[i].sampler));
      }
      uint64_t set_key =
          XXH3_64bits_withSeed(key_words, sizeof(uint64_t) * info_count, uint64_t(set_layout));
      auto cached = texture_set_cache.find(set_key);
      if (cached != texture_set_cache.end()) {
        tb.sets[stage] = cached->second;
        continue;
      }
      if (texture_set_cache.size() >= 16384) {
        FlushSetCache();
      }
      VkDescriptorSet set = AllocatePersistentSet(set_layout);
      if (!set) return skip(11);
      texture_set_cache.emplace(set_key, set);
      VkWriteDescriptorSet writes[2] = {};
      uint32_t write_count = 0;
      if (!texture_bindings.empty()) {
        VkWriteDescriptorSet& w = writes[write_count++];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = 0;
        w.descriptorCount = uint32_t(texture_bindings.size());
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = infos;
      }
      if (!sampler_bindings.empty()) {
        VkWriteDescriptorSet& w = writes[write_count++];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = uint32_t(texture_bindings.size());
        w.descriptorCount = uint32_t(sampler_bindings.size());
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = infos + texture_bindings.size();
      }
      dfn().vkUpdateDescriptorSets(vk, write_count, writes, 0, nullptr);
      tb.sets[stage] = set;
    }
    tb.slots |= tb.used_mask;
    tb.valid = true;
    tb.frame = frame;
    tb.submission = submission_number;
    tb.vs = vs;
    tb.ps = ps;
    tb.layout = layout;
    tb.views_epoch = textures.views_epoch();
    tb.set_generation = set_cache_generation;
    tb.answers_epoch = textures.answers_epoch();
    // Only the slots these bindings read are cleared: another draw's
    // bindings may still need to see the others as written.
    owner->texture_fetch_dirty_ &= ~tb.slots;
  }

  // System constants come from the render state, the texture fetch
  // constants and the vertex index registers.
  bool system_same = last_system.submission == submission_number &&
                     system_inputs.setup_generation == setup_generation &&
                     system_inputs.fetch_generation == owner->fetch_generation_ &&
                     system_inputs.index_generation == owner->index_generation_ &&
                     system_inputs.used_mask == tb.used_mask;
  if (!system_same) {
    UpdateSystemConstants(st.primitive_polygonal, st.viewport, tb.used_mask, 0, vs, ps);
    system_inputs = {setup_generation, owner->fetch_generation_, owner->index_generation_,
                     tb.used_mask};
    system_same = last_system.submission == submission_number &&
                  !std::memcmp(&last_system_constants, &system_constants,
                               sizeof(system_constants));
  }
  if (system_same) {
    system_offset = last_system.offset;
  } else {
    uint8_t* system_out =
        Allocate(sizeof(SpirvShaderTranslator::SystemConstants), ubo_alignment, system_offset);
    if (!system_out) return skip(9);
    std::memcpy(system_out, &system_constants, sizeof(system_constants));
    std::memcpy(&last_system_constants, &system_constants, sizeof(system_constants));
    last_system = {submission_number, 0, nullptr, frame, system_offset};
  }

  // Record.
  if (!BeginPass(targets)) return skip(14);
  DrawCommand d;
  d.commands = current().main;
  d.flags = 0;
  d.layout = layout->layout;
  if (bound_pipeline != st.pipeline) {
    d.flags |= kDrawPipeline;
    d.pipeline = st.pipeline;
    bound_pipeline = st.pipeline;
  }
  if (bound_layout != layout->layout || !bound_static_sets) {
    d.flags |= kDrawSharedSet;
    d.set_shared = set_shared;
    bound_layout = layout->layout;
    bound_static_sets = true;
    bound_offsets_valid = false;
    bound_texture_sets[0] = bound_texture_sets[1] = VK_NULL_HANDLE;
  }
  uint32_t* dynamic_offsets = d.offsets;
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferSystem] = uint32_t(system_offset);
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferFloatVertex] = uint32_t(vs_float_offset);
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferFloatPixel] = uint32_t(ps_float_offset);
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferBoolLoop] = uint32_t(bool_loop_offset);
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferFetch] = uint32_t(fetch_offset);
  if (!bound_offsets_valid || std::memcmp(bound_offsets, d.offsets, sizeof(d.offsets))) {
    d.flags |= kDrawConstants;
    d.set_constants = set_constants;
    std::memcpy(bound_offsets, d.offsets, sizeof(d.offsets));
    bound_offsets_valid = true;
  }
  if (tb.sets[0] && tb.sets[0] != bound_texture_sets[0]) {
    d.flags |= kDrawTexturesVertex;
    d.texture_sets[0] = tb.sets[0];
    bound_texture_sets[0] = tb.sets[0];
  }
  if (tb.sets[1] && tb.sets[1] != bound_texture_sets[1]) {
    d.flags |= kDrawTexturesPixel;
    d.texture_sets[1] = tb.sets[1];
    bound_texture_sets[1] = tb.sets[1];
  }

  // Dynamic state.
  if (std::memcmp(&st.vp, &bound_viewport, sizeof(st.vp))) {
    d.flags |= kDrawViewport;
    d.viewport = st.vp;
    bound_viewport = st.vp;
  }
  VkRect2D sr;
  sr.offset.x = int32_t(std::min(st.scissor.offset[0], pass_extent.width));
  sr.offset.y = int32_t(std::min(st.scissor.offset[1], pass_extent.height));
  sr.extent.width = std::min(st.scissor.extent[0], pass_extent.width - uint32_t(sr.offset.x));
  sr.extent.height = std::min(st.scissor.extent[1], pass_extent.height - uint32_t(sr.offset.y));
  if (std::memcmp(&sr, &bound_scissor, sizeof(sr))) {
    d.flags |= kDrawScissor;
    d.scissor = sr;
    bound_scissor = sr;
  }
  if (std::memcmp(&st.bias_constant, &bound_depth_bias[0], sizeof(float)) ||
      std::memcmp(&st.bias_slope, &bound_depth_bias[1], sizeof(float))) {
    d.flags |= kDrawDepthBias;
    d.bias_constant = st.bias_constant;
    d.bias_slope = st.bias_slope;
    bound_depth_bias[0] = st.bias_constant;
    bound_depth_bias[1] = st.bias_slope;
  }
  if (std::memcmp(st.blend_constants, bound_blend_constants, sizeof(st.blend_constants))) {
    d.flags |= kDrawBlend;
    std::memcpy(d.blend, st.blend_constants, sizeof(d.blend));
    std::memcpy(bound_blend_constants, st.blend_constants, sizeof(st.blend_constants));
  }
  std::memcpy(d.stencil, st.stencil, sizeof(d.stencil));
  if (st.stencil[0] != bound_stencil[0] || st.stencil[1] != bound_stencil[1]) {
    d.flags |= kDrawStencilCompare;
  }
  if (st.stencil[2] != bound_stencil[2] || st.stencil[3] != bound_stencil[3]) {
    d.flags |= kDrawStencilWrite;
  }
  if (st.stencil[4] != bound_stencil[4] || st.stencil[5] != bound_stencil[5]) {
    d.flags |= kDrawStencilReference;
  }
  std::memcpy(bound_stencil, st.stencil, sizeof(st.stencil));

  d.count = draw_count;
  if (host_indexed) {
    d.flags |= kDrawIndexed;
    if (index_offset != bound_index_offset || index_type != bound_index_type ||
        !bound_index_valid) {
      d.flags |= kDrawIndexBuffer;
      d.index_buffer = stream_buffer;
      d.index_offset = index_offset;
      d.index_type = index_type;
      bound_index_offset = index_offset;
      bound_index_type = index_type;
      bound_index_valid = true;
    }
  }
  if (water_dump_enabled && setup.active_vs &&
      setup.active_vs->ucode_data_hash() == UINT64_C(0xFB0614F28843002E)) {
    RequestWaterDump();
  }

  if (edram_depth_alias_clear && setup.targets.depth && !setup.targets.color[0] &&
      setup.targets.color[1] == nullptr) {
    auto draw_initiator = r.Get<reg::VGT_DRAW_INITIATOR>();
    if (draw_initiator.prim_type == xenos::PrimitiveType::kRectangleList) {
      AliasDepthClearToColor(r.Get<reg::RB_SURFACE_INFO>().msaa_samples);
    }
  }
  if (WaterSimTracing()) {
    if (((post_trace_seconds && post_trace_count == 2) || capture_all) && setup.active_ps) {
      PostDump(setup.active_ps->ucode_data_hash());
    }
    TraceSimDraw(d.count);
  }
  if (water_seq_frame && frame == water_seq_frame && water_seq_count < 24) {
    WaterSequenceDump(d.count);
  }
  if (water_raw_pending && setup.active_ps &&
      setup.active_ps->ucode_data_hash() == UINT64_C(0xA8A487FAF6E47E59)) {
    DumpWaterSimInputs();
  }
  if (profile_draws && profile_frame) {
    // Its own segment: the time to the next mark is charged to this shader pair.
    const uint64_t vs_hash = setup.active_vs ? setup.active_vs->ucode_data_hash() : 0;
    const uint64_t ps_hash = setup.active_ps ? setup.active_ps->ucode_data_hash() : 0;
    if (profile_draw_kinds) {
      // B:122: by what the draw changed (bit 59 marks the kind key) and whether it is indexed.
      ProfileMark((kProfileDraw << 60) | (uint64_t(1) << 59) | (d.flags & 0x201F));
    } else {
      ProfileMark((kProfileDraw << 60) | ((vs_hash & 0x3FFFFFFF) << 30) | (ps_hash & 0x3FFFFFFF));
    }
  }
  AppendOcclusion(d);
  SubmitDraw(d);
  ProfileDraw(d.count);
  ++frame_draw_count;
  ++stats().draws;
  for (RenderTarget* rt : targets.color) {
    if (rt) rt->last_used_frame = frame;
  }
}

// --- Resolves ------------------------------------------------------------------------

void Renderer::Impl::Resolve() {
  p2::gpuwait::Scope gpu_wait_scope(p2::gpuwait::Resolve);
  const RegisterFile& r = *regs;
  auto copy_control = r.Get<reg::RB_COPY_CONTROL>();
  auto surface_info = r.Get<reg::RB_SURFACE_INFO>();

  // The rectangle (draw_util::GetResolveInfo): Direct3D 9 puts its three
  // vertices in vertex fetch constant 0.
  xenos::xe_gpu_vertex_fetch_t fetch = r.GetVertexFetch(0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 3 * 2) {
    return;
  }
  const float* vertices = memory->TranslatePhysical<const float*>(fetch.address << 2);
  float half_pixel =
      r.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  int32_t v[6];
  for (uint32_t i = 0; i < 6; ++i) {
    float f = xenos::GpuSwap(vertices[i], fetch.endian) + half_pixel;
    v[i] = int32_t(f * 256.0f);
  }
  int32_t x0 = (std::min({v[0], v[2], v[4]}) + 127) >> 8;
  int32_t y0 = (std::min({v[1], v[3], v[5]}) + 127) >> 8;
  int32_t x1 = (std::max({v[0], v[2], v[4]}) + 127) >> 8;
  int32_t y1 = (std::max({v[1], v[3], v[5]}) + 127) >> 8;
  if (r.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    auto offset = r.Get<reg::PA_SC_WINDOW_OFFSET>();
    x0 += offset.window_x_offset;
    x1 += offset.window_x_offset;
    y0 += offset.window_y_offset;
    y1 += offset.window_y_offset;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(r, scissor, false);
  x0 = std::clamp(x0, int32_t(scissor.offset[0]), int32_t(scissor.offset[0] + scissor.extent[0]));
  x1 = std::clamp(x1, int32_t(scissor.offset[0]), int32_t(scissor.offset[0] + scissor.extent[0]));
  y0 = std::clamp(y0, int32_t(scissor.offset[1]), int32_t(scissor.offset[1] + scissor.extent[1]));
  y1 = std::clamp(y1, int32_t(scissor.offset[1]), int32_t(scissor.offset[1] + scissor.extent[1]));
  x0 &= ~7;
  y0 &= ~7;
  x1 = rex::align(x1, 8);
  y1 = rex::align(y1, 8);
  x1 = std::min(x1, int32_t(surface_info.surface_pitch));
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  uint32_t msaa = HostMsaa(surface_info.msaa_samples);
  const uint32_t sample_x = SampleScaleX(surface_info.msaa_samples);
  const uint32_t sample_y = SampleScaleY(surface_info.msaa_samples);
  bool is_depth = copy_control.copy_src_select >= 4;
  ++stats().resolves;

  BeginRecording();
  EndPass(PassEnd::Resolve);
  ProfileMark((kProfileResolve << 60) | (uint64_t(is_depth) << 56) |
              (uint64_t(surface_info.surface_pitch & 0x7FF) << 32) |
              (uint64_t(uint32_t(x1 - x0) & 0xFFFF) << 16) | uint32_t(y1 - y0));
  VkCommandBuffer c = cmd();

  RenderTargetKey source_key = {};
  source_key.pitch = surface_info.surface_pitch * sample_x;
  source_key.msaa = msaa;
  if (is_depth) {
    auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
    source_key.base_tiles = depth_info.depth_base;
    source_key.format = uint32_t(depth_info.depth_format);
    source_key.is_depth = 1;
  } else {
    auto color_info =
        r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[copy_control.copy_src_select]);
    source_key.base_tiles = color_info.color_base;
    source_key.format = RtKeyColorFormat(color_info.color_format);
  }
  if (reflection_half_rate && !is_depth && reflection_texture &&
      (r[rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF) == reflection_texture) {
    if (ReflectionSkipFrame()) {
      ++work_reflection_skipped;
      return;  // the water keeps last frame's reflection
    }
    reflection_color = source_key;
    reflection_color_known = true;
    reflection_resolved_frame = frame;
  }
  RenderTarget* source = GetRenderTarget(source_key, uint32_t(y1) * sample_y);
  if (WaterSimTracing()) {
    ++water_sim_trace_lines;
    auto ci = r.Get<reg::RB_COPY_DEST_INFO>();
    REXLOG_INFO("[native] SIMTRACE resolve {} src fmt{}@{} key pitch {} msaa {} (sample {}x{}) "
                "guest pitch {} rect {},{}-{},{} -> {:08X} tex fmt {} exp {} pitch {} endian {} swap {} | copy cmd {} "
                "sample sel {} clear c{} d{} | src image {}x{}",
                is_depth ? "depth" : "color", source_key.format, source_key.base_tiles,
                source_key.pitch, uint32_t(surface_info.msaa_samples), sample_x, sample_y,
                uint32_t(surface_info.surface_pitch), x0, y0, x1, y1,
                r[rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF,
                uint32_t(ci.copy_dest_format), int32_t(ci.copy_dest_exp_bias),
                r.Get<reg::RB_COPY_DEST_PITCH>().copy_dest_pitch, uint32_t(ci.copy_dest_endian),
                uint32_t(ci.copy_dest_swap),
                uint32_t(copy_control.copy_command), uint32_t(copy_control.copy_sample_select),
                uint32_t(copy_control.color_clear_enable), uint32_t(copy_control.depth_clear_enable),
                source ? source->width : 0, source ? source->height : 0);
  }
  if (!source) return;
  // Prototype 2 (L3+R3 capture): the colour target behind every resolve of the captured frame,
  // so the frame's passes can be looked at in order (logs/capN_rNN_*.bin, raw host texels).
  if (capture_all && WaterSimTracing() && !is_depth && capture_resolve_dumps < 80) {
    ++capture_resolve_dumps;
    RecordImageDump(source->image, source->format, std::min(source->width, 1280u),
                    std::min(source->height, 736u),
                    fmt::format("cap{}_r{:02}_fmt{}_b{}_rect{}x{}_to{:08X}_tex{}_exp{}", capture_count,
                                capture_resolve_dumps, source_key.format, source_key.base_tiles,
                                x1 - x0, y1 - y0,
                                r[rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF,
                                uint32_t(r.Get<reg::RB_COPY_DEST_INFO>().copy_dest_format),
                                int32_t(r.Get<reg::RB_COPY_DEST_INFO>().copy_dest_exp_bias)));
  }
  c = cmd();
  uint32_t width = uint32_t(x1 - x0);
  const uint32_t source_rows = source->height / sample_y;
  uint32_t height = std::min(uint32_t(y1 - y0), source_rows - std::min(uint32_t(y0), source_rows));
  if (!height) return;
  if (work_stats_enabled) {
    ++work_resolves;
    work_resolve_pixels += uint64_t(width) * height;
  }

  // Copy.
  uint32_t dest_base = r[rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE];
  auto dest_info = r.Get<reg::RB_COPY_DEST_INFO>();
  bool copy = dest_base != 0 &&
              (copy_control.copy_command == xenos::CopyCommand::kRaw ||
               copy_control.copy_command == xenos::CopyCommand::kConvert);
  if (copy) {
    VkMemoryBarrier to_transfer = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                       VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    dfn().vkCmdPipelineBarrier(c,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_transfer, 0, nullptr,
                               0, nullptr);
    // Sized by what the resolve covers, not by the destination pitch (aligned
    // to 32): textures sample the image across its whole size, and the game
    // samples its full-screen resolves at 1040 wide, where the pitch made the
    // image 1056 and every one of them needed a corrected copy each frame.
    // Smaller textures sampled from a corner still get one (CopyToSized).
    uint32_t dest_width = uint32_t(x1);
    uint32_t dest_height = uint32_t(y0) + height;
    dest_width = std::min(dest_width, 8192u);
    dest_height = std::min(dest_height, 8192u);
    VkFormat dest_format = is_depth ? VK_FORMAT_R32_SFLOAT : source->format;
    // Mercenaries 2: the resolve's exponent bias and the destination format's range, which
    // the project8 renderer ignored (HDR scene resolved unscaled: overexposure, run19).
    float resolve_scale = std::ldexp(1.0f, int(dest_info.copy_dest_exp_bias));
    uint32_t resolve_clamp = 0;
    switch (dest_info.copy_dest_format) {
      case xenos::ColorFormat::k_8:
      case xenos::ColorFormat::k_1_5_5_5:
      case xenos::ColorFormat::k_5_6_5:
      case xenos::ColorFormat::k_6_5_5:
      case xenos::ColorFormat::k_8_8_8_8:
      case xenos::ColorFormat::k_2_10_10_10:
      case xenos::ColorFormat::k_8_A:
      case xenos::ColorFormat::k_8_B:
      case xenos::ColorFormat::k_8_8:
      case xenos::ColorFormat::k_8_8_8_8_A:
      case xenos::ColorFormat::k_4_4_4_4:
      case xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16:
      case xenos::ColorFormat::k_2_10_10_10_AS_16_16_16_16:
        resolve_clamp = 1;
        break;
      default:
        break;
    }
    // Mercenaries 2 (HANDOFF section 53): k_16_16 / k_16_16_16_16 render targets hold
    // [-32, 32] (stored here as SNORM [-1, 1], the translator scales by 1/32). Resolved to
    // k_16_16_FLOAT / k_16_16_16_16_FLOAT textures they must come out as the real values:
    // a float destination image and 32 times the stored value.
    // Prototype 2: Xenia's resolve packing (resolve.xesli / pixel_formats.xesli XePackFixed):
    // value * 2^exp_bias, then packed into the destination by its number format - unsigned
    // fraction clamps to [0, 1], signed fraction to [-1, 1] - whatever the render target was.
    // A UNORM / SNORM destination image does that clamp itself and samples exactly as the
    // texture unit reads the 16-bit data. The Mercs2 rule below (1/32, float destination) was
    // tuned on that game's water and does not follow the hardware.
    const bool xenia_pack = !is_depth && REXCVAR_GET(p2_native_resolve_xenia_pack) &&
                            (dest_info.copy_dest_format == xenos::ColorFormat::k_16_16 ||
                             dest_info.copy_dest_format == xenos::ColorFormat::k_16_16_16_16);
    if (xenia_pack) {
      const bool two = dest_info.copy_dest_format == xenos::ColorFormat::k_16_16;
      switch (dest_info.copy_dest_number) {
        case xenos::SurfaceNumberFormat::kSignedRepeatingFraction:
          dest_format = two ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16B16A16_SNORM;
          break;
        case xenos::SurfaceNumberFormat::kUnsignedRepeatingFraction:
          dest_format = two ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R16G16B16A16_UNORM;
          break;
        // Integer numbers (Prototype 2 resolves its HDR scene as k_16_16_16_16 unsigned integer
        // with exp bias 10: the stored word is round(value * 1024), which the texture unit then
        // reads as a 16-bit fraction, word / 65535). A UNORM image holding word / 65535 is
        // exactly that, clamp to [0, 65535] included.
        // run10: as UNORM (word / 65535) the whole scene went black, run09 (float image with
        // the integer value itself) drew it: the game's fetch reads the integer, scaled by its
        // exp_adjust. Kept as an option.
        case xenos::SurfaceNumberFormat::kUnsignedInteger:
          if (REXCVAR_GET(p2_native_resolve_int_as_unorm)) {
            dest_format = two ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R16G16B16A16_UNORM;
            resolve_scale *= 1.0f / 65535.0f;
          }
          break;
        case xenos::SurfaceNumberFormat::kSignedInteger:
          if (REXCVAR_GET(p2_native_resolve_int_as_unorm)) {
            dest_format = two ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16B16A16_SNORM;
            resolve_scale *= 1.0f / 32767.0f;
          }
          break;
        default:
          break;  // float numbers: keep the float image
      }
      resolve_clamp = 0;
    } else if (!is_depth) {
      const auto rt_format = xenos::ColorRenderTargetFormat(source->key.format);
      const bool signed_fixed = rt_format == xenos::ColorRenderTargetFormat::k_16_16 ||
                                rt_format == xenos::ColorRenderTargetFormat::k_16_16_16_16;
      if (signed_fixed && (dest_info.copy_dest_format == xenos::ColorFormat::k_16_16_FLOAT ||
                           dest_info.copy_dest_format ==
                               xenos::ColorFormat::k_16_16_16_16_FLOAT)) {
        resolve_scale *= float(REXCVAR_GET(p2_native_signed_resolve_scale));
      } else if (signed_fixed &&
                 (dest_info.copy_dest_format == xenos::ColorFormat::k_16_16 ||
                  dest_info.copy_dest_format == xenos::ColorFormat::k_16_16_16_16)) {
        dest_format = dest_info.copy_dest_format == xenos::ColorFormat::k_16_16
                          ? VK_FORMAT_R16G16_SNORM
                          : VK_FORMAT_R16G16B16A16_SNORM;
        resolve_scale *= 1.0f / 32.0f;
      }
    }
    if (water_texture_addresses.count(dest_base & 0x1FFFFFFF)) {
      static uint32_t water_resolve_lines = 0;
      if (water_resolve_lines < 60) {
        ++water_resolve_lines;
        REXLOG_INFO("[native] Trace water resolve: {} RT fmt {} base {} pitch {} msaa {} rect "
                    "{},{} {}x{} (height {}) -> {:08X} tex fmt {} exp bias {} dest pitch {} "
                    "image {}x{} scale {} clamp {}",
                    is_depth ? "depth" : "color", source->key.format, source->key.base_tiles,
                    uint32_t(surface_info.surface_pitch), uint32_t(surface_info.msaa_samples), x0,
                    y0, x1 - x0, y1 - y0, height, dest_base & 0x1FFFFFFF,
                    uint32_t(dest_info.copy_dest_format), int32_t(dest_info.copy_dest_exp_bias),
                    r.Get<reg::RB_COPY_DEST_PITCH>().copy_dest_pitch, dest_width, dest_height,
                    resolve_scale, resolve_clamp);
      }
    }
    if (!is_depth && (source->key.format == uint32_t(xenos::ColorRenderTargetFormat::k_16_16) ||
                      source->key.format ==
                          uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16))) {
      static uint32_t signed_resolve_lines = 0;
      if (signed_resolve_lines < 40) {
        ++signed_resolve_lines;
        REXLOG_INFO("[native] Trace signed resolve: RT fmt {} base {} pitch {} rect {},{} {}x{} -> "
                    "{:08X} tex fmt {} exp bias {} scale {} dest vk{} msaa {}",
                    source->key.format, source->key.base_tiles,
                    uint32_t(surface_info.surface_pitch), x0, y0, x1 - x0, y1 - y0,
                    dest_base & 0x1FFFFFFF, uint32_t(dest_info.copy_dest_format),
                    int32_t(dest_info.copy_dest_exp_bias), resolve_scale, uint32_t(dest_format),
                    uint32_t(surface_info.msaa_samples));
      }
    }
    if (!is_depth) {
      const uint32_t combination = uint32_t(source->key.format) |
                                   (uint32_t(dest_info.copy_dest_format) << 8) |
                                   (uint32_t(dest_info.copy_dest_exp_bias & 0x3F) << 16) |
                                   (sample_x * sample_y << 24);
      if (resolve_logged.insert(combination).second) {
        REXLOG_INFO("[native] Resolve: render target format {} -> texture format {}, exp bias "
                    "{}, {} samples per pixel, clamp {}, number format {}, dest vk{}",
                    source->key.format, uint32_t(dest_info.copy_dest_format),
                    int32_t(dest_info.copy_dest_exp_bias), sample_x * sample_y, resolve_clamp,
                    uint32_t(dest_info.copy_dest_number), uint32_t(dest_format));
      }
    }
    uint32_t host_swizzle = is_depth ? uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR)
                                     : uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
    if (!is_depth && dest_info.copy_dest_swap) {
      host_swizzle = XE_GPU_MAKE_TEXTURE_SWIZZLE(B, G, R, A);
    }
    ResolveDestination* dest = textures.GetResolveDestination(
        dest_base & 0x1FFFFFFF, dest_width, dest_height, dest_format, host_swizzle, c);
    if (dest) dest->guest_pitch = r.Get<reg::RB_COPY_DEST_PITCH>().copy_dest_pitch;
    uint32_t probe_path = 0;  // 0=no destination, 1=color compute, 2=depth compute,
    // 3=depth scratch, 4=blit, 5=resolve image, 6=copy image,
    // 7=unsupported depth, 8=depth descriptor failure, 9=depth words direct.
    bool atlas_written = false;
    if (dest) {
      if (is_depth) {
        // Depth cannot be copied into a color image directly. It goes through
        // a scratch buffer, where a compute pass makes both forms a game can
        // read a depth resolve back as: floats, into this R32_SFLOAT image for
        // k_24_8 fetches, and the words the resolve leaves in Xbox 360
        // memory, (depth24 << 8) | stencil, into a second image for fetches
        // as any other format (see TextureCache::kDepthWordsTag). Games read
        // the stencil byte that way to tell the player from the scene: with
        // depth alone in every channel, a post effect meant to skip the skater
        // drew him doubled with a light halo.
        const uint64_t texels = uint64_t(width) * height;
        const VkDeviceSize stencil_offset = rex::align(VkDeviceSize(texels * 4), VkDeviceSize(256));
        const VkDeviceSize packed_offset =
            rex::align(stencil_offset + VkDeviceSize(texels), VkDeviceSize(256));
        const bool has_stencil = HasStencil(source->format);
        const bool unorm_guest = xenos::DepthRenderTargetFormat(source->key.format) ==
                                 xenos::DepthRenderTargetFormat::kD24S8;
        if (msaa == 0 && depth_copy_pipeline && source->sample_view &&
            !textures.WantsDepthWords(dest_base & 0x1FFFFFFF)) {
          // The usual case: the game samples the resolve only as depth.
          uint32_t atlas_row = 0;
          ResolveDestination::Sized* atlas =
              shadow_atlas_direct
                  ? textures.FindAtlas(dest_base & 0x1FFFFFFF, dest_width, dest_height,
                                       dest_format, &atlas_row)
                  : nullptr;
          bool cascade_skipped = false;
          if (atlas && dest_height) {
            const uint32_t cascade = atlas_row / dest_height;
            if (shadow_stagger && cascade != shadow_cascade && !shadow_stagger_broken) {
              shadow_stagger_broken = true;
              REXLOG_WARN("[native] shadow stagger off: cascade {} resolved as number {} of the "
                          "frame",
                          cascade, shadow_cascade);
            }
            if (!shadow_rt_known && shadow_depth_clamp) {
              setup_cache.clear();
              setup.valid = false;
            }
            shadow_rt_known = true;
            shadow_rt_base = source->key.base_tiles;
            shadow_rt_pitch = source->key.pitch;
            shadow_rt_format = source->key.format;
            shadow_cascade = cascade + 1;
            if (ShadowCascadeSkipped(cascade)) {
              // Its rows keep the map of the previous frame; its draws were not issued.
              cascade_skipped = true;
              atlas_written = true;
              ++work_shadow_skipped_resolves;
            }
          }
          auto atlas_written_now = [&]() {
            // The destination's own image is left as it was, and CopyToSized below skipped:
            // the game samples this memory only through the atlas.
            atlas->filled = true;
            ++work_atlas_direct;
            if (post_trace_seconds && post_trace_count < 6) ArmPostTrace();
            static bool logged = false;
            if (!logged) {
              logged = true;
              REXLOG_INFO("[native] Depth resolve to {:08X} ({}x{}) written into the atlas at "
                          "row {}", dest_base & 0x1FFFFFFF, dest_width, dest_height, atlas_row);
            }
            atlas_written = true;
          };
          if (cascade_skipped) {
            // Nothing to convert.
          } else if (atlas && atlas_fragment_pipeline && atlas->color_target && sample_x == 1 &&
                     sample_y == 1 &&
                     AtlasFragmentPass(c, *source, *atlas, x0, y0, width, height, atlas_row)) {
            probe_path = 10;
            atlas_written_now();
          } else {
          VkMemoryBarrier to_compute = {
              VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                  VK_ACCESS_SHADER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
          dfn().vkCmdPipelineBarrier(
              c,
              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                  VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &to_compute, 0, nullptr, 0, nullptr);
          VkDescriptorSet set = AllocateSet(depth_copy_set_layout);
          if (!set && resolve_probe_enabled) probe_path = 8;
          if (set) {
            probe_path = 2;
            VkDescriptorImageInfo images[2] = {
                {VK_NULL_HANDLE, source->sample_view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, atlas ? atlas->storage_view : dest->view,
                 VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet writes[2] = {};
            for (uint32_t i = 0; i < 2; ++i) {
              writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
              writes[i].dstSet = set;
              writes[i].dstBinding = i;
              writes[i].descriptorCount = 1;
              writes[i].descriptorType =
                  i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
              writes[i].pImageInfo = &images[i];
            }
            dfn().vkUpdateDescriptorSets(vk, 2, writes, 0, nullptr);
            int32_t params[9] = {x0, y0, int32_t(width), int32_t(height),
                                 int32_t(sample_x), int32_t(sample_y), 0, int32_t(atlas_row),
                                 (atlas && shadow_empty_lit) ? 1 : 0};
            dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_copy_pipeline);
            dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_copy_layout, 0,
                                          1, &set, 0, nullptr);
            dfn().vkCmdPushConstants(c, depth_copy_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                     sizeof(params), params);
            dfn().vkCmdDispatch(c, (width + 7) / 8, (height + 7) / 8, 1);
            if (atlas) atlas_written_now();
          }
          // Readers: the draws, CopyToSized below, and the next depth test.
          VkMemoryBarrier after_compute = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                           VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
          dfn().vkCmdPipelineBarrier(
              c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                  VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
              0, 1, &after_compute, 0, nullptr, 0, nullptr);
          }
        } else if (msaa == 0 && sample_x == 1 && sample_y == 1 && depth_words_pipeline &&
                   unorm_guest && has_stencil && source->sample_view && source->stencil_view &&
                   textures.StorageCapable(VK_FORMAT_R8G8B8A8_UNORM) &&
                   DepthWordsDirect(c, *source, *dest, dest_base, dest_width, dest_height, x0, y0,
                                    width, height)) {
          // HANDOFF B:91: depth and words in one pass, same bits as the buffer route below.
          probe_path = 9;
        } else if (msaa == 0 && depth_scratch && depth_convert_pipeline &&
                   packed_offset + texels * 4 <= kDepthScratchSize) {
          probe_path = 3;
          VkBufferImageCopy regions[2] = {};
          regions[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
          regions[0].imageOffset = {x0, y0, 0};
          regions[0].imageExtent = {width, height, 1};
          regions[1] = regions[0];
          regions[1].bufferOffset = stencil_offset;
          regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
          dfn().vkCmdCopyImageToBuffer(c, source->image, VK_IMAGE_LAYOUT_GENERAL, depth_scratch,
                                       has_stencil ? 2 : 1, regions);
          VkMemoryBarrier to_compute = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                        VK_ACCESS_TRANSFER_WRITE_BIT,
                                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
          dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &to_compute, 0,
                                     nullptr, 0, nullptr);
          uint32_t count = uint32_t(texels);
          uint32_t groups = (count + 63) / 64;
          constexpr uint32_t kGroupsPerRow = 4096;
          uint32_t params[5] = {count, kGroupsPerRow * 64, uint32_t(stencil_offset / 4),
                                uint32_t(packed_offset / 4),
                                (source->format == VK_FORMAT_D24_UNORM_S8_UINT ? 1u : 0u) |
                                    (has_stencil ? 2u : 0u)};
          dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_convert_pipeline);
          dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_convert_layout,
                                        0, 1, &depth_convert_set, 0, nullptr);
          dfn().vkCmdPushConstants(c, depth_convert_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(params), params);
          dfn().vkCmdDispatch(c, std::min(groups, kGroupsPerRow),
                              (groups + kGroupsPerRow - 1) / kGroupsPerRow, 1);
          VkMemoryBarrier to_copy = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
          dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_copy, 0, nullptr,
                                     0, nullptr);
          VkBufferImageCopy out = regions[0];
          out.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          dfn().vkCmdCopyBufferToImage(c, depth_scratch, dest->image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                       &out);
          // The words only for unorm depth: float24 would need its own
          // encoding, and the game samples those buffers (shadow maps) as depth.
          if (unorm_guest) {
            ResolveDestination* words = textures.GetResolveDestination(
                (dest_base & 0x1FFFFFFF) | TextureCache::kDepthWordsTag, dest_width, dest_height,
                VK_FORMAT_R8G8B8A8_UNORM, uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA), c);
            if (words) {
              out.bufferOffset = packed_offset;
              dfn().vkCmdCopyBufferToImage(c, depth_scratch, words->image,
                                           VK_IMAGE_LAYOUT_GENERAL, 1, &out);
              CountSizedCopies(*words);
              textures.CopyToSized(*words, c);
            }
          }
        } else {
          probe_path = 7;
          static bool logged = false;
          if (!logged) {
            logged = true;
            REXLOG_WARN("[native] Depth resolve not done: {}x{}, {}x MSAA", width, height,
                        1u << msaa);
          }
        }
      } else if (!msaa && textures.StorageCapable(dest->format) &&
                 ComputeColorCopy(c, source->view, dest->view, x0, y0, x0, y0, width, height,
                                  sample_x, sample_y, resolve_scale, resolve_clamp,
                                  textures.ColorAttachmentCapable(dest->format)
                                      ? dest->format
                                      : VK_FORMAT_UNDEFINED)) {
        probe_path = 1;
        // Mercenaries 2: done by compute with the resolve's exponent bias, the destination
        // format's range and the samples averaged (copy_color.cs.glsl).
      } else if (sample_x * sample_y > 1) {
        probe_path = 4;
        // Samples drawn as pixels (SampleScaleX/Y): a filtered blit averages each
        // pixel's samples, as the guest's resolve does.
        VkImageBlit blit = {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.srcOffsets[0] = {x0 * int32_t(sample_x), y0 * int32_t(sample_y), 0};
        blit.srcOffsets[1] = {(x0 + int32_t(width)) * int32_t(sample_x),
                              (y0 + int32_t(height)) * int32_t(sample_y), 1};
        blit.dstOffsets[0] = {x0, y0, 0};
        blit.dstOffsets[1] = {x0 + int32_t(width), y0 + int32_t(height), 1};
        Extra(device).vkCmdBlitImage(c, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                                     VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
      } else if (msaa) {
        probe_path = 5;
        VkImageResolve resolve = {};
        resolve.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        resolve.dstSubresource = resolve.srcSubresource;
        resolve.srcOffset = {x0, y0, 0};
        resolve.dstOffset = {x0, y0, 0};
        resolve.extent = {width, height, 1};
        vkCmdResolveImage(c, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                                VK_IMAGE_LAYOUT_GENERAL, 1, &resolve);
      } else if (source->samples == VK_SAMPLE_COUNT_1_BIT && textures.StorageCapable(dest->format) &&
                 ComputeColorCopy(c, source->view, dest->view, x0, y0, x0, y0, width, height, 1,
                                  1, 1.0f, 0,
                                  textures.ColorAttachmentCapable(dest->format)
                                      ? dest->format
                                      : VK_FORMAT_UNDEFINED)) {
        probe_path = 1;
        // Done by compute (see copy_color.cs.glsl).
      } else {
        probe_path = 6;
        VkImageCopy region = {};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.srcOffset = {x0, y0, 0};
        region.dstOffset = {x0, y0, 0};
        region.extent = {width, height, 1};
        Extra(device).vkCmdCopyImage(c, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                             VK_IMAGE_LAYOUT_GENERAL, 1, &region);
      }
      if (!atlas_written) {
        CountSizedCopies(*dest);
        textures.CopyToSized(*dest, c);
      }
      if (post_dump_next_resolve) {
        post_dump_next_resolve = false;
        RecordImageDump(dest->image, dest->format, dest->width, dest->height,
                        fmt::format("cap{}_edge_out_{:08X}", capture_count,
                                    dest_base & 0x1FFFFFFF));
      }
    }
    if (resolve_probe_enabled) {
      uint32_t scale_bits = 0;
      std::memcpy(&scale_bits, &resolve_scale, sizeof(scale_bits));
      resolve_probe.Record({probe_path, width, height, uint32_t(source->format),
                            uint32_t(dest_format), sample_x, sample_y, msaa, scale_bits,
                            resolve_clamp, dest_base & 0x1FFFFFFF,
                            uint32_t(copy_control.depth_clear_enable) |
                              (uint32_t(copy_control.color_clear_enable) << 1)},
                           uint64_t(width) * height);
    }
  }

  // Clear (RenderTargetCache::PerformTransfersAndResolveClears).
  bool clear_depth = copy_control.depth_clear_enable != 0;
  bool clear_color = !is_depth && copy_control.color_clear_enable != 0;
  if (clear_depth || clear_color) {
    // After a copy the clear must wait for it (it reads what the clear
    // overwrites). Without one, the clear only follows earlier rendering,
    // which the pending resolve barrier or pass order already covers.
    if (copy) {
      VkMemoryBarrier to_clear = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                  VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
      dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                 0, 1, &to_clear, 0, nullptr, 0, nullptr);
    }
    PassTargets clear_targets;
    RenderTarget* depth_target = nullptr;
    RenderTarget* color_target = nullptr;
    if (clear_depth) {
      auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
      RenderTargetKey key = {};
      key.base_tiles = depth_info.depth_base;
      key.pitch = surface_info.surface_pitch * sample_x;
      key.format = uint32_t(depth_info.depth_format);
      key.msaa = msaa;
      key.is_depth = 1;
      depth_target = GetRenderTarget(key, uint32_t(y1) * sample_y);
      clear_targets.depth = depth_target;
    }
    if (clear_color) {
      color_target = source;
      clear_targets.color[0] = color_target;
    }
    EndPass(PassEnd::ClearBegin);
    if (BeginPass(clear_targets)) {
      c = cmd();
      VkClearAttachment attachments[2] = {};
      uint32_t count = 0;
      if (depth_target) {
        VkClearAttachment& a = attachments[count++];
        a.aspectMask = HasStencil(depth_target->format)
                           ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                           : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT);
        uint32_t clear = r[rex::graphics::XE_GPU_REG_RB_DEPTH_CLEAR];
        uint32_t depth24 = clear >> 8;
        a.clearValue.depthStencil.depth =
            xenos::DepthRenderTargetFormat(depth_target->key.format) ==
                    xenos::DepthRenderTargetFormat::kD24FS8
                ? xenos::Float20e4To32(depth24)
                : xenos::UNorm24To32(depth24);
        a.clearValue.depthStencil.stencil = clear & 0xFF;
      }
      if (color_target) {
        VkClearAttachment& a = attachments[count++];
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        a.colorAttachment = 0;
        // Xenia draw_util.h: for 64bpp formats RB_COLOR_CLEAR_LO holds the low 32 bits and
        // RB_COLOR_CLEAR the high ones; 32bpp values come from RB_COLOR_CLEAR alone.
        const bool clear_64bpp = IsColorFormat64bpp(xenos::ColorRenderTargetFormat(color_target->key.format));
        uint64_t value = clear_64bpp ? ((uint64_t(r[rex::graphics::XE_GPU_REG_RB_COLOR_CLEAR]) << 32) |
                                        r[rex::graphics::XE_GPU_REG_RB_COLOR_CLEAR_LO])
                                     : uint64_t(r[rex::graphics::XE_GPU_REG_RB_COLOR_CLEAR]);
        using F = xenos::ColorRenderTargetFormat;
        switch (F(color_target->key.format)) {
          case F::k_8_8_8_8:
          case F::k_8_8_8_8_GAMMA:
            for (uint32_t j = 0; j < 4; ++j) {
              a.clearValue.color.float32[j] = float((value >> (j * 8)) & 0xFF) / 255.0f;
            }
            break;
          case F::k_2_10_10_10:
          case F::k_2_10_10_10_AS_10_10_10_10:
            for (uint32_t j = 0; j < 3; ++j) {
              a.clearValue.color.float32[j] = float((value >> (j * 10)) & 0x3FF) / 1023.0f;
            }
            a.clearValue.color.float32[3] = float((value >> 30) & 0x3) / 3.0f;
            break;
          case F::k_2_10_10_10_FLOAT:
          case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
            for (uint32_t j = 0; j < 3; ++j) {
              a.clearValue.color.float32[j] = xenos::Float7e3To32((value >> (j * 10)) & 0x3FF);
            }
            a.clearValue.color.float32[3] = float((value >> 30) & 0x3) / 3.0f;
            break;
          case F::k_16_16_FLOAT:
          case F::k_16_16_16_16_FLOAT:
            for (uint32_t j = 0; j < 4; ++j) {
              a.clearValue.color.float32[j] =
                  rex::xenos_half_to_float(uint16_t((value >> (j * 16)) & 0xFFFF));
            }
            break;
          case F::k_16_16:
          case F::k_16_16_16_16:
            for (uint32_t j = 0; j < 4; ++j) {
              a.clearValue.color.float32[j] = std::max(
                  float(int16_t((value >> (j * 16)) & 0xFFFF)) * (32.0f / 32767.0f), -32.0f);
            }
            break;
          case F::k_32_FLOAT: {
            uint32_t bits = uint32_t(value);
            std::memcpy(&a.clearValue.color.float32[0], &bits, 4);
          } break;
          case F::k_32_32_FLOAT: {
            uint32_t bits[2] = {uint32_t(value), uint32_t(value >> 32)};
            std::memcpy(a.clearValue.color.float32, bits, 8);
          } break;
          default:
            break;
        }
      }
      VkClearRect rect = {};
      const uint32_t cx = uint32_t(x0) * sample_x, cy = uint32_t(y0) * sample_y;
      rect.rect.offset = {int32_t(cx), int32_t(cy)};
      rect.rect.extent = {std::min(width * sample_x, pass_extent.width - std::min(cx, pass_extent.width)),
                          std::min(height * sample_y, pass_extent.height - std::min(cy, pass_extent.height))};
      rect.layerCount = 1;
      if (rect.rect.extent.width && rect.rect.extent.height) {
        dfn().vkCmdClearAttachments(c, count, attachments, 1, &rect);
      }
      EndPass(PassEnd::ClearEnd);
    }
  }

  // The barrier that makes the copies and clears visible is recorded before
  // the next rendering or present, so a run of resolves shares one (each
  // barrier is a full wait for idle on this GPU).
  resolve_barrier_pending = true;
}

void Renderer::Impl::FlushResolveBarrier() {
  if (!resolve_barrier_pending) {
    return;
  }
  resolve_barrier_pending = false;
  VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_TRANSFER_READ_BIT};
  dfn().vkCmdPipelineBarrier(cmd(),
                             VK_PIPELINE_STAGE_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &after, 0, nullptr, 0, nullptr);
}

// --- Swap ------------------------------------------------------------------------------

void Renderer::Impl::Swap(rex::ui::Presenter* presenter, uint32_t frontbuffer, uint32_t width,
                          uint32_t height) {
  p2::gpuwait::Scope gpu_wait_scope(p2::gpuwait::NativeSwap);
  // Water debug cycle: advance once a frame after the water appeared (cached setups do not
  // come back to BuildDrawSetup on their own).
  if (water_debug_next != std::chrono::steady_clock::time_point{}) UpdateWaterDebug();
  if (water_dump_enabled || post_trace_seconds || capture_combo || auto_capture_draws) {
    FinishWaterDumps(false);
    RecordWaterDumps();
  }
  if (capture_all && frame > water_sim_trace_frame) {
    capture_all = false;
    REXLOG_INFO("[native] CAPTURE {} done ({} lines)", capture_count, water_sim_trace_lines);
  }
  if (capture_combo && !capture_all) PollCaptureCombo();
  if (auto_capture_draws && !capture_all && auto_capture_done < 2 && capture_count < 6) {
    // run16: a hard reset on every lighter frame never reached the streak at 7-10 fps; a dip
    // now only steps it back.
    auto_capture_streak = frame_draw_count >= auto_capture_draws ? auto_capture_streak + 1
                          : (auto_capture_streak ? auto_capture_streak - 1 : 0);
    const auto now = std::chrono::steady_clock::now();
    if (auto_capture_streak >= 100 && now >= auto_capture_next) {
      ++auto_capture_done;
      auto_capture_next = now + std::chrono::seconds(30);
      auto_capture_streak = 0;
      ArmCapture("automatic: gameplay draw count");
    }
  }
  // VdSwap describes the frontbuffer in texture fetch constant 0, swizzle
  // included; the SDK presents from it too (RequestSwapTexture). Without the
  // swizzle, red and blue come out swapped.
  xenos::xe_gpu_texture_fetch_t fetch = regs->GetTextureFetch(0);
  uint32_t address = frontbuffer & 0x1FFFFFFF;
  uint32_t guest_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  if (fetch.type == xenos::FetchConstantType::kTexture && fetch.base_address) {
    address = fetch.base_address << 12;
    guest_swizzle = fetch.swizzle;
  }
  uint32_t source_width = 0, source_height = 0;
  VkImageView source_view =
      textures.GetResolveDestinationView2D(address, guest_swizzle, &source_width, &source_height);
  static uint32_t missing_logged = 0;
  if (!source_view && missing_logged < 8) {
    ++missing_logged;
    REXLOG_WARN("[native] Nothing was resolved to the frontbuffer {:08X} ({}x{})", address, width,
                height);
  }
  static bool swap_logged = false;
  if (source_view && !swap_logged) {
    swap_logged = true;
    REXLOG_INFO("[native] Frontbuffer {:08X} {}x{}, swizzle {:03X}", address, width, height,
                guest_swizzle);
  }
  if (presenter && source_view) {
    width = std::min(width ? width : source_width, source_width);
    height = std::min(height ? height : source_height, source_height);
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    uint32_t display_width = std::max(1u, uint32_t(mode.display_width));
    uint32_t display_height = std::max(1u, uint32_t(mode.display_height));
    presenter->RefreshGuestOutput(
        width, height, display_width, display_height,
        [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          auto& vulkan_context = static_cast<
              rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
          BeginRecording();
          EndPass(PassEnd::Present);
          uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
          std::memcpy(ramp_mapping + 256 * sizeof(uint32_t) * slot, gamma_ramp,
                      sizeof(gamma_ramp));
          VkDescriptorImageInfo source_info = {VK_NULL_HANDLE, source_view,
                                               VK_IMAGE_LAYOUT_GENERAL};
          VkDescriptorImageInfo dest_info = {VK_NULL_HANDLE, vulkan_context.image_view(),
                                             VK_IMAGE_LAYOUT_GENERAL};
          VkWriteDescriptorSet writes[2] = {};
          writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          writes[0].dstSet = swap_sets[slot][1];
          writes[0].descriptorCount = 1;
          writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          writes[0].pImageInfo = &source_info;
          writes[1] = writes[0];
          writes[1].dstSet = swap_sets[slot][2];
          writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          writes[1].pImageInfo = &dest_info;
          dfn().vkUpdateDescriptorSets(vk, 2, writes, 0, nullptr);
          FlushResolveBarrier();
          VkCommandBuffer c = cmd();
          VkImageMemoryBarrier barrier = {};
          barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
          barrier.srcAccessMask =
              vulkan_context.image_ever_written_previously()
                  ? rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask
                  : 0;
          barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
          barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.image = vulkan_context.image();
          barrier.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
          dfn().vkCmdPipelineBarrier(
              c,
              vulkan_context.image_ever_written_previously()
                  ? rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask
                  : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
          dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, swap_pipeline);
          dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, swap_pipeline_layout, 0,
                                        3, swap_sets[slot], 0, nullptr);
          uint32_t size[2] = {width, height};
          dfn().vkCmdPushConstants(c, swap_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(size), size);
          dfn().vkCmdDispatch(c, (width + 15) / 16, (height + 7) / 8, 1);
          barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          barrier.dstAccessMask = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
          barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
          barrier.newLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
          dfn().vkCmdPipelineBarrier(
              c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0,
              nullptr, 1, &barrier);
          // Submitted before returning, so the presenter's own work comes
          // after it on the queue.
          Submit();
          context.SetIs8bpc(true);
          return true;
        });
  }
  Submit();
  // Frame boundary.
  RexSwitchPerfSceneFrame(uint32_t(frame_draw_count));
  auto now = std::chrono::steady_clock::now();
  if (REXCVAR_GET(p2_native_copy_ab) && frame_draw_count >= 1200) {
    if (!p2_copy_scene_start.time_since_epoch().count()) p2_copy_scene_start = now;
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now-p2_copy_scene_start).count();
    const int phase = seconds < 30 ? 0 : seconds < 75 ? 1 : 2;
    if (phase != p2_copy_phase) {
      p2_copy_phase = phase;
      fragment_copies = phase == 1 && copy_fragment_layout != VK_NULL_HANDLE;
      p2_frame_count = 0;
      REXLOG_INFO("[p2-copy-ab] phase={} scene_seconds={} colour_copy={} ; discard first 5 seconds for pipeline warmup", phase, seconds,
          fragment_copies ? "fragment" : "compute");
    }
  }
  if (last_swap_time.time_since_epoch().count()) {
    if (REXCVAR_GET(p2_frame_diagnostics) && frame_draw_count >= 1200) {
      p2_frame_ms[p2_frame_count++] = std::chrono::duration<double, std::milli>(now-last_swap_time).count();
      if (p2_frame_count == p2_frame_ms.size()) {
        auto sorted = p2_frame_ms;
        std::sort(sorted.begin(), sorted.end());
        double total = 0; size_t late = 0;
        for (double v : sorted) { total += v; late += v > 33.5; }
        REXLOG_INFO("[p2-frame] copy_phase={} 300 scene frames: fps={:.2f} median={:.2f} p95={:.2f} p99={:.2f} max={:.2f} ms over33.5ms={}",
            p2_copy_phase, 300000.0/total, sorted[149], sorted[284], sorted[296], sorted.back(), late);
        p2_frame_count = 0;
      }
    } else { p2_frame_count = 0; }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_swap_time).count();
    if (ms > 50) {
      RendererStats& st = stats();
      ++st.frames_over_50ms;
      if (ms > 100) ++st.frames_over_100ms;
      st.slow_frame_pipelines += st.pipelines_created - at_last_swap.pipelines_created;
      st.slow_frame_shaders += st.shaders_translated - at_last_swap.shaders_translated;
      st.slow_frame_uploads += textures.stats().uploads - at_last_swap.textures_uploaded;
      st.slow_frame_render_targets +=
          st.render_targets_created - at_last_swap.render_targets_created;
    }
  }
  last_swap_time = now;
  at_last_swap = stats();
  at_last_swap.textures_uploaded = textures.stats().uploads;
  // Save the driver's pipeline cache once new pipelines stop appearing for a
  // while, off this thread: the file is tens of megabytes on the SD card.
  if (!driver_cache_path.empty() && stats().pipelines_created != pipelines_at_last_save &&
      now - last_pipeline_time > std::chrono::seconds(5) && !driver_cache_writing.load()) {
    SaveDriverCache();
  }
  if (prewarm_enabled && !prewarm_path.empty() && prewarm_keys.size() != prewarm_saved_keys &&
      now - last_pipeline_time > std::chrono::seconds(5) && !prewarm_writing.load()) {
    SavePrewarm();
  }
  stats().textures_uploaded = textures.stats().uploads;
  stats().texture_bytes_uploaded = textures.stats().upload_bytes;
  stats().textures_created = textures.stats().created;
  stats().textures_streamed = textures.stats().streamed;
  stats().textures_pending = textures.stats().pending;
  stats().textures_replaced = textures.stats().replaced;
  stats().frame_upload_wait_ns = frame_upload_wait_ns;
  stats().frame_fence_wait_ns = frame_fence_wait_ns;
  stats().frame_drain_wait_ns = frame_drain_wait_ns;
  stats().frame_texture_ns = frame_texture_ns;
  stats().frame_draws = frame_draw_count;
  stats().frame_stream_wait_ns = frame_stream_wait_ns;
  frame_stream_wait_ns = 0;
  frame_upload_wait_ns = frame_fence_wait_ns = frame_drain_wait_ns = frame_texture_ns = 0;
  frame_draw_count = 0;
  shadow_cascade = 0;
  ++frame;
  p2::skinlog::g_frame.store(uint32_t(frame), std::memory_order_relaxed);
  if (frame % 600 == 0) {
    REXLOG_INFO("[native-occlusion] matching: address ends {}, overlapping begins {}, repeated begins {}, unmatched ends {}, demand submissions {}",
      occlusion_address_ends,occlusion_overlaps,occlusion_abandoned,occlusion_unmatched_ends,occlusion_demand_submits);
    REXLOG_INFO("[native] last 600 frames: skinned ranges {} rewritten / {} unchanged (no hashing), {} MB of "
                "vertex/index data hashed; depth prepass draws skipped {}, reflection draws skipped {}",
                skin_log_fresh, skin_log_same, (vertex_hash_bytes - hash_bytes_at_report) >> 20,
                work_prepass_skipped - prepass_at_report, work_reflection_skipped - reflection_at_report);
    REXLOG_INFO("[native] hashed MB/ranges: index {}/{}, small exact {}/{}, changed exact {}/{}, sampled {}/{}",
                hash_class_bytes[0] >> 20, hash_class_ranges[0], hash_class_bytes[1] >> 20, hash_class_ranges[1],
                hash_class_bytes[2] >> 20, hash_class_ranges[2], hash_class_bytes[3] >> 20, hash_class_ranges[3]);
    REXLOG_INFO("[native] settled small ranges not hashed: {} checks; range table pruned {} times, {} entries",
                settled_skips, vertex_hash_prunes, vertex_hashes.size());
    REXLOG_INFO("[native] skinned-page ranges: {} new, {} exact checks, {} fresh (not hashed)", skin_page_ranges,
                skin_page_exact, skin_page_unique);
    skin_page_ranges = skin_page_exact = skin_page_unique = 0;
    vertex_hash_prunes = 0;
    settled_skips = 0;
    for (int i = 0; i < 4; ++i) hash_class_bytes[i] = hash_class_ranges[i] = 0;
    prepass_at_report = work_prepass_skipped;
    reflection_at_report = work_reflection_skipped;
    skin_log_fresh = skin_log_same = 0;
    hash_bytes_at_report = vertex_hash_bytes;
  }
  profile_frame = profile_pool && frame % 16 == 0;
  PollCompleted();
  ProfileReport();
  ReportWorkStats();
  ReportResolveProbe();
  textures.BeginFrame(frame, completed_frame);
  FreeRetiredSetPools();
  for (auto it = retired_render_targets.begin(); it != retired_render_targets.end();) {
    if (it->frame <= completed_frame) {
      if (it->sample_view) dfn().vkDestroyImageView(vk, it->sample_view, nullptr);
      if (it->stencil_view) dfn().vkDestroyImageView(vk, it->stencil_view, nullptr);
      dfn().vkDestroyImageView(vk, it->view, nullptr);
      dfn().vkDestroyImage(vk, it->image, nullptr);
      dfn().vkFreeMemory(vk, it->memory, nullptr);
      it = retired_render_targets.erase(it);
    } else {
      ++it;
    }
  }
}

// --- Driver pipeline cache ---------------------------------------------------------------

namespace {
constexpr uint64_t kDriverCacheMagic = 0x3148434E54414E52ULL;  // "RNATNCH1"

std::vector<uint8_t> ReadDriverCache(const std::filesystem::path& path) {
  std::vector<uint8_t> bytes;
  if (FILE* file = rex::filesystem::OpenFile(path, "rb")) {
    uint64_t header[3] = {};
    if (fread(header, sizeof(header), 1, file) == 1 && header[0] == kDriverCacheMagic &&
        header[1] >= 32 && header[1] <= 128u * 1024 * 1024) {
      bytes.resize(size_t(header[1]));
      if (fread(bytes.data(), bytes.size(), 1, file) != 1 ||
          XXH3_64bits(bytes.data(), bytes.size()) != header[2]) {
        bytes.clear();
      }
    }
    fclose(file);
  }
  return bytes;
}
}  // namespace

bool Renderer::water_dump_enabled() const { return impl_->water_dump_enabled; }

bool Renderer::TakePacketTraceRequest() {
  const bool request = impl_->packet_trace_request;
  impl_->packet_trace_request = false;
  return request;
}

void Renderer::InitializeDriverCache(const std::filesystem::path& cache_root,
                                     uint32_t title_id) {
  Impl& im = *impl_;
  if (!im.device) {
    return;
  }
  std::error_code error;
  auto directory = cache_root / "shaders" / "driver";
  std::filesystem::create_directories(directory, error);
  if (error) {
    return;
  }
  const auto& properties = im.device->properties();
  im.driver_cache_path =
      directory / fmt::format("native-{:08X}-{:08X}-{:08X}-{:08X}.cache", title_id,
                              properties.vendorID, properties.deviceID, properties.driverVersion);
  // The newest complete file: the rename that replaces it can fail on Horizon
  // (it does not overwrite), leaving the data in .tmp.
  std::vector<uint8_t> data = ReadDriverCache(im.driver_cache_path);
  auto temporary = im.driver_cache_path;
  temporary += ".tmp";
  std::vector<uint8_t> newer = ReadDriverCache(temporary);
  if (newer.size() > data.size()) {
    data = std::move(newer);
  }
  VkPipelineCacheCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  info.initialDataSize = data.size();
  info.pInitialData = data.empty() ? nullptr : data.data();
  VkPipelineCache cache = VK_NULL_HANDLE;
  if (Extra(im.device).vkCreatePipelineCache(im.vk, &info, nullptr, &cache) != VK_SUCCESS) {
    info.initialDataSize = 0;
    info.pInitialData = nullptr;
    if (Extra(im.device).vkCreatePipelineCache(im.vk, &info, nullptr, &cache) != VK_SUCCESS) {
      return;
    }
    data.clear();
  }
  // Called before the guest runs, so the ring thread is not using the old one.
  if (im.driver_cache) {
    Extra(im.device).vkDestroyPipelineCache(im.vk, im.driver_cache, nullptr);
  }
  im.driver_cache = cache;
  im.driver_cache_saved_size = data.size();
  REXLOG_INFO("[native] Driver pipeline cache: {} KB loaded from {}", data.size() >> 10,
              im.driver_cache_path.string());
  im.prewarm_enabled = REXCVAR_GET(p2_native_pipeline_prewarm);
  if (im.prewarm_enabled && !im.prewarm_thread.joinable()) {
    im.LoadPrewarm(directory, title_id);
  }
}

namespace {
constexpr uint64_t kPrewarmMagic = 0x314D5257504E324DULL;  // "M2NPWRM1"
constexpr uint32_t kPrewarmVersion = 1;

// Header: magic, version, sizeof(PipelineKey), shader count, key count, hash of the payload.
// Payload: per shader hash (8), type (4), dword count (4), dwords; then the keys.
bool ParsePrewarm(const std::vector<uint8_t>& bytes, std::vector<uint8_t>* payload_out,
                  uint32_t* shader_count, uint32_t* key_count) {
  constexpr size_t kHeader = 8 + 4 * 4 + 8;
  if (bytes.size() < kHeader) return false;
  uint64_t magic, payload_hash;
  uint32_t fields[4];
  std::memcpy(&magic, bytes.data(), 8);
  std::memcpy(fields, bytes.data() + 8, 16);
  std::memcpy(&payload_hash, bytes.data() + 24, 8);
  if (magic != kPrewarmMagic || fields[0] != kPrewarmVersion ||
      fields[1] != sizeof(PipelineKey) ||
      XXH3_64bits(bytes.data() + kHeader, bytes.size() - kHeader) != payload_hash) {
    return false;
  }
  payload_out->assign(bytes.begin() + kHeader, bytes.end());
  *shader_count = fields[2];
  *key_count = fields[3];
  return true;
}

std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path) {
  std::vector<uint8_t> bytes;
  if (FILE* file = rex::filesystem::OpenFile(path, "rb")) {
    uint8_t chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
      bytes.insert(bytes.end(), chunk, chunk + got);
      if (bytes.size() > (64u << 20)) {
        bytes.clear();
        break;
      }
    }
    fclose(file);
  }
  return bytes;
}
}  // namespace

void Renderer::Impl::LoadPrewarm(const std::filesystem::path& directory, uint32_t title_id) {
  prewarm_path = directory / fmt::format("native-prewarm-{:08X}.bin", title_id);
  auto temporary = prewarm_path;
  temporary += ".tmp";
  std::vector<uint8_t> payload;
  uint32_t shader_count = 0, key_count = 0;
  const char* source = "none";
  for (const auto* path : {&prewarm_path, &temporary}) {
    std::vector<uint8_t> candidate;
    uint32_t candidate_shaders = 0, candidate_keys = 0;
    if (ParsePrewarm(ReadWholeFile(*path), &candidate, &candidate_shaders, &candidate_keys) &&
        candidate_keys >= key_count) {
      payload = std::move(candidate);
      shader_count = candidate_shaders;
      key_count = candidate_keys;
      source = path == &prewarm_path ? "file" : "tmp";
    }
  }
  bool valid = true;
  size_t offset = 0;
  for (uint32_t i = 0; i < shader_count && valid; ++i) {
    PrewarmShader entry;
    uint32_t dwords = 0;
    if (offset + 16 > payload.size()) {
      valid = false;
      break;
    }
    std::memcpy(&entry.hash, payload.data() + offset, 8);
    std::memcpy(&entry.type, payload.data() + offset + 8, 4);
    std::memcpy(&dwords, payload.data() + offset + 12, 4);
    offset += 16;
    if (entry.type > 1 || !dwords || offset + size_t(dwords) * 4 > payload.size()) {
      valid = false;
      break;
    }
    entry.ucode.resize(dwords);
    std::memcpy(entry.ucode.data(), payload.data() + offset, size_t(dwords) * 4);
    offset += size_t(dwords) * 4;
    if (prewarm_shader_hashes.insert(entry.hash).second) {
      prewarm_shader_list.push_back(std::move(entry));
    }
  }
  if (valid && offset + size_t(key_count) * sizeof(PipelineKey) == payload.size()) {
    for (uint32_t i = 0; i < key_count && prewarm_keys.size() < kPrewarmMaxKeys; ++i) {
      PipelineKey key;
      std::memcpy(&key, payload.data() + offset + size_t(i) * sizeof(PipelineKey), sizeof(key));
      if (prewarm_key_hashes.insert(XXH3_64bits(&key, sizeof(key))).second) {
        prewarm_keys.push_back(key);
      }
    }
  } else {
    valid = false;
  }
  if (!valid) {
    prewarm_shader_list.clear();
    prewarm_shader_hashes.clear();
    prewarm_keys.clear();
    prewarm_key_hashes.clear();
  }
  prewarm_saved_keys = prewarm_keys.size();
  REXLOG_INFO("[native] pipeline prewarm: {} pipelines and {} shaders listed ({}{}) in {}",
              prewarm_keys.size(), prewarm_shader_list.size(), source,
              valid ? "" : ", damaged or other version: starting a new list",
              prewarm_path.string());
  if (!prewarm_keys.empty() && !prewarm_thread.joinable()) {
    prewarm_thread = std::thread(
        [this, list_shaders = prewarm_shader_list, keys = prewarm_keys]() mutable {
          PrewarmThread(std::move(list_shaders), std::move(keys));
        });
  }
}

void Renderer::Impl::SavePrewarm() {
  prewarm_saved_keys = prewarm_keys.size();
  auto data = std::make_shared<std::vector<uint8_t>>();
  std::vector<uint8_t>& out = *data;
  constexpr size_t kHeader = 8 + 4 * 4 + 8;
  out.resize(kHeader);
  for (const PrewarmShader& entry : prewarm_shader_list) {
    const uint32_t dwords = uint32_t(entry.ucode.size());
    const size_t at = out.size();
    out.resize(at + 16 + size_t(dwords) * 4);
    std::memcpy(out.data() + at, &entry.hash, 8);
    std::memcpy(out.data() + at + 8, &entry.type, 4);
    std::memcpy(out.data() + at + 12, &dwords, 4);
    std::memcpy(out.data() + at + 16, entry.ucode.data(), size_t(dwords) * 4);
  }
  const size_t keys_at = out.size();
  out.resize(keys_at + prewarm_keys.size() * sizeof(PipelineKey));
  std::memcpy(out.data() + keys_at, prewarm_keys.data(), prewarm_keys.size() * sizeof(PipelineKey));
  const uint64_t magic = kPrewarmMagic;
  const uint32_t fields[4] = {kPrewarmVersion, uint32_t(sizeof(PipelineKey)),
                              uint32_t(prewarm_shader_list.size()), uint32_t(prewarm_keys.size())};
  const uint64_t payload_hash = XXH3_64bits(out.data() + kHeader, out.size() - kHeader);
  std::memcpy(out.data(), &magic, 8);
  std::memcpy(out.data() + 8, fields, 16);
  std::memcpy(out.data() + 24, &payload_hash, 8);
  prewarm_writing = true;
  if (prewarm_writer.joinable()) {
    prewarm_writer.join();
  }
  std::filesystem::path path = prewarm_path;
  const size_t key_count = prewarm_keys.size();
  prewarm_writer = std::thread([this, data, path, key_count]() {
    rex::thread::set_current_thread_name("GPU prewarm save");
    auto temporary = path;
    temporary += ".tmp";
    bool written = false;
    if (FILE* file = rex::filesystem::OpenFile(temporary, "wb")) {
      written = fwrite(data->data(), data->size(), 1, file) == 1;
      if (fclose(file) != 0) written = false;
      if (written) {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::rename(temporary, path, error);
      }
    }
    REXLOG_INFO("[native] pipeline prewarm list saved: {} pipelines, {} KB{}", key_count,
                data->size() >> 10, written ? "" : " (write failed)");
    prewarm_writing = false;
  });
}

void Renderer::Impl::SaveDriverCache() {
  pipelines_at_last_save = stats().pipelines_created;
  size_t size = 0;
  if (Extra(device).vkGetPipelineCacheData(vk, driver_cache, &size, nullptr) != VK_SUCCESS ||
      size < 32 || size == driver_cache_saved_size) {
    return;
  }
  auto data = std::make_shared<std::vector<uint8_t>>(size);
  if (Extra(device).vkGetPipelineCacheData(vk, driver_cache, &size, data->data()) != VK_SUCCESS) {
    return;
  }
  data->resize(size);
  driver_cache_saved_size = size;
  driver_cache_writing = true;
  std::filesystem::path path = driver_cache_path;
  // Joined, never detached: detaching throws on Horizon, which ended the game.
  if (driver_cache_writer.joinable()) {
    driver_cache_writer.join();
  }
  driver_cache_writer = std::thread([this, data, path]() {
    rex::thread::set_current_thread_name("GPU cache save");
    auto temporary = path;
    temporary += ".tmp";
    if (FILE* file = rex::filesystem::OpenFile(temporary, "wb")) {
      const uint64_t header[] = {kDriverCacheMagic, uint64_t(data->size()),
                                 XXH3_64bits(data->data(), data->size())};
      bool written = fwrite(header, sizeof(header), 1, file) == 1 &&
                     fwrite(data->data(), data->size(), 1, file) == 1;
      if (fclose(file) != 0) written = false;
      if (written) {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::rename(temporary, path, error);
      }
    }
    driver_cache_writing = false;
  });
}

// --- Renderer ---------------------------------------------------------------------------

Renderer::Renderer() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
Renderer::~Renderer() { Shutdown(); }

bool Renderer::Initialize(rex::ui::vulkan::VulkanProvider* provider, rex::memory::Memory* memory,
                          RegisterFile* regs) {
  impl_->provider = provider;
  impl_->memory = memory;
  impl_->regs = regs;
  if (!impl_->Initialize()) {
    impl_->Shutdown();
    return false;
  }
  return true;
}

std::string Renderer::TakeTextureUploadReport() {
  return impl_ ? impl_->textures.TakeUploadReport() : std::string();
}

void Renderer::Flush() {
  if (impl_) impl_->Submit();
}

void Renderer::KickRecorder() {
  if (impl_) impl_->KickRecorder();
}

void Renderer::Shutdown() {
  if (impl_) {
    impl_->Shutdown();
  }
}

void Renderer::LoadShader(xenos::ShaderType type, const uint32_t* ucode, uint32_t dword_count,
                          uint32_t guest_address) {
  // The game switches shaders for most draws, loading them from the same few
  // hundred addresses: remember them by address and size, confirmed by a hash
  // of all of the code. A sample of it (the first, middle and last 16 words)
  // was tried first and was not enough: a level loading its shaders where the
  // previous screen's had been got the old shader whenever the two agreed at
  // the sampled words, and drew its world with the wrong vertex shader.
  // A full hash on every load (twice a draw, 12 MB a frame at 3000 draws) was
  // too slow, so each location is verified in full once a frame, and within
  // the frame by its first words: shaders are replaced between screens, not
  // in the middle of drawing one.
  uint64_t location = 0;
  uint64_t sample = 0;
  uint64_t head = 0;
  if (guest_address) {
    location = (uint64_t(guest_address) << 20) | (uint64_t(dword_count) << 2) | uint64_t(type);
    auto known = impl_->shaders_by_location.find(location);
    head = XXH3_64bits(ucode, std::min(dword_count, 16u) * sizeof(uint32_t));
    if (known != impl_->shaders_by_location.end() &&
        known->second.verified_frame == impl_->frame && known->second.head == head) {
      if (type == xenos::ShaderType::kVertex) {
        impl_->active_vs = known->second.shader;
      } else {
        impl_->active_ps = known->second.shader;
      }
      return;
    }
    sample = XXH3_64bits(ucode, dword_count * sizeof(uint32_t));
    if (known != impl_->shaders_by_location.end() && known->second.sample == sample) {
      known->second.verified_frame = impl_->frame;
      known->second.head = head;
      if (type == xenos::ShaderType::kVertex) {
        impl_->active_vs = known->second.shader;
      } else {
        impl_->active_ps = known->second.shader;
      }
      return;
    }
  }
  uint64_t hash = guest_address ? sample : XXH3_64bits(ucode, dword_count * sizeof(uint32_t));
  auto& shaders = impl_->shaders;
  auto it = shaders.find(hash);
  VulkanShader* shader;
  if (it != shaders.end()) {
    shader = it->second.get();
  } else {
    std::unique_ptr<VulkanShader> created;
    if (impl_->prewarm_published.load(std::memory_order_acquire)) {
      // Already translated by the pipeline prewarm (HANDOFF B:93).
      std::lock_guard<std::mutex> lock(impl_->prewarm_mutex);
      auto found = impl_->prewarmed_shaders.find(hash);
      if (found != impl_->prewarmed_shaders.end() && found->second->type() == type &&
          found->second->ucode_dword_count() == dword_count) {
        created = std::move(found->second);
        impl_->prewarmed_shaders.erase(found);
        ++impl_->stats().prewarm_shaders;
      }
    }
    if (!created) {
      created = std::make_unique<VulkanShader>(impl_->device, type, hash, ucode, dword_count);
    }
    shader = created.get();
    shaders.emplace(hash, std::move(created));
  }
  if (type == xenos::ShaderType::kVertex) {
    impl_->active_vs = shader;
  } else {
    impl_->active_ps = shader;
  }
  if (location) {
    impl_->shaders_by_location[location] = {shader, sample, impl_->frame, head};
  }
}

namespace {
struct CpuTimer {
  uint64_t& total;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  explicit CpuTimer(uint64_t& t) : total(t) {}
  ~CpuTimer() {
    total += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count());
  }
};
}  // namespace

void Renderer::Draw(const uint32_t* immediate_indices, uint32_t immediate_index_words) {
  CpuTimer timer(stats_.cpu_ns);
  impl_->Draw(immediate_indices, immediate_index_words);
}

void Renderer::Resolve() {
  CpuTimer timer(stats_.cpu_ns);
  impl_->Resolve();
}

void Renderer::Swap(rex::ui::Presenter* presenter, uint32_t frontbuffer, uint32_t width,
                    uint32_t height) {
  CpuTimer timer(stats_.cpu_ns);
  impl_->Swap(presenter, frontbuffer, width, height);
}

void Renderer::SetGammaRamp256(const uint32_t* entries) {
  std::memcpy(impl_->gamma_ramp, entries, sizeof(impl_->gamma_ramp));
}

}  // namespace p2::native

namespace p2::native {
bool Renderer::OcclusionQueryEvent(uint32_t address,bool end){return impl_->OcclusionEvent(address,end);}
void Renderer::PollOcclusionQueries(bool demand){impl_->PollOcclusionQueries(demand);}
}

bool p2::native::Renderer::IsOcclusionResultAddress(uint32_t address) const {
  if(!impl_)return false;
  for(uint32_t i=0;i<512;++i)if(impl_->occlusion.IsEnded(i)){
    const uint32_t base=impl_->occlusion.address(i);
    if(uint32_t(address-base)<32u)return true;
  }
  return false;
}
