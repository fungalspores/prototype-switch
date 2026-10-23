// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native renderer for the Switch port, stage A: the graphics system.
//
// Adapted from nfsmw-nx (StevensND/nfsmw-nx, app/src/nfsmw_nativo_sistema.cpp,
// GPL-3.0): the MMIO registers, the command ring consumer, the answers the game
// waits for and the vblank thread follow its design and, in places, its code.
// Its Need for Speed specific parts (the offline shader library, the FlushState
// markers, pairing draws with Direct3D calls) are not carried over.
//
// What this stage does and does not do
//   - The game runs without the Xenos emulation: its Direct3D fills the ring,
//     and this system consumes it, keeps the GPU register file, performs the
//     memory writes, interrupts and waits the game depends on, and fakes
//     occlusion query results like the emulation does.
//   - Draws and copies are only counted. Each swap presents a test colour whose
//     green channel moves with the swap count, so a running game can be told
//     from a stalled one. Render targets, resolves and draws are the next
//     stages.

#include <rex/ui/gpu_wait_diagnostics.h>
#include "p2_native_system.h"
#include "../guest_pacing_stats.h"
#include "native_renderer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

// project8-sw defines this in its SDK patches; ours has no equivalent, so it lives here.
REXCVAR_DECLARE(uint32_t, p2_host_core_mask);  // SDK xthread.cpp
REXCVAR_DEFINE_INT32(switch_cp_core, -1, "Prototype2",
                     "Core (0-2) to pin the native ring thread to; -1 leaves it unpinned");

// The game starts its next frame as soon as the swap-complete interrupt of the
// previous one arrives (its frame limiter waits for a counter that interrupt
// bumps). With the native renderer that interrupt came back immediately, so
// frames were paced only by how long they took: 16.7, 33 or 50 ms in turn,
// which feels choppy even at a good average. Holding the interrupt to vblank
// boundaries, at least this many vblanks after the previous one, gives the
// even cadence a console gets from vsync; 2 is a steady 30.
REXCVAR_DEFINE_BOOL(p2_pace_before_present, false, "Prototype2",
                    "Hold a frame for the frame rate cap before presenting it rather than "
                    "after, so frames go on screen evenly spaced");
REXCVAR_DEFINE_UINT32(p2_min_frame_vblanks, 1, "Prototype2",
                      "Native renderer: minimum 60 Hz vblanks between frames (1 = the game's "
                      "own pacing, 2 = an even 30 fps)");

