// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native renderer for the Switch port: draws the game's frames with Vulkan
// directly from the Xbox 360 GPU state the command ring leaves in the registers,
// without emulating the GPU's embedded memory (EDRAM).
//
// The design follows the native renderer of nfsmw-nx (StevensND/nfsmw-nx,
// app/src/nfsmw_nativo_*.cpp, GPL-3.0):
//   - Render targets are ordinary Vulkan images, one per EDRAM address, format,
//     pitch and sample count. Nothing is ever copied in or out of an EDRAM
//     buffer, so there are no ownership transfers.
//   - A resolve is an image copy into a texture registered at the destination
//     address. Textures fetched from that address, and the frontbuffer, use it
//     directly; the guest memory is not written.
//   - Vertex and index data are copied per submission into arenas the shaders
//     read, and deduplicated by content.
// Where nfsmw-nx uses an offline library of shaders recompiled for Need for
// Speed, this uses ReXGlue's runtime shader translator, the same one the
// emulated path uses, so the game's shaders need no preparation.
//
// Everything here runs on the thread that consumes the command ring.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class Presenter;
}
namespace rex::ui::vulkan {
class VulkanProvider;
}

namespace p2::native {

struct RendererStats {
  // BindingReuse: hit, new, identity, descriptor generation, answers, views, expiry.
  uint64_t texture_binding_cache[7] = {};
  uint64_t texture_binding_refresh_attempts = 0, texture_binding_refresh_kept = 0;
  uint64_t draws = 0;
  uint64_t draws_skipped = 0;
  uint64_t resolves = 0;
  uint64_t pipelines_created = 0;
  uint64_t shaders_translated = 0;
  // Ring thread time spent translating shaders and creating pipelines (HANDOFF B:93).
  uint64_t translate_ns = 0;
  uint64_t pipeline_ns = 0;
  // Taken from the pipeline prewarm instead of being made on the ring.
  uint64_t prewarm_shaders = 0;
  uint64_t prewarm_pipelines = 0;
  uint64_t textures_uploaded = 0;
  uint64_t texture_bytes_uploaded = 0;
  uint64_t textures_created = 0;
  uint64_t textures_streamed = 0;
  uint64_t textures_pending = 0;
  uint64_t textures_replaced = 0;
  uint64_t vertex_bytes_copied = 0;
  uint64_t render_passes = 0;
  uint64_t submissions = 0;
  // Renderer CPU time on the ring thread (draws, resolves, swaps), and GPU
  // time of the completed submissions (raw timestamp ticks times the
  // reported period; on NVK for this GPU that reads about 1.6x low).
  uint64_t cpu_ns = 0;
  uint64_t gpu_ns = 0;
  uint64_t gpu_submissions = 0;
  // Frames (swap to swap on the ring thread) longer than 50 and 100 ms, and
  // what those frames did.
  uint64_t frames_over_50ms = 0;
  uint64_t frames_over_100ms = 0;
  uint64_t slow_frame_pipelines = 0;
  uint64_t slow_frame_shaders = 0;
  uint64_t slow_frame_uploads = 0;
  uint64_t slow_frame_render_targets = 0;
  uint64_t render_targets_created = 0;
  // How often the cached draw state and texture bindings had to be rebuilt.
  uint64_t setups_built = 0;
  uint64_t texture_bindings_built = 0;
  // Where the last frame's time went on the ring thread, for reporting slow
  // frames: waits for texture untiling, for the GPU (a submission slot or
  // stream space), for the draw recording thread, and time spent finding
  // textures; set at each swap.
  uint64_t frame_upload_wait_ns = 0;
  uint64_t frame_fence_wait_ns = 0;
  uint64_t frame_drain_wait_ns = 0;
  uint64_t frame_texture_ns = 0;
  uint64_t frame_draws = 0;
  // The part of frame_fence_wait_ns spent waiting for stream space.
  uint64_t frame_stream_wait_ns = 0;
  // Draws skipped, by reason (see SkipReason in the .cpp).
  uint64_t skipped_by_reason[16] = {};
};

class Renderer {
 public:
  Renderer();
  ~Renderer();

