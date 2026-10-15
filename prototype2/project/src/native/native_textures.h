// SPDX-License-Identifier: GPL-3.0-or-later
//
// Textures for the native renderer (see native_renderer.h).
//
// Guest textures are untiled and converted on the CPU into a staging buffer and
// copied into an image by the setup command buffer, which runs before the draws
// of its submission. An image is identified by its fetch-constant layout and a
// hash of its guest data, so a texture whose memory changes becomes a different
// image, and an image never changes while a submission may be reading it: that
// is what lets every upload be hoisted out of the draw command buffer, which
// then never has to break a render pass to upload.
//
// Resolve destinations are images the renderer copies render targets into.
// They are registered by guest address, and a texture fetched from that address
// uses them instead of the guest memory, which the native renderer never writes.
//
// The formats, the view swizzles and the sampler parameters follow ReXGlue's
// VulkanTextureCache (itself from Xenia, BSD-3-Clause, see LICENSES/), which is
// what the translated shaders are written against.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/device.h>

namespace rex::memory {
class Memory;
}

namespace p2::native {

// Where the texture cache puts upload data and records the copies.
struct UploadTarget {
  // Returns a CPU pointer for `size` bytes aligned to `alignment`, and their
  // offset in `buffer`. Null if the arena is exhausted.
  std::function<uint8_t*(size_t size, size_t alignment, VkDeviceSize& offset_out)> allocate;
  VkBuffer buffer = VK_NULL_HANDLE;
  // Records into the command buffer that runs before the submission's draws.
  VkCommandBuffer setup_commands = VK_NULL_HANDLE;
  // The submission being recorded (increasing).
  uint64_t submission = 0;
};

// An image a resolve writes into. Owned by the texture cache.
struct ResolveDestination {
  uint32_t address = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  // Plain 2D view, for presenting.
  VkImageView view = VK_NULL_HANDLE;
  // Guest components (XYZW, as a texture fetch sees the resolved memory) in
  // terms of the host components (xenos swizzle encoding, 3 bits each).
  uint32_t host_swizzle = 0;
  // Views as textures (2D arrays), by host swizzle; bit 31 set for plain 2D.
  std::unordered_map<uint32_t, VkImageView> texture_views;
  uint64_t last_written_frame = 0;
  // Prototype 2: the resolve's RB_COPY_DEST_PITCH (texels), for GetCompositeView.
  uint32_t guest_pitch = 0;
  // Copies of the top-left corner at the sizes the game samples this address
  // as, where that differs from the image. Texture coordinates span the whole
  // image, so sampling the image itself scaled the content: the blur the game
  // makes from a 264x152 resolve into a 320-wide destination came back 1.2
  // times too small, a ghost of the skater up and to the left with a halo.
  // Refreshed by every resolve here (CopyToSized).
  struct Sized {
    uint32_t width = 0;
    uint32_t height = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkImageView> views;
    // Plain 2D view for the compute copy into it (null: copied by transfer).
    VkImageView storage_view = VK_NULL_HANDLE;
    // HANDOFF B:111: storage_view may also be a colour attachment (R32_SFLOAT copies: the
    // shadow atlas is written by a fragment pass).
    bool color_target = false;
    bool filled = false;
  };
  std::vector<std::unique_ptr<Sized>> sized;
};

struct TextureCacheStats {
  uint64_t uploads = 0;
  uint64_t created = 0;
  uint64_t streamed = 0;
  uint64_t upload_bytes = 0;
  uint64_t hash_bytes = 0;
  uint64_t unsupported = 0;
  uint64_t resolved_hits = 0;
  uint64_t live_textures = 0;
  // Lookups answered with the empty texture: still streaming in, or a format
  // or dimension not handled (logged once each).
  uint64_t pending = 0;
  // Textures whose content changed after a draw of this frame had used them.
  uint64_t replaced = 0;
  // Fetch answers: looked up in full (memo miss), memo dropped whole, and
  // dropped only for the address a resolve destination changed at.
  uint64_t uncached = 0;
  uint64_t memo_full_clears = 0;
  uint64_t memo_targeted = 0;
  uint64_t memo_dropped = 0;
};

class TextureCache {
 public:
  // Added to a depth resolve's address for the image holding what the resolve
  // leaves in guest memory, (depth24 << 8) | stencil per texel: fetches of
  // that address as anything but a depth format get it. Resolve addresses are
  // page aligned, so the tag never names a real one.
  static constexpr uint32_t kDepthWordsTag = 1;

  TextureCache();
  ~TextureCache();

  bool Initialize(const rex::ui::vulkan::VulkanDevice* device, rex::memory::Memory* memory);
  void Shutdown();

  // Frame boundary: textures not used for a while are destroyed once the GPU
  // is done with them. completed_frame is the last frame whose work finished.
  void BeginFrame(uint64_t frame, uint64_t completed_frame);

  // Records the null images' first transition. Called once, before any use.
  void InitializeNullImages(VkCommandBuffer commands);