// run19: the guest's mfmsr/mtmsrd "interrupts off" regions around lwarx/stwcx were made lock-free in the
// generated code (tools/prepare_switch_codegen.py, run18 +2x fps). true brings the SDK's process-wide
// mutex back for A/B (level load crashed 3x in run19 with an invalid guest call).
REXCVAR_DEFINE_UINT32(p2_msr_lock_mode, 2, "Prototype2",
                      "Guest interrupt-disabled atomic regions: 0 = no lock (fastest; crashed level loads in "
                      "run19/21), 1 = one lock per address stripe, 2 = the SDK's single global mutex (slowest)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(p2_renderer, "xenos", "Prototype2",
                      "xenos = the SDK's emulated Xbox 360 GPU; native = the native renderer "
                      "(in development: stage A presents a test colour)")
    .allowed({"xenos", "native"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__SWITCH__)
extern "C" {
#include <switch/kernel/svc.h>
}
extern "C" void RexGmSetReadOnlyMapped(bool enabled);
// The frame counter the console FPS overlay and the profiler read (the
// emulated path counts its swaps there too).
extern "C" void RexSwitchPerfCount(unsigned id);
#endif

namespace p2::native {
namespace {

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kRegisterCount = 0x5003;  // RegisterFile::kRegisterCount

// Registers with a fixed value or an effect, as in the SDK's graphics_system.cpp.
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;

constexpr uint32_t kFakeOcclusionSamples = 1000;  // query_occlusion_fake_sample_count
constexpr uint16_t kMaxExtent = 2048 >> 3;        // xenos::kTexture2DCubeMaxWidthHeight >> 3
constexpr int kMaxIndirectDepth = 4;
// Without a GPU some waits can never be satisfied; they are cut short so the
// ring cannot hang.
constexpr auto kWaitRegMemMax = std::chrono::milliseconds(200);
constexpr uint32_t kOutputWidth = 1280;
constexpr uint32_t kOutputHeight = 720;

std::atomic<uint64_t> g_swaps_presented{0};

// Big-endian words of a command buffer. In the ring the position wraps around
// (its size is a power of two); in an indirect buffer it does not.
struct Reader {
  const uint8_t* base = nullptr;
  uint32_t mask = 0;  // words - 1 in the ring; 0 in a linear buffer
  uint32_t pos = 0;
  uint32_t end = 0;

  uint32_t Pending() const { return mask ? ((end - pos) & mask) : (end - pos); }
  uint32_t Peek() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    const uint32_t value = Peek();
    Advance(1);
    return value;
  }
  void Advance(uint32_t words) { pos = mask ? ((pos + words) & mask) : (pos + words); }
};

bool Compare(uint32_t info, uint32_t value, uint32_t reference) {
  switch (info & 0x7) {
    case 0x0:
      return false;
    case 0x1:
      return value < reference;
    case 0x2:
      return value <= reference;
    case 0x3:
      return value == reference;
    case 0x4:
      return value != reference;
    case 0x5:
      return value >= reference;
    case 0x6:
      return value > reference;
    default:
      return true;
  }
}

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeGraphicsSystem()
      : register_file_(std::make_unique<rex::graphics::RegisterFile>()),
        registers_(register_file_->values) {
#if defined(__SWITCH__)
    // Nothing watches guest pages for writes without the SDK's shared memory,
    // so the pages the game makes read-only can stay mapped. Otherwise every
    // read of them is an emulated fault: 200,000 a second at the menu, which
    // held it at 8 fps. The emulated path gets this from SharedMemory.
    RexGmSetReadOnlyMapped(true);
#endif
    std::memset(registers_, 0, sizeof(register_file_->values));
    // The linear 256-entry gamma ramp the SDK starts with.
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t value = i * 0x3FF / 0xFF;
      gamma_ramp_[i] = value | (value << 10) | (value << 20);
    }
  }
  ~NativeGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) {
      return X_STATUS_SUCCESS;
    }
    app_context_ = app_context;
    if (!provider_) {
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
      if (!provider_) {
        REXLOG_ERROR("[native] Could not create the Vulkan device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[native] Could not create the presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[native] Native graphics system (stage A): SDK presenter, no Xenos emulation");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!renderer_ready_ && provider_) {
      renderer_ready_ = renderer_.Initialize(provider_.get(), memory_, register_file_.get());
      packet_trace_enabled_ = renderer_ready_ && renderer_.water_dump_enabled();
      if (!renderer_ready_) {
        REXLOG_ERROR("[native] The renderer did not start; frames will be a test colour");
      }
    }
    if (!mmio_registered_) {
      mmio_registered_ = memory_->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, this,
                                                        &ReadMmioThunk, &WriteMmioThunk);
      if (!mmio_registered_) {
        REXLOG_ERROR("[native] Could not register the GPU MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (running_.exchange(true)) {
      return X_STATUS_SUCCESS;
    }
    vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { return VblankLoop(); }));
    vblank_thread_->set_name("GPU VSync native");
    vblank_thread_->Create();
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { return RingLoop(); }));
    ring_thread_->set_name("GPU ring native");
    ring_thread_->Create();
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[native] Interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    // As CommandProcessor::InitializeRingBuffer: (1 << (size_log2 + 3)) bytes.
    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    ring_generation_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[native] Ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    read_pointer_writeback_.store(ptr, std::memory_order_release);
  }

  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override {
    (void)blocking;
    if (renderer_ready_) {
      renderer_.InitializeDriverCache(cache_root, title_id);
    }
  }

  void Shutdown() override {
    if (running_.exchange(false)) {
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_all();
      if (ring_thread_) {
        ring_thread_->Wait(0, 0, 0, nullptr);
        ring_thread_.reset();
      }
      if (vblank_thread_) {
        vblank_thread_->Wait(0, 0, 0, nullptr);
        vblank_thread_.reset();
      }
    }
    if (renderer_ready_) {
      renderer_.Shutdown();
      renderer_ready_ = false;
    }
    DestroyVulkan();
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  using OutputContext =
      rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext;

  // --- MMIO registers ----------------------------------------------------------

  static uint32_t ReadMmioThunk(void* ppc_context, void* context, uint32_t address) {
    (void)ppc_context;
    return static_cast<NativeGraphicsSystem*>(context)->ReadMmio(address);
  }

  static void WriteMmioThunk(void* ppc_context, void* context, uint32_t address, uint32_t value) {
    (void)ppc_context;
    static_cast<NativeGraphicsSystem*>(context)->WriteMmio(address, value);
  }

  uint32_t ReadMmio(uint32_t address) {
    const uint32_t index = (address & 0xFFFF) / 4;
    switch (index) {
      case kRegRbEdramTiming:
        return 0x08100748;
      case kRegRbBcControl:
        return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus:
        return 1;  // in vblank
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        const uint32_t width = std::min(uint32_t(mode.display_width), uint32_t(0x0FFF));
        const uint32_t height = std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
        return (width << 16) | height;
      }
      default:
        break;
    }
    return Register(index);
  }

  void WriteMmio(uint32_t address, uint32_t value) {
    const uint32_t index = (address & 0xFFFF) / 4;
    if (index == kRegCpRbWptr) {
      write_pointer_.store(value, std::memory_order_release);
      // Taking the lock for an instant keeps the wakeup from being lost when
      // the ring thread is between checking and going to sleep.
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_one();
    }
    // No side effects through MMIO (graphics_system.cpp).
    if (index < kRegisterCount && registers_[index] != value) {
      const uint32_t old_value = registers_[index];
      registers_[index] = value;
      renderer_.RegisterWritten(index, old_value, value);
    }
  }

  uint32_t Register(uint32_t index) const {
    return index < kRegisterCount ? registers_[index] : 0;
  }

  // A register write from the ring, with the side effects it has when the GPU
  // processes it (CommandProcessor::WriteRegister). The Direct3D writes a
  // SCRATCH_REG and then waits with WAIT_REG_MEM until it appears at
  // SCRATCH_ADDR, so those are copied to memory; COHER_STATUS_HOST stays
  // pending until polled.
  // Consecutive registers from the stream. Blocks of shader constants, most of
  // the register traffic (every draw sets its matrices), skip the per-register
  // call and switch: a changed one only bumps a counter in the renderer.
  void WriteRegisters(uint32_t index, uint32_t count, Reader& data) {
    if (index >= 0x4000 && index + count <= 0x4928) {
      bool vs_float = false, ps_float = false, bool_loop = false;
      uint32_t fetch_slots = 0;
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t r = index + i;
        const uint32_t v = data.Read();
        if (registers_[r] == v) continue;
        registers_[r] = v;
        if (r < 0x4400) {
          vs_float = true;
        } else if (r < 0x4800) {
          ps_float = true;
        } else if (r < 0x48C0) {
          fetch_slots |= 1u << ((r - 0x4800) / 6);
        } else {
          bool_loop = true;
        }
      }
      renderer_.ConstantsWritten(vs_float, ps_float, fetch_slots, bool_loop);
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      WriteRegister(index + i, data.Read());
    }
  }

  // The fast path for the registers the draws write (0x1928 and up): no special case lives there,
  // so a changed value only needs the store and the renderer's bookkeeping. Measured in Prototype
  // with proto_stage_timer: 25,000 register writes per slow frame, 6.3 ms of the ring thread.
  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= 0x1928 && index < kRegisterCount) [[likely]] {
      const uint32_t old_value = registers_[index];
      if (old_value == value) return;
      registers_[index] = value;
      renderer_.RegisterWritten(index, old_value, value);
      return;
    }
    WriteRegisterSlow(index, value);
  }

  void WriteRegisterSlow(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) {
      return;
    }
    if (index == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
      value |= UINT32_C(0x80000000);
    }
    // Caches in the renderer depend on register values, and the game rewrites
    // most state with the same value before every draw: only a change counts.
    if (registers_[index] != value) {
      const uint32_t old_value = registers_[index];
      registers_[index] = value;
      renderer_.RegisterWritten(index, old_value, value);
    }
    switch (index) {
      case rex::graphics::XE_GPU_REG_DC_LUT_RW_INDEX:
        gamma_ramp_component_ = 0;
        return;
      case rex::graphics::XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // CommandProcessor::WriteRegister: red, green, blue in sequence, the
        // write enable mask in blue, green, red order.
        uint32_t entry = registers_[rex::graphics::XE_GPU_REG_DC_LUT_RW_INDEX] & 0xFF;
        if (registers_[rex::graphics::XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
            (1u << (2 - gamma_ramp_component_))) {
          uint32_t color = (value & 0xFFFF) >> 6;
          uint32_t shift = 20 - 10 * gamma_ramp_component_;  // red is bits 20:29
          gamma_ramp_[entry] = (gamma_ramp_[entry] & ~(0x3FFu << shift)) | (color << shift);
        }
        if (++gamma_ramp_component_ >= 3) {
          gamma_ramp_component_ = 0;
          registers_[rex::graphics::XE_GPU_REG_DC_LUT_RW_INDEX] =
              (registers_[rex::graphics::XE_GPU_REG_DC_LUT_RW_INDEX] & ~0xFFu) |
              ((entry + 1) & 0xFF);
        }
        renderer_.SetGammaRamp256(gamma_ramp_);
        return;
      }
      case rex::graphics::XE_GPU_REG_DC_LUT_30_COLOR: {
        uint32_t entry = registers_[rex::graphics::XE_GPU_REG_DC_LUT_RW_INDEX] & 0xFF;
        uint32_t enable = registers_[rex::graphics::XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        uint32_t mask = ((enable & 0b001) ? 0x3FFu : 0) | ((enable & 0b010) ? 0x3FFu << 10 : 0) |
                        ((enable & 0b100) ? 0x3FFu << 20 : 0);
        gamma_ramp_[entry] = (gamma_ramp_[entry] & ~mask) | (value & mask);
        renderer_.SetGammaRamp256(gamma_ramp_);
        return;
      }
      default:
        break;
    }
    if (index >= rex::graphics::XE_GPU_REG_SCRATCH_REG0 &&
        index <= rex::graphics::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t n = index - rex::graphics::XE_GPU_REG_SCRATCH_REG0;
      if ((UINT32_C(1) << n) & registers_[rex::graphics::XE_GPU_REG_SCRATCH_UMSK]) {
        const uint32_t address = registers_[rex::graphics::XE_GPU_REG_SCRATCH_ADDR] + n * 4;
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address), value);
      }
    }
  }

  void WriteMemory(uint32_t address, uint32_t value) {
    const uint32_t swapped = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~uint32_t(0x3)), &swapped, sizeof(swapped));
  }

  uint32_t ReadMemory(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, memory_->TranslatePhysical(address & ~uint32_t(0x3)), sizeof(value));
    return xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
  }

  // --- Threads -------------------------------------------------------------------

  void Interrupt(uint32_t source, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) {
      return;
    }
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) {
      return;
    }
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    // Not raised above the game's threads: the vblank interrupt runs the
    // game's handler, which takes a guest spinlock, and at a higher priority
    // this thread spun on it while the holder could not run on its core. The
    // vblanks stopped and the game never started another frame (black
    // screen).
    auto next = Clock::now() + interval;
    while (running_.load(std::memory_order_acquire)) {
      // To the deadline, not in 1 ms steps: those made every vblank up to a
      // millisecond or more late.
      // Our SDK has no SleepUntil: sleep the remaining time to the deadline.
      if (const auto remaining = next - Clock::now(); remaining > Clock::duration::zero()) {
        rex::thread::Sleep(std::chrono::duration_cast<std::chrono::microseconds>(remaining));
      }
      const auto now = Clock::now();
      // After a long pause (loading, a debugger) hundreds of vblanks are not
      // replayed.
      if (now - next > std::chrono::milliseconds(250)) {
        next = now;
      }
      while (now >= next) {
        vblanks_.fetch_add(1, std::memory_order_release);
        counter_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
    }
    return 0;
  }

  int RingLoop() {
    if(P2GpuWaitRole)P2GpuWaitRole(p2::gpuwait::Ring);
#if defined(__SWITCH__)
    // The same core the emulated path's command processor is pinned to: on a
    // core of its own it is not migrated around the game's threads.
    {
      const int32_t core = REXCVAR_GET(switch_cp_core);
      const uint32_t host_mask = REXCVAR_GET(p2_host_core_mask) & 0xFu;
      if (core >= 0 && core <= 2) {
        const Result result = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
        REXLOG_INFO("[native] Ring thread on core {}: result {:08X}", core, uint32_t(result));
      } else if (host_mask) {
        const Result result =
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, __builtin_ctz(host_mask), host_mask);
        REXLOG_INFO("[native] Ring thread on core mask {:X}: result {:08X}", host_mask,
                    uint32_t(result));
      }
    }