  bool Initialize(rex::ui::vulkan::VulkanProvider* provider, rex::memory::Memory* memory,
                  rex::graphics::RegisterFile* regs);
  void Shutdown();

  // PM4_IM_LOAD / IM_LOAD_IMMEDIATE: the microcode in guest byte order.
  // guest_address is where the microcode lives (0 when it came in the packet).
  void LoadShader(rex::graphics::xenos::ShaderType type, const uint32_t* ucode_big_endian,
                  uint32_t dword_count, uint32_t guest_address);

  // A draw in the color/depth EDRAM modes. index_words holds the indices of an
  // immediate-index draw (DRAW_INDX with the indices in the packet), already in
  // host byte order; null otherwise.
  void Draw(const uint32_t* immediate_indices, uint32_t immediate_index_words);

  // A draw in the copy EDRAM mode: resolve and/or clear.
  void Resolve();

  // Asynchronous native sample queries. Guest counters complete after a GPU fence.
  bool OcclusionQueryEvent(uint32_t address, bool end);
  void PollOcclusionQueries(bool demand = false);
  bool IsOcclusionResultAddress(uint32_t address) const;

  // PM4_XE_SWAP. Presents the texture resolved at the frontbuffer address.
  void Swap(rex::ui::Presenter* presenter, uint32_t frontbuffer_address, uint32_t width,
            uint32_t height);

  // Loads the driver's compiled pipelines saved by an earlier session, and
  // saves them there as new ones appear. Call before the guest starts.
  void InitializeDriverCache(const std::filesystem::path& cache_root, uint32_t title_id);
  // Water diagnostic (HANDOFF B:100): the command processor keeps a packet history while this is
  // on, and logs it when the water simulation draw has been dumped.
  bool water_dump_enabled() const;
  bool TakePacketTraceRequest();

  // The 256-entry gamma ramp, as the DC_LUT_30_COLOR register packs it.
  void SetGammaRamp256(const uint32_t* entries);

  const RendererStats& stats() const { return stats_; }
  // Lets the draw recording thread catch up: the ring thread is about to wait.
  void KickRecorder();
  // Submits what has been recorded so far, so the GPU works while the ring
  // thread waits (the frame rate cap holds a frame before presenting it).
  void Flush();
  // The textures uploaded most since the last call (TextureCache).
  std::string TakeTextureUploadReport();

  // Called for every register the command ring writes, so constant uploads
  // can be reused while their registers stay unchanged.
  // One byte per register says what a changed value invalidates: the chain of range checks this
  // replaced ran on every write, and the ring thread does 25,000 of them in a slow frame
  // (proto_stage_timer: 6.3 ms). Filled once by BuildRegisterKinds().
  enum RegisterKind : uint8_t {
    kRegIgnored = 0,  // written every draw or only read by resolves: no cache depends on it
    kRegState,        // the render state the draws cache (and its rolling hash below 0x2400)
    kRegStateNoHash,  // state, outside the hashed context block
    kRegIndex,        // vertex index range and offset: system constants only
    kRegVsFloat,
    kRegPsFloat,
    kRegFetch,
    kRegBoolLoop,
  };
  static constexpr uint32_t kRegisterKindCount = 0x5000;
  struct RegisterKinds {
    uint8_t kind[kRegisterKindCount];
    RegisterKinds() {
      for (uint32_t i = 0; i < kRegisterKindCount; ++i) {
        uint8_t k;
      if (i >= 0x4000) {
        k = i < 0x4400 ? kRegVsFloat
             : i < 0x4800 ? kRegPsFloat
             : i < 0x48C0 ? kRegFetch
             : i < 0x4928 ? kRegBoolLoop
                          : kRegStateNoHash;
      } else if (i >= 0x2100 && i <= 0x2102) {
        k = kRegIndex;
      } else if (i == 0x2103 || (i >= 0x21F9 && i <= 0x21FC) || (i >= 0x2318 && i <= 0x231F) ||
                 i == 0x2324 || i == 0x2325 || i == 0x2293 || (i >= 0x0578 && i <= 0x057F) ||
                 i == 0x01DC || i == 0x01DD || (i >= 0x0A2F && i <= 0x0A31) || i == 0x0C44 ||
                 i == 0x0C45 || i == 0x01C5) {
        k = kRegIgnored;
      } else {
        k = (i >= 0x2000 && i < 0x2400) ? kRegState : kRegStateNoHash;
      }
        kind[i] = k;
      }
    }
  };