  // The view for a translated shader's texture binding. Never null: a texture
  // that cannot be made gives a transparent black one.
  VkImageView GetView(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                      rex::graphics::xenos::FetchOpDimension dimension, bool is_signed,
                      UploadTarget& upload);

  // About the view the last GetView returned: the texture behind it (null for
  // a resolve or the empty texture), and the last frame through which that
  // answer stays the same while the fetch constant does.
  void* last_texture() const;
  // Diagnostic (HANDOFF B:98): the image and format behind a last_texture() answer.
  bool DebugTextureImage(void* texture, VkImage* image, VkFormat* format) const;
  uint64_t last_valid_until() const;
  // Marks a texture as used by this submission and frame, as GetView does:
  // what frees textures and rewrites them in place goes by it.
  void Touch(void* texture, uint64_t submission);

  VkSampler GetSampler(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                       const rex::graphics::SpirvShader::SamplerBinding& binding);

  // Finds or makes the image a resolve at this address writes into; recreated
  // if the size or the format changes. commands receives the layout
  // transition of a new image.
  ResolveDestination* GetResolveDestination(uint32_t address, uint32_t width, uint32_t height,
                                            VkFormat format, uint32_t host_swizzle,
                                            VkCommandBuffer commands);
  const ResolveDestination* FindResolveDestination(uint32_t address) const;
  // After a resolve wrote dest (transfer writes recorded in commands): brings
  // its sized copies up to date.
  void CopyToSized(ResolveDestination& dest, VkCommandBuffer commands);
  // HANDOFF B:105: a sized copy (written by compute) of another resolve destination that a
  // resolve of width x height to address falls inside of, whole rows of 32-line tiles below its
  // top: the game resolves its four 1024x1024 shadow cascades one after another in memory and
  // samples them as one 1024x4096 texture, of which CopyToSized fills only the top quarter.
  // *row receives the first line of the resolve in it.
  ResolveDestination::Sized* FindAtlas(uint32_t address, uint32_t width, uint32_t height,
                                       VkFormat format, uint32_t* row);
  // Copies a rectangle between two views on the GPU without vkCmdCopyImage,
  // barriers included (the renderer's compute copy); false if it cannot.
  using ComputeCopy = std::function<bool(VkCommandBuffer commands, VkImageView source,
                                         VkImageView dest, int32_t source_x, int32_t source_y,
                                         int32_t dest_x, int32_t dest_y, uint32_t width,
                                         uint32_t height, VkFormat attachment_format)>;
  void SetComputeCopy(ComputeCopy copy);
  // Whether images of this format can be written by the compute copy.
  bool StorageCapable(VkFormat format);
  // HANDOFF B:112: whether resolve destinations of this format can be rendered to (they then get
  // COLOR_ATTACHMENT usage and are filled by fragment copies).
  bool ColorAttachmentCapable(VkFormat format);

  // Whether a depth resolve to this address has been sampled as anything but
  // a depth format, so it needs the guest memory words (kDepthWordsTag).
  bool WantsDepthWords(uint32_t address) const;
  // The textures uploaded most since the last call (address, format, size,
  // uploads, content fingerprint), for the periodic report; empty if none.
  std::string TakeUploadReport();
  // A plain 2D view of a resolve destination with a host swizzle, for the
  // swap (which samples a non-array image).
  VkImageView GetResolveDestinationView2D(uint32_t address, uint32_t guest_swizzle,
                                          uint32_t* width_out, uint32_t* height_out);

  // Untiling runs on a worker: call before submitting the command buffers
  // that copy from the staging data.
  void WaitForUploads();

  const TextureCacheStats& stats() const { return stats_; }
  // Changes whenever image views are destroyed, so descriptor sets cached by
  // view handle must be dropped (a new view may reuse a handle).
  uint64_t views_epoch() const;
  // An id for a view that is never given to another: a handle can be reused
  // once its view is destroyed, an id cannot, so descriptor sets cached by id
  // stay right without being dropped whenever any view is destroyed.
  uint64_t ViewId(VkImageView view);
  // Whether the view with this id still exists.
  bool ViewAlive(VkImageView view, uint64_t id) const;
  // Changes when what GetView answers for some fetch may have changed other
  // than by its own checks (a resolve destination made, a texture replaced).
  uint64_t answers_epoch() const;

 private:
  VkImageView GetViewUncached(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                              rex::graphics::xenos::FetchOpDimension dimension, bool is_signed,
                              UploadTarget& upload, VkImageView null_view);
  // Prototype 2: a texture that spans several resolve destinations (the game resolves each of
  // its 4 shadow cascades into one quadrant of a 1536x1536 tiled texture, each at the tiled
  // address of its corner) gets one image they are all copied into (null: not such a texture).
  VkImageView GetCompositeView(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                               uint32_t width, uint32_t height, uint32_t base_address,
                               UploadTarget& upload);
  struct Texture;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  TextureCacheStats stats_;
};

}  // namespace p2::native