#endif
    uint32_t read = 0;
    uint32_t generation = 0;
    auto last_report = Clock::now();
    while (running_.load(std::memory_order_acquire)) {
      {
        if (renderer_ready_) {
          renderer_.PollOcclusionQueries();renderer_.KickRecorder();
        }
        const auto idle_start = Clock::now();
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_cv_.wait_for(lock, std::chrono::milliseconds(4), [&] {
          return !running_.load(std::memory_order_acquire) ||
                 write_pointer_.load(std::memory_order_acquire) != read ||
                 ring_generation_.load(std::memory_order_acquire) != generation;
        });
        ring_idle_ns_ += uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - idle_start)
                .count());
      }
      if (!running_.load(std::memory_order_acquire)) {
        break;
      }
      if (Clock::now() - last_report > std::chrono::seconds(10)) {
        last_report = Clock::now();
        Report();
      }
      const uint32_t gen = ring_generation_.load(std::memory_order_acquire);
      if (gen != generation) {
        generation = gen;
        read = 0;  // InitializeRingBuffer leaves the read pointer at zero
      }
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      if (!base || !words) {
        continue;
      }
      const uint32_t written = write_pointer_.load(std::memory_order_acquire) & (words - 1);
      if (written == read) {
        // No producer commands arrived during the existing 4 ms idle wait.
        // CPU GetData may be waiting without issuing WAIT_REG_MEM; flush once.
        if(renderer_ready_)renderer_.PollOcclusionQueries(true);
        continue;
      }
      Reader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.mask = words - 1;
      reader.pos = read;
      reader.end = written;
      while (reader.Pending()) {
        if (renderer_ready_ && (packets_ & 63u)==0) renderer_.PollOcclusionQueries();
        if (!Packet(reader, 0)) {
          // A partial packet: the rest arrives with the next CP_RB_WPTR.
          break;
        }
      }
      read = reader.pos;
      const uint32_t writeback = read_pointer_writeback_.load(std::memory_order_acquire);
      if (writeback) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), read);
      }
    }
    return 0;
  }

  void Report() {
    if(P2GpuWaitReport)P2GpuWaitReport();
    REXLOG_INFO("[native] last 10 s: {} packets, {} draws, {} copies, {} swaps, {} waits cut short",
                packets_ - reported_packets_, draws_ - reported_draws_, copies_ - reported_copies_,
                swaps_ - reported_swaps_, waits_cut_short_);
    reported_packets_ = packets_;
    reported_draws_ = draws_;
    reported_copies_ = copies_;
    reported_swaps_before_ = reported_swaps_;
    reported_swaps_ = swaps_;
    if (renderer_ready_) {
      const RendererStats& s = renderer_.stats();
      REXLOG_INFO(
          "[native] renderer: {} drawn, {} skipped, {} resolves, {} passes, {} submissions, "
          "{} shaders, {} pipelines, {} textures ({} MB), {} MB vertices",
          s.draws - reported_render_.draws, s.draws_skipped - reported_render_.draws_skipped,
          s.resolves - reported_render_.resolves, s.render_passes - reported_render_.render_passes,
          s.submissions - reported_render_.submissions, s.shaders_translated,
          s.pipelines_created, s.textures_uploaded - reported_render_.textures_uploaded,
          (s.texture_bytes_uploaded - reported_render_.texture_bytes_uploaded) >> 20,
          (s.vertex_bytes_copied - reported_render_.vertex_bytes_copied) >> 20);
      REXLOG_INFO(
          "[native] textures: {} made now, {} streamed in, {} replaced after use, {} lookups "
          "answered empty (streaming or not handled)",
          s.textures_created - reported_render_.textures_created,
          s.textures_streamed - reported_render_.textures_streamed,
          s.textures_replaced - reported_render_.textures_replaced,
          s.textures_pending - reported_render_.textures_pending);
      {
        std::string uploads = renderer_.TakeTextureUploadReport();
        if (!uploads.empty()) {
          REXLOG_INFO("[native] most uploaded textures: {}", uploads);
        }
      }
      uint64_t frames = std::max<uint64_t>(swaps_ - reported_swaps_before_, 1);
      uint64_t gpu_count =
          std::max<uint64_t>(s.gpu_submissions - reported_render_.gpu_submissions, 1);
      REXLOG_INFO(
          "[native] per frame: renderer CPU {:.1f} ms, GPU {:.1f} ms over {:.1f} submissions "
          "(GPU time as the driver reports it)",
          double(s.cpu_ns - reported_render_.cpu_ns) / 1e6 / double(frames),
          double(s.gpu_ns - reported_render_.gpu_ns) / 1e6 / double(frames),
          double(s.gpu_submissions - reported_render_.gpu_submissions) / double(frames));
      REXLOG_INFO(
          "[native] binding cache per frame: hit {:.0f}, new {:.0f}, identity {:.0f}, "
          "set_generation {:.0f}, answers_epoch {:.0f}, dead_views {:.0f}, expired {:.0f}",
          double(s.texture_binding_cache[0] - reported_render_.texture_binding_cache[0]) / double(frames),
          double(s.texture_binding_cache[1] - reported_render_.texture_binding_cache[1]) / double(frames),
          double(s.texture_binding_cache[2] - reported_render_.texture_binding_cache[2]) / double(frames),
          double(s.texture_binding_cache[3] - reported_render_.texture_binding_cache[3]) / double(frames),
          double(s.texture_binding_cache[4] - reported_render_.texture_binding_cache[4]) / double(frames),
          double(s.texture_binding_cache[5] - reported_render_.texture_binding_cache[5]) / double(frames),
          double(s.texture_binding_cache[6] - reported_render_.texture_binding_cache[6]) / double(frames));
      REXLOG_INFO("[native] binding refresh per frame: attempted {:.0f}, sets kept {:.0f}",
          double(s.texture_binding_refresh_attempts - reported_render_.texture_binding_refresh_attempts) / double(frames),
          double(s.texture_binding_refresh_kept - reported_render_.texture_binding_refresh_kept) / double(frames));
      (void)gpu_count;
      REXLOG_INFO("[native] per frame: {:.0f} draws, {:.0f} state rebuilds, {:.0f} texture "
                  "binding rebuilds",
                  double(s.draws - reported_render_.draws) / double(frames),
                  double(s.setups_built - reported_render_.setups_built) / double(frames),
                  double(s.texture_bindings_built - reported_render_.texture_bindings_built) /
                      double(frames));
      REXLOG_INFO("[native] per frame: {:.1f} waits for the game, {:.1f} ms waiting",
                  double(waits_ - reported_waits_) / double(frames),
                  double(wait_ns_ - reported_wait_ns_) / 1e6 / double(frames));
      reported_waits_ = waits_;
    slow_frames_logged_ = 0;
      REXLOG_INFO(
          "[native] slow frames: {} over 50 ms, {} over 100 ms; in them {} pipelines, "
          "{} shaders, {} texture uploads, {} render targets",
          s.frames_over_50ms - reported_render_.frames_over_50ms,
          s.frames_over_100ms - reported_render_.frames_over_100ms,
          s.slow_frame_pipelines - reported_render_.slow_frame_pipelines,
          s.slow_frame_shaders - reported_render_.slow_frame_shaders,
          s.slow_frame_uploads - reported_render_.slow_frame_uploads,
          s.slow_frame_render_targets - reported_render_.slow_frame_render_targets);
      REXLOG_INFO(
          "[native] ring compile time: {:.1f} ms translating {} shaders, {:.1f} ms creating {} "
          "pipelines; from the prewarm: {} shaders, {} pipelines",
          double(s.translate_ns - reported_render_.translate_ns) / 1e6,
          s.shaders_translated - reported_render_.shaders_translated,
          double(s.pipeline_ns - reported_render_.pipeline_ns) / 1e6,
          s.pipelines_created - reported_render_.pipelines_created,
          s.prewarm_shaders - reported_render_.prewarm_shaders,
          s.prewarm_pipelines - reported_render_.prewarm_pipelines);
      reported_wait_ns_ = wait_ns_;
      {
        // The registers that most often changed the render state.
        std::vector<std::pair<uint32_t, uint32_t>> top;
        for (uint32_t i = 0; i < 0x4000; ++i) {
          if (renderer_.state_change_counts_[i]) {
            top.emplace_back(renderer_.state_change_counts_[i], i);
            renderer_.state_change_counts_[i] = 0;
          }
        }
        std::sort(top.rbegin(), top.rend());
        std::string list;
        for (size_t i = 0; i < std::min<size_t>(top.size(), 12); ++i) {
          list += fmt::format(" {:04X}:{}", top[i].second, top[i].first);
        }
        REXLOG_INFO("[native] state changes by register (10 s):{}", list);
      }
      std::string reasons;
      for (uint32_t i = 0; i < 16; ++i) {
        uint64_t n = s.skipped_by_reason[i] - reported_render_.skipped_by_reason[i];
        if (n) reasons += fmt::format(" {}:{}", i, n);
      }
      REXLOG_INFO("[native] skipped by reason:{}", reasons);
      reported_render_ = s;
    }
  }
  RendererStats reported_render_;
  uint64_t reported_swaps_before_ = 0;

  // --- PM4 packets -----------------------------------------------------------------

  // Consumes a whole packet, or leaves the position untouched if it is not
  // complete yet.
  bool Packet(Reader& reader, int depth) {
    const uint32_t packet = reader.Peek();
    uint32_t count = 0;
    switch (packet >> 30) {
      case 0:
        count = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0;
        break;
      case 1:
        count = 2;
        break;
      case 2:
        count = 0;
        break;
      default:
        count = ((packet >> 16) & 0x3FFF) + 1;
        break;
    }
    if (reader.Pending() < count + 1) {
      return false;
    }
    reader.Advance(1);
    ++packets_;
    Reader data = reader;
    if (packet_trace_enabled_) {
      // HANDOFF B:100: the last packets before the water simulation draw.
      PacketTraceEntry& e = packet_trace_[packet_trace_next_++ % packet_trace_.size()];
      e.header = packet;
      e.count = count;
      e.bin_select = uint32_t(bin_select_);
      e.bin_mask = uint32_t(bin_mask_);
      Reader peek = data;
      for (uint32_t i = 0; i < 4; ++i) e.words[i] = i < count ? peek.Read() : 0;
    }
    reader.Advance(count);
    if (!packet) {
      return true;
    }
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool one_register = (packet >> 15) & 0x1;
        if (one_register) {
          for (uint32_t i = 0; i < count; ++i) {
            WriteRegister(index, data.Read());
          }
        } else {
          WriteRegisters(index, count, data);
        }
        break;
      }
      case 1: {
        const uint32_t value1 = data.Read();
        const uint32_t value2 = data.Read();
        WriteRegister(packet & 0x7FF, value1);
        WriteRegister((packet >> 11) & 0x7FF, value2);
        break;
      }
      case 2:
        break;
      default:
        Type3Packet(data, packet, count, depth);
        break;
    }
    return true;
  }

  void Type3Packet(Reader& data, uint32_t packet, uint32_t words, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    if ((packet & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      return;  // predicate fails; predicated swaps are always dropped
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (words < 1) break;
        const uint32_t cpus = data.Read();
        if (swap_interrupt_pending_) {
          // The swap-complete interrupt: right after the present, which the
          // frame rate cap already held (PM4_XE_SWAP), or held here if the
          // cap is not applied before presenting.
          swap_interrupt_pending_ = false;
          if (!REXCVAR_GET(p2_pace_before_present)) PaceFrame();
        }
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) {
            Interrupt(1, cpu);
          }
        }
        break;
      }
      case xenos::PM4_XE_SWAP:
        // VdSwap writes "SWAP", the physical address of the frontbuffer and
        // its size.
        if (words >= 4) {
          data.Read();
          swap_frontbuffer_ = data.Read();
          swap_width_ = data.Read();
          swap_height_ = data.Read();
        }
        if (REXCVAR_GET(p2_pace_before_present)) PaceFrame();
        Present();
        swap_interrupt_pending_ = true;
        ReportSlowFrame();
        break;
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (words < 2) break;
        const uint32_t address = data.Read() & 0x1FFFFFFF;
        const uint32_t length = data.Read() & 0xFFFFF;
        if (depth < kMaxIndirectDepth) {
          IndirectBuffer(address, length, depth + 1);
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        p2::gpuwait::Scope gpu_wait_total(p2::gpuwait::WaitMem);
        if (words < 5) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const auto wait_start = Clock::now();
        const auto limit = wait_start + kWaitRegMemMax;
        bool waited = false;
        uint64_t gpu_wait_blocked=0;uint32_t gpu_wait_kind=p2::gpuwait::WaitMemBlocked;
        for (uint32_t attempt = 0;; ++attempt) {
          if (!(info & 0x10) && poll == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST &&
              (Register(poll) & UINT32_C(0x80000000))) {
            registers_[poll] = 0;  // MakeCoherent: no shared memory to synchronise
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
          if (Compare(info, value & mask, reference)) {
            break;
          }
          if (Clock::now() >= limit || !running_.load(std::memory_order_acquire)) {
            if (waits_cut_short_++ == 0) {
              REXLOG_WARN("[native] WAIT_REG_MEM not met ({} {:08X} ref {:08X} mask {:08X})",
                          (info & 0x10) ? "memory" : "register", poll, reference, mask);
            }
            break;
          }
          if(!waited && P2GpuWaitEnabled && P2GpuWaitEnabled()){
            if((info&0x10) && renderer_ready_ && renderer_.IsOcclusionResultAddress(poll & ~uint32_t(3)))gpu_wait_kind=p2::gpuwait::WaitMemOcclusion;
            gpu_wait_blocked=P2GpuWaitBegin(gpu_wait_kind);
          }
          if(renderer_ready_ && (attempt & 63u)==0)renderer_.PollOcclusionQueries(true);
          if (!waited && renderer_ready_) {
            // Idle time: let the draw recording thread empty its queue.
            renderer_.KickRecorder();
          }
          waited = true;
          // The game usually releases what the ring waits for within tens of
          // microseconds; yielding first avoids oversleeping each wait.
          if (attempt < 64) {
            std::this_thread::yield();
          } else {
            rex::thread::Sleep(std::chrono::microseconds(50));
          }
        }
        if(gpu_wait_blocked)P2GpuWaitEnd(gpu_wait_kind,gpu_wait_blocked);
        if (waited) {
          ++waits_;
          wait_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   Clock::now() - wait_start)
                                   .count());
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (words < 3) break;
        const uint32_t info = data.Read();
        const uint32_t and_value = data.Read();
        const uint32_t or_value = data.Read();
        uint32_t value = Register(info & 0x1FFF);
        value &= ((info >> 31) & 0x1) ? Register(and_value & 0x1FFF) : and_value;
        value |= ((info >> 30) & 0x1) ? Register(or_value & 0x1FFF) : or_value;
        WriteRegister(info & 0x1FFF, value);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        if (words < 2) break;
        const uint32_t reg = data.Read();
        const uint32_t address = data.Read();
        WriteMemory(address, Register(reg));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (words < 1) break;
        uint32_t address = data.Read();
        for (uint32_t i = 1; i < words; ++i) {
          WriteMemory(address, data.Read());
          address += 4;
        }
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (words < 6) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const uint32_t destination = data.Read();
        const uint32_t value_to_write = data.Read();
        const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
        if (Compare(info, value & mask, reference)) {
          if (info & 0x100) {
            WriteMemory(destination, value_to_write);
          } else {
            WriteRegister(destination, value_to_write);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (words < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        if (words < 3) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        const uint32_t value = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        WriteMemory(address, ((initiator >> 31) & 0x1) ? counter_.load(std::memory_order_relaxed)
                                                       : value);
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        if (words < 2) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        // The screen extent of the previous draw: the maximum is faked.
        const uint16_t extent[] = {0, kMaxExtent, 0, kMaxExtent, 0, 1};
        uint8_t* destination = memory_->TranslatePhysical(address & ~uint32_t(0x3));
        for (size_t i = 0; i < std::size(extent); ++i) {
          rex::memory::store_and_swap<uint16_t>(destination + i * 2, extent[i]);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        if (words < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        const uint32_t address = Register(rex::graphics::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR);
        if (address) {
          NativeOcclusionResult(address);
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (words < 1) break;
        const uint32_t type_index = data.Read();
        const uint32_t base = ConstantBase(type_index);
        if (base != UINT32_MAX) {
          const uint32_t index = base + (type_index & 0x7FF);
          WriteRegisters(index, words - 1, data);
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        // Same format: the first value is the register index, the rest are
        // written consecutively.
        if (words < 1) break;
        const uint32_t index = data.Read() & 0xFFFF;
        WriteRegisters(index, words - 1, data);
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        // Registers loaded from guest memory instead of from the ring.
        if (words < 3) break;
        const uint32_t address = data.Read() & 0x3FFFFFFF;
        const uint32_t type_index = data.Read();
        const uint32_t amount = data.Read() & 0xFFF;
        const uint32_t base = ConstantBase(type_index);
        if (base != UINT32_MAX) {
          const uint8_t* source = memory_->TranslatePhysical(address);
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 0; i < amount; ++i) {
            WriteRegister(index + i, rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4));
          }
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (words < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (words < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (words < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (words < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK: {
        if (words < 2) break;
        const uint64_t high = data.Read();
        const uint64_t low = data.Read();
        bin_mask_ = (high << 32) | low;
        break;
      }
      case xenos::PM4_SET_BIN_SELECT: {
        if (words < 2) break;
        const uint64_t high = data.Read();
        const uint64_t low = data.Read();
        bin_select_ = (high << 32) | low;
        break;
      }
      case xenos::PM4_VIZ_QUERY: {
        // Like the emulation: when the query ends it is reported visible, in
        // case the game reads the result.
        if (words < 1) break;
        const uint32_t value = data.Read();
        const uint32_t id = value & 0x3F;
        if (!(value & 0x100)) {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_START);
        } else {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_END);
          const uint32_t reg = id < 32 ? uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0)
                                       : uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1);
          registers_[reg] |= UINT32_C(1) << (id < 32 ? id : id - 32);
        }
        break;
      }
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        // DRAW_INDX starts with the visibility query word.
        const uint32_t skip = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (words < skip + 1) break;
        if (skip) {
          data.Read();
        }
        const uint32_t initiator = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);
        // Indices in a buffer (DMA): its base and size come in the packet and
        // are stored in their registers, as ExecutePacketType3Draw does.
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA)) {
          if (words < skip + 3) break;
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_BASE, data.Read());
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE, data.Read());
        }
        // Indices in the packet itself.
        const uint32_t* immediate = nullptr;
        uint32_t immediate_words = 0;
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kImmediate)) {
          immediate_words = words - skip - 1;
          immediate_indices_.resize(immediate_words);
          for (uint32_t i = 0; i < immediate_words; ++i) {
            immediate_indices_[i] = data.Read();
          }
          immediate = immediate_indices_.data();
        }
        // A draw in copy mode is a resolve and/or a clear.
        if ((Register(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) ==
            uint32_t(xenos::EdramMode::kCopy)) {
          ++copies_;
          if (renderer_ready_) {
            renderer_.Resolve();
          }
        } else {
          ++draws_;
          if (renderer_ready_) {
            renderer_.Draw(immediate, immediate_words);
            if (packet_trace_enabled_ && renderer_.TakePacketTraceRequest()) LogPacketTrace();
          }
        }
        break;
      }
      case xenos::PM4_IM_LOAD: {
        // The shader microcode from guest memory (CommandProcessor::
        // ExecutePacketType3_IM_LOAD).
        if (words < 2) break;
        const uint32_t address_type = data.Read();
        const uint32_t start_size = data.Read();
        const uint32_t dwords = start_size & 0xFFFF;
        const uint32_t address = address_type & ~uint32_t(0x3) & 0x1FFFFFFF;
        if (renderer_ready_ && dwords && uint64_t(address) + dwords * 4 <= 0x20000000u) {
          renderer_.LoadShader(xenos::ShaderType(address_type & 0x3),
                               memory_->TranslatePhysical<const uint32_t*>(address), dwords,
                               address);
        }
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        // The microcode is in the packet, which may wrap around the ring.
        if (words < 2) break;
        const uint32_t type = data.Read();
        const uint32_t dwords = std::min(data.Read() & 0xFFFF, words - 2);
        shader_words_.resize(dwords);
        for (uint32_t i = 0; i < dwords; ++i) {
          // Kept in guest byte order, as the shader expects.
          shader_words_[i] = rex::byte_swap(data.Read());
        }
        if (renderer_ready_ && dwords) {
          renderer_.LoadShader(xenos::ShaderType(type & 0x3), shader_words_.data(), dwords, 0);
        }
        break;
      }
      default:
        // Cache invalidations and the rest: nothing to do.
        break;
    }
  }

  static uint32_t ConstantBase(uint32_t type_index) {
    switch ((type_index >> 16) & 0xFF) {
      case 0:
        return 0x4000;  // ALU
      case 1:
        return 0x4800;  // fetch
      case 2:
        return 0x4900;  // bool
      case 3:
        return 0x4908;  // loop
      case 4:
        return 0x2000;  // registers
      default:
        return UINT32_MAX;
    }
  }

  void NativeOcclusionResult(uint32_t address) {
    auto* counts=memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
    const uint32_t finished=rex::byte_swap(UINT32_C(0xFFFFFEED));
    const bool end=counts->ZPass_A==finished || counts->ZPass_B==finished ||
                   counts->ZFail_A==finished || counts->ZFail_B==finished;
    const bool native=renderer_ready_ && renderer_.OcclusionQueryEvent(address,end);
    static uint32_t event_logs=0;
    if(event_logs++<32)REXLOG_INFO("[native-occlusion] snapshot {:08X} sentinel_end={} measured={}",address,end,native);
    if(native)return;
    FakeOcclusionResult(address);
  }

  // As the emulation's fallback: the game marks the counters it waits for with
  // 0xFFFFFEED, and a visible sample count is written when a query ends.
  void FakeOcclusionResult(uint32_t address) {
    auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
    const uint32_t kFinished = rex::byte_swap(UINT32_C(0xFFFFFEED));
    const bool end = counts->ZPass_A == kFinished || counts->ZPass_B == kFinished ||
                     counts->ZFail_A == kFinished || counts->ZFail_B == kFinished;
    std::memset(counts, 0, sizeof(*counts));
    if (end) {
      counts->ZPass_A = kFakeOcclusionSamples;
      counts->Total_A = kFakeOcclusionSamples;
    }
  }

  void IndirectBuffer(uint32_t address, uint32_t words, int depth) {
    Reader reader;
    reader.base = memory_->TranslatePhysical(address);
    reader.pos = 0;
    reader.end = words;
    while (reader.Pending()) {
      if (!Packet(reader, depth)) {
        break;
      }
    }
  }

  // --- Test presentation -------------------------------------------------------------

  // A frame longer than 50 ms on the ring thread, broken down, at most 12
  // per report period: the drops left are single long frames, which the 10 s
  // averages hide.
  void ReportSlowFrame() {
    const auto now = Clock::now();
    const uint64_t game_wait_ns = wait_ns_ - frame_start_wait_ns_;
    frame_start_wait_ns_ = wait_ns_;
    const uint64_t idle_ns = ring_idle_ns_ - frame_start_idle_ns_;
    frame_start_idle_ns_ = ring_idle_ns_;
    const uint64_t hold_ns = hold_ns_ - frame_start_hold_ns_;
    frame_start_hold_ns_ = hold_ns_;
    const uint64_t vblank_ns = p2::pacing::g_vblank_wait_ns.load(std::memory_order_relaxed);
    const uint64_t game_vblank_ns = vblank_ns - frame_start_vblank_ns_;
    frame_start_vblank_ns_ = vblank_ns;
    const uint64_t skips = p2::pacing::g_late_skips.load(std::memory_order_relaxed);
    const uint64_t late_skips = skips - frame_start_skips_;
    frame_start_skips_ = skips;
    const auto previous = frame_start_;
    frame_start_ = now;
    if (!renderer_ready_ || previous.time_since_epoch().count() == 0) return;
    const double ms =
        double(std::chrono::duration_cast<std::chrono::microseconds>(now - previous).count()) /
        1000.0;
    const RendererStats& s = renderer_.stats();
    const uint64_t pipelines = s.pipelines_created - frame_start_pipelines_;
    const uint64_t shaders = s.shaders_translated - frame_start_shaders_;
    const uint64_t textures = s.textures_uploaded - frame_start_textures_;
    const uint64_t created = s.textures_created - frame_start_created_;
    frame_start_pipelines_ = s.pipelines_created;
    frame_start_shaders_ = s.shaders_translated;
    frame_start_textures_ = s.textures_uploaded;
    frame_start_created_ = s.textures_created;
    if (ms < 50.0 || slow_frames_logged_ >= ((P2GpuWaitEnabled && P2GpuWaitEnabled())?2u:12u)) return;
    ++slow_frames_logged_;
    REXLOG_INFO(
        "[native] slow frame {:.0f} ms: ring idle {:.1f} ms, pacing hold {:.1f} ms, game's vblank "
        "wait {:.1f} ms, late skips {}",
        ms, double(idle_ns) / 1e6, double(hold_ns) / 1e6, double(game_vblank_ns) / 1e6, late_skips);
    REXLOG_INFO(
        "[native] slow frame {:.0f} ms: waited for the game {:.1f} ms, the GPU {:.1f} ms "
        "({:.1f} of it for stream space), "
        "texture untiling {:.1f} ms, the draw thread {:.1f} ms; textures {:.1f} ms; {} draws, "
        "{} texture uploads ({} made now), {} shaders, {} pipelines",
        ms, double(game_wait_ns) / 1e6, double(s.frame_fence_wait_ns) / 1e6,
        double(s.frame_stream_wait_ns) / 1e6,
        double(s.frame_upload_wait_ns) / 1e6, double(s.frame_drain_wait_ns) / 1e6,
        double(s.frame_texture_ns) / 1e6, s.frame_draws, textures, created, shaders, pipelines);
  }

  // The frame rate cap (p2_min_frame_vblanks): a frame is presented at a
  // vblank at least that many after the previous present, or at once if it is
  // later than that. The hold used to come after the present, before the
  // swap-complete interrupt: an early frame went on screen at once and the
  // next one, if a little over budget, 50 ms later, a short gap then a long
  // one, felt as a hitch even at 30 fps on average. Held before it, presents
  // are evenly spaced, and the interrupt still follows the present.
  void PaceFrame() {
  p2::gpuwait::Scope gpu_wait_scope(p2::gpuwait::Pacing);
    const uint32_t min_vblanks = REXCVAR_GET(p2_min_frame_vblanks);
    if (min_vblanks > 1 && last_swap_vblank_) {
      const uint64_t target = last_swap_vblank_ + min_vblanks;
      if (REXCVAR_GET(p2_pace_before_present) &&
          vblanks_.load(std::memory_order_acquire) < target && renderer_ready_) {
        // The frame's draws go to the GPU now, not after the wait.
        renderer_.Flush();
      }
      const auto hold_start = Clock::now();
      const auto limit = hold_start + std::chrono::milliseconds(100);
      while (vblanks_.load(std::memory_order_acquire) < target &&
             running_.load(std::memory_order_acquire) && Clock::now() < limit) {
        rex::thread::Sleep(std::chrono::microseconds(250));
      }
      hold_ns_ += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - hold_start)
              .count());
    }
    last_swap_vblank_ = vblanks_.load(std::memory_order_acquire);
  }

  void Present() {
  p2::gpuwait::Scope gpu_wait_scope(p2::gpuwait::RingSwap);
    if(P2GpuWaitFrame)P2GpuWaitFrame();
    const uint64_t swap = ++swaps_;
    g_swaps_presented.fetch_add(1, std::memory_order_relaxed);
    counter_.fetch_add(1, std::memory_order_relaxed);
#if defined(__SWITCH__)
    RexSwitchPerfCount(0);
#endif
    if (renderer_ready_) {
      renderer_.Swap(presenter_.get(), swap_frontbuffer_, swap_width_, swap_height_);
      return;
    }
    if (!presenter_) {
      return;
    }
    presenter_->RefreshGuestOutput(
        kOutputWidth, kOutputHeight, kOutputWidth, kOutputHeight,
        [this, swap](rex::ui::Presenter::GuestOutputRefreshContext& context) {
          return ClearOutput(static_cast<OutputContext&>(context), swap);
        });
  }

  bool ClearOutput(OutputContext& context, uint64_t swap) {
  p2::gpuwait::Scope gpu_wait_scope(p2::gpuwait::ClearFence);
    const rex::ui::vulkan::VulkanDevice* device_info = provider_->vulkan_device();
    const auto& dfn = device_info->functions();
    const VkDevice device = device_info->device();

    if (render_pass_ == VK_NULL_HANDLE) {
      VkAttachmentDescription attachment{};
      attachment.format = rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachment.finalLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
      VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &reference;
      VkRenderPassCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      info.attachmentCount = 1;
      info.pAttachments = &attachment;
      info.subpassCount = 1;
      info.pSubpasses = &subpass;
      if (dfn.vkCreateRenderPass(device, &info, nullptr, &render_pass_) != VK_SUCCESS) {
        render_pass_ = VK_NULL_HANDLE;
        return false;
      }
    }

    if (pool_ == VK_NULL_HANDLE) {
      VkCommandPoolCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      info.queueFamilyIndex = device_info->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &info, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        return false;
      }
      VkCommandBufferAllocateInfo allocate{};
      allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      allocate.commandPool = pool_;
      allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocate.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &allocate, &commands_) != VK_SUCCESS) {
        commands_ = VK_NULL_HANDLE;
      }
      VkFenceCreateInfo fence_info{};
      fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      if (dfn.vkCreateFence(device, &fence_info, nullptr, &fence_) != VK_SUCCESS) {
        fence_ = VK_NULL_HANDLE;
      }
    }
    if (commands_ == VK_NULL_HANDLE || fence_ == VK_NULL_HANDLE) {
      return false;
    }

    // A single command buffer: wait for the previous one before reusing it.
    if (fence_pending_) {
      dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
      dfn.vkResetFences(device, 1, &fence_);
      fence_pending_ = false;
    }
    dfn.vkResetCommandPool(device, pool_, 0);

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == context.image_version()) {
        framebuffer = f.framebuffer;
      }
    }
    if (framebuffer == VK_NULL_HANDLE) {
      Framebuffer& f = framebuffers_[next_framebuffer_];
      next_framebuffer_ = (next_framebuffer_ + 1) % std::size(framebuffers_);
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
      }
      VkImageView view = context.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = render_pass_;
      info.attachmentCount = 1;
      info.pAttachments = &view;
      info.width = kOutputWidth;
      info.height = kOutputHeight;
      info.layers = 1;
      if (dfn.vkCreateFramebuffer(device, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = context.image_version();
      framebuffer = f.framebuffer;
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn.vkBeginCommandBuffer(commands_, &begin) != VK_SUCCESS) {
      return false;
    }
    // Test colour: green rises and falls with the swap count, so a capture
    // tells a running game from a stalled one.
    const float phase = float(swap % 240) / 239.0f;
    VkClearValue color{};
    color.color.float32[0] = 0.05f;
    color.color.float32[1] = 0.10f + 0.40f * phase;
    color.color.float32[2] = 0.35f;
    color.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass_;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent.width = kOutputWidth;
    pass.renderArea.extent.height = kOutputHeight;
    pass.clearValueCount = 1;
    pass.pClearValues = &color;
    dfn.vkCmdBeginRenderPass(commands_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    dfn.vkCmdEndRenderPass(commands_);
    if (dfn.vkEndCommandBuffer(commands_) != VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands_;
    {
      const auto queue =
          p2::gpuwait::Measure(p2::gpuwait::QueueLock,[&]{return device_info->AcquireQueue(device_info->queue_family_graphics_compute(), 0);});
      if (p2::gpuwait::Measure(p2::gpuwait::QueueSubmit,[&]{return dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence_);}) != VK_SUCCESS) {
        return false;
      }
    }
    fence_pending_ = true;
    context.SetIs8bpc(true);
    return true;
  }

  void DestroyVulkan() {
    if (!provider_ || !provider_->vulkan_device()) {
      return;
    }
    const rex::ui::vulkan::VulkanDevice* device_info = provider_->vulkan_device();
    const auto& dfn = device_info->functions();
    const VkDevice device = device_info->device();
    if (fence_ != VK_NULL_HANDLE) {
      if (fence_pending_) {
        p2::gpuwait::Scope gpu_shutdown_wait(p2::gpuwait::ShutdownWait);
        dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
        fence_pending_ = false;
      }
      dfn.vkDestroyFence(device, fence_, nullptr);
      fence_ = VK_NULL_HANDLE;
    }
    for (Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f = Framebuffer{};
      }
    }
    if (pool_ != VK_NULL_HANDLE) {
      dfn.vkDestroyCommandPool(device, pool_, nullptr);
      pool_ = VK_NULL_HANDLE;
      commands_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_, nullptr);
      render_pass_ = VK_NULL_HANDLE;
    }
  }

  // --- State -------------------------------------------------------------------------

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;

  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  bool mmio_registered_ = false;

  std::atomic<bool> running_{false};
  rex::system::object_ref<rex::system::XHostThread> vblank_thread_;
  rex::system::object_ref<rex::system::XHostThread> ring_thread_;

  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> callback_data_{0};
  // Incremented by vblanks and swaps; EVENT_WRITE_SHD can write it (the SDK's
  // command processor counter).
  std::atomic<uint32_t> counter_{0};
  // Vblanks delivered, and the vblank of the last swap-complete interrupt.
  std::atomic<uint64_t> vblanks_{0};
  uint64_t last_swap_vblank_ = 0;
  Clock::time_point frame_start_{};
  uint64_t frame_start_wait_ns_ = 0, frame_start_pipelines_ = 0, frame_start_shaders_ = 0,
           frame_start_textures_ = 0, frame_start_created_ = 0;
  uint32_t slow_frames_logged_ = 0;
  uint64_t ring_idle_ns_ = 0, hold_ns_ = 0;
  uint64_t frame_start_idle_ns_ = 0, frame_start_hold_ns_ = 0, frame_start_vblank_ns_ = 0,
           frame_start_skips_ = 0;
  bool swap_interrupt_pending_ = false;

  std::atomic<uint32_t> ring_base_{0};
  std::atomic<uint32_t> ring_words_{0};
  std::atomic<uint32_t> ring_generation_{0};
  std::atomic<uint32_t> write_pointer_{0};
  std::atomic<uint32_t> read_pointer_writeback_{0};
  std::mutex ring_mutex_;
  std::condition_variable ring_cv_;

  // Touched by the ring thread and, through MMIO, by the game's threads; as in
  // the emulation, races on these integers are harmless.
  // The renderer reads the GPU state from here: it is the SDK's register file,
  // so its helpers (draw_util and the rest) work on it unchanged.
  std::unique_ptr<rex::graphics::RegisterFile> register_file_;
  uint32_t* registers_;
  Renderer renderer_;
  bool renderer_ready_ = false;
  // Packet history for the water diagnostic (Renderer::water_dump_enabled).
  struct PacketTraceEntry {
    uint32_t header = 0, count = 0, bin_select = 0, bin_mask = 0;
    uint32_t words[4] = {};
  };
  bool packet_trace_enabled_ = false;
  std::array<PacketTraceEntry, 160> packet_trace_{};
  uint64_t packet_trace_next_ = 0;
  void LogPacketTrace() {
    const uint64_t n = std::min<uint64_t>(packet_trace_next_, packet_trace_.size());
    REXLOG_INFO("[native] PKTTRACE last {} packets before the water simulation draw", n);
    for (uint64_t i = packet_trace_next_ - n; i < packet_trace_next_; ++i) {
      const PacketTraceEntry& e = packet_trace_[i % packet_trace_.size()];
      const uint32_t type = e.header >> 30;
      REXLOG_INFO("[native] PKTTRACE t{} {} hdr {:08X} n {} pred {} bins {:08X}/{:08X} | {:08X} "
                  "{:08X} {:08X} {:08X}",
                  type, type == 3 ? fmt::format("op {:02X}", (e.header >> 8) & 0x7F)
                                  : fmt::format("reg {:04X}", e.header & 0x7FFF),
                  e.header, e.count, type == 3 ? (e.header & 1) : 0, e.bin_select, e.bin_mask,
                  e.words[0], e.words[1], e.words[2], e.words[3]);
    }
  }
  // CommandProcessor's gamma ramp state (DC_LUT_*), 256-entry table only.
  uint32_t gamma_ramp_[256] = {};
  uint32_t gamma_ramp_component_ = 0;
  std::vector<uint32_t> immediate_indices_;
  std::vector<uint32_t> shader_words_;
  uint64_t bin_mask_ = ~uint64_t(0);
  uint64_t bin_select_ = ~uint64_t(0);

  uint32_t swap_frontbuffer_ = 0;
  uint32_t swap_width_ = 0;
  uint32_t swap_height_ = 0;

  // Ring thread only.
  uint64_t packets_ = 0, draws_ = 0, copies_ = 0, swaps_ = 0, waits_cut_short_ = 0;
  uint64_t waits_ = 0, wait_ns_ = 0, reported_waits_ = 0, reported_wait_ns_ = 0;
  uint64_t reported_packets_ = 0, reported_draws_ = 0, reported_copies_ = 0,
           reported_swaps_ = 0;

  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint64_t version = 0;
  };
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer commands_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool fence_pending_ = false;
  Framebuffer framebuffers_[3];
  size_t next_framebuffer_ = 0;
};

}  // namespace

bool Enabled() { return REXCVAR_GET(p2_renderer) == "native"; }

uint64_t SwapsPresented() { return g_swaps_presented.load(std::memory_order_relaxed); }

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem() {
  return std::make_unique<NativeGraphicsSystem>();
}

}  // namespace p2::native