  // Called for every register the command ring writes, so constant uploads
  // can be reused while their registers stay unchanged.
  void RegisterWritten(uint32_t index, uint32_t old_value, uint32_t new_value) {
    static const RegisterKinds kinds;
    const uint8_t kind = index < kRegisterKindCount ? kinds.kind[index] : uint8_t(kRegStateNoHash);
    switch (kind) {
      case kRegIgnored:
        return;
      case kRegState:
        ++state_generation_;
        ++state_change_counts_[index];
        // The render state is the context block; its hash is kept up to date one register at a
        // time (each register contributes Mix(index, value), and an XOR swaps old for new).
        state_hash_ ^= Mix(index, old_value) ^ Mix(index, new_value);
        return;
      case kRegStateNoHash:
        ++state_generation_;
        if (index < 0x4000) ++state_change_counts_[index];
        return;
      case kRegIndex:
        ++index_generation_;
        return;
      case kRegVsFloat:
        ++vs_float_generation_;
        return;
      case kRegPsFloat:
        ++ps_float_generation_;
        return;
      case kRegFetch: {
        ++fetch_generation_;
        const uint32_t slot_bit = 1u << ((index - 0x4800) / 6);
        texture_fetch_dirty_ |= slot_bit;
        vertex_fetch_dirty_ |= slot_bit;
        return;
      }
      case kRegBoolLoop:
        ++bool_loop_generation_;
        return;
    }
  }

  // A block of shader constant registers (0x4000-0x4927) written: the same
  // bookkeeping as RegisterWritten for each changed one, done once.
  void ConstantsWritten(bool vs_float, bool ps_float, uint32_t fetch_slots, bool bool_loop) {
    if (vs_float) ++vs_float_generation_;
    if (ps_float) ++ps_float_generation_;
    if (fetch_slots) {
      ++fetch_generation_;
      texture_fetch_dirty_ |= fetch_slots;
      vertex_fetch_dirty_ |= fetch_slots;
    }
    if (bool_loop) ++bool_loop_generation_;
  }

  static uint64_t Mix(uint32_t index, uint32_t value) {
    if (!value) {
      return 0;
    }
    uint64_t x = (uint64_t(index) << 32) | value;
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
  }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  RendererStats stats_;

 public:
  // Bumped by every register write that can change how a draw renders.
  uint64_t state_generation_ = 1;
  // Hash of the render state registers (all zero at the start: hash 0).
  uint64_t state_hash_ = 0;
  // Which registers changed the render state, for the report.
  uint32_t state_change_counts_[0x4000] = {};
  // Fetch constant slots (6 registers each) written since the renderer last
  // looked at them, for textures and for vertex buffers.
  uint32_t texture_fetch_dirty_ = ~0u;
  uint32_t vertex_fetch_dirty_ = ~0u;
  uint64_t index_generation_ = 1;
  uint64_t vs_float_generation_ = 1;
  uint64_t ps_float_generation_ = 1;
  uint64_t fetch_generation_ = 1;
  uint64_t bool_loop_generation_ = 1;
};

}  // namespace p2::native
