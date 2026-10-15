// SPDX-License-Identifier: BSD-3-Clause
// Release-only FPS/runtime integration derived from the ReXGlue Horizon SDK.
// No stack sampling, CPU reports, profiling counters or diagnostic libnx timers.
#include <switch.h>
#include <atomic>
#include <cstdio>
#include <malloc.h>
#include <rex/ui/switch_apm.h>
#include <rex/ui/switch_saltynx.h>
#include <rex/ui/switch_sysclk.h>
extern "C" {
const char* RexSwitchLogDir(void);
Result __real_threadCreate(Thread*,ThreadFunc,void*,void*,size_t,int,int);
Result __real_threadClose(Thread*);
Result __real_nvFenceWait(NvFence*,s32);
Result __real_nvGpuChannelKickoff(NvGpuChannel*);
Result __real_nvMapCreate(NvMap*,void*,u32,u32,NvKind,bool);
Result __real_nvAddressSpaceAllocFixed(NvAddressSpace*,bool,u64,iova_t);
Result __real_nvioctlNvhostAsGpu_MapBufferEx(u32,u32,u32,u32,u32,u64,u64,u64,u64*);
void __real_armDCacheClean(void*,size_t);
Result __real_nwindowQueueBuffer(NWindow*,s32,const NvMultiFence*);
Result __real_bqDequeueBuffer(Binder*,bool,u32,u32,s32,u32,s32*,NvMultiFence*);
void __real_svcSleepThread(s64);
}
namespace {
std::atomic<u64> g_counters[1]{};
std::atomic<uint32_t> g_resolution{(1280u<<16)|720u};
Thread g_thread;
struct TicOverlay {
  u64 tic = 0;
  u64 presented = 0;
  double media[10] = {};
  size_t pos = 0;

  void Boot(u64 now) {
    tic = now;
    presented = g_counters[0].load(std::memory_order_relaxed);
    // Publish what is known right away: the resolution is valid from the first instant (g_resolution
    // starts at 1280x720 and the game corrects it when choosing the video mode), so the overlay's RES
    // row can show up even before the first frame.
    const uint32_t res = g_resolution.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Update(0.0, 0.0, res >> 16, res & 0xFFFF, presented);
  }

  // Once per second. `with_clocks` turns off the Reverse-NX and clock sysmodule part, which is better
  // left alone during warm-up.
  void Step(u64 now, u64 freq, bool with_clocks) {
    if (now - tic < freq) {
      return;
    }
    const double seconds = double(now - tic) / double(freq);
    const u64 presented_now = g_counters[0].load(std::memory_order_relaxed);
    const double fps = seconds > 0.0 ? double(presented_now - presented) / seconds : 0.0;
    media[pos] = fps;
    pos = (pos + 1) % 10;
    double sum = 0.0;
    size_t how_many = 0;
    for (double v : media) {
      if (v > 0.0) {
        sum += v;
        ++how_many;
      }
    }
    const uint32_t res = g_resolution.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Update(fps, how_many ? sum / double(how_many) : fps, res >> 16, res & 0xFFFF,
                                        presented_now);
    // Reverse-NX is also told that the game keeps asking for the mode (its overlay requires it to show
    // the controls) and, if the system rules, the real mode is mirrored.
    // And if requested, the clock sysmodule is asked for its docked clocks.
    if (with_clocks) {
      const bool docked_real = appletGetOperationMode() == AppletOperationMode_Console;
      const bool docked = rex::ui::switch_saltynx::BaseMode(docked_real);
      rex::ui::switch_sysclk::FollowMode(docked, docked_real);
    }
    /*
     * The memory clock is watched here too. It is outside the if above on purpose: the
     * system raises the EMC right after we request the GPU profile, that is, during
     * warm-up, which is when with_clocks is false. It is one clkrst read per second for
     * the first 20 and one every 30 after that; when we have not changed anything it
     * does not even do that.
     */
    RexSwitchApmWatch();
    presented = presented_now;
    tic = now;
  }
};

void ReleaseFpsMain(void*) {
 const u64 freq=armGetSystemTickFreq();
 TicOverlay overlay; overlay.Boot(armGetSystemTick());
 while(true){__real_svcSleepThread(1000000000LL);overlay.Step(armGetSystemTick(),freq,true);}
}
__attribute__((constructor(102))) void StartReleaseFps() {
 if(R_SUCCEEDED(__real_threadCreate(&g_thread,ReleaseFpsMain,nullptr,nullptr,0x10000,0x3B,-2)))threadStart(&g_thread);
}
} // namespace
extern "C" {

Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                           size_t stack_sz, int prio, int cpuid) {
  const Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
  if (R_SUCCEEDED(rc) && t) {
    // No profiling registration.
  } else if (R_FAILED(rc)) {
    // pthread maps several distinct Horizon failures to ENOMEM. Keep the
    // original result and heap counters so allocation and mapping failures
    // can be distinguished without altering thread behavior.
    const auto heap = mallinfo();
    char path[1024];
    std::snprintf(path, sizeof(path), "%sthread_create.log", RexSwitchLogDir());
    if (FILE* f = std::fopen(path, "a")) {
      std::fprintf(f, "threadCreate result=0x%08X stack=%zu priority=%d core=%d "
                      "heap_used=%d heap_free=%d largest_tail=%d\n",
                   rc, stack_sz, prio, cpuid, heap.uordblks, heap.fordblks, heap.keepcost);
      std::fclose(f);
    }
  }
  return rc;
}

Result __wrap_threadClose(Thread* t) {
  if (t) {
    // No profiling registration.
  }
  return __real_threadClose(t);
}

/* libnx calls that cost IPC or walk memory. See g_calls. */
Result __wrap_nvFenceWait(NvFence* f, s32 timeout_us) {
  const Result rc = __real_nvFenceWait(f, timeout_us);
  return rc;
}

Result __wrap_nvGpuChannelKickoff(NvGpuChannel* c) {
  const Result rc = __real_nvGpuChannelKickoff(c);
  return rc;
}

Result __wrap_nvMapCreate(NvMap* m, void* cpu_addr, u32 size, u32 align, NvKind kind, bool is_cpu_cacheable) {
  const Result rc = __real_nvMapCreate(m, cpu_addr, size, align, kind, is_cpu_cacheable);
  return rc;
}

Result __wrap_nvAddressSpaceAllocFixed(NvAddressSpace* a, bool sparse, u64 size, iova_t iova) {
  const Result rc = __real_nvAddressSpaceAllocFixed(a, sparse, size, iova);
  return rc;
}

Result __wrap_nvioctlNvhostAsGpu_MapBufferEx(u32 fd, u32 flags, u32 kind, u32 nvmap_handle, u32 page_size,
                                             u64 buffer_offset, u64 mapping_size, u64 input_offset, u64* offset) {
  const Result rc = __real_nvioctlNvhostAsGpu_MapBufferEx(fd, flags, kind, nvmap_handle, page_size, buffer_offset,
                                                          mapping_size, input_offset, offset);
  return rc;
}

void __wrap_armDCacheClean(void* addr, size_t size) {
  __real_armDCacheClean(addr, size);
}

/*
 * The presentation interval (masseffect_interval_swap).
 *
 * It is reapplied on every present, not when the chain is created, on purpose: the WSI sets it to 1
 * when creating the swapchain, and the chain is recreated when switching from docked to handheld.
 * With 0 nothing is touched and the swapchain mode rules (IMMEDIATE -> interval 0).
 *
 * libnx only accepts interval 0 if the NWindow has three or more buffers; the Horizon WSI always
 * configures exactly 3, so the condition holds. 2 is always accepted.
 *
 * Warning: it is 0 and must stay that way. Measured: with the current frame time, setting 2 sends
 * 22 % of the frames to 66.7 ms. It only makes sense with a median frame time below 31 ms.
 */
std::atomic<unsigned> g_interval_swap{0};

Result __wrap_nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence) {
  const unsigned request = g_interval_swap.load(std::memory_order_relaxed);
  if (request != 0 && nw != nullptr && nw->swap_interval != request) {
    nwindowSetSwapInterval(nw, request);
  }
  const Result rc = __real_nwindowQueueBuffer(nw, slot, fence);
  return rc;
}

Result __wrap_bqDequeueBuffer(Binder* b, bool async, u32 width, u32 height, s32 format, u32 usage, s32* buf,
                              NvMultiFence* fence) {
  const Result rc = __real_bqDequeueBuffer(b, async, width, height, format, usage, buf, fence);
  return rc;
}

void __wrap_svcSleepThread(s64 nano) {
  __real_svcSleepThread(nano);
}

/*
 * Counters: 0 = guest present, 1 = read emulated through the shadow,
 * 2 = fault resolved by an SDK handler, 3 = retry emulated through the
 * shadow, 4 = SEH. 28..32 = draws per surface width from IssueDraw
 * (1600 or more, 1280-1599, 640-1279, 256-639, under 256).
 */
// A long frame, in armGetSystemTick ticks (see WindowHitch).
void RexSwitchPerfHitch(u64 start, u64 end) {
  // Disabled in the release mode.
}

void RexSwitchPerfExclusiveCore(int core) {
  (void)core; // Profile-driven experimental affinity is disabled in release mode.
}

void RexSwitchPerfResolution(unsigned w, unsigned h) {
  g_resolution.store(((w & 0xFFFF) << 16) | (h & 0xFFFF), std::memory_order_relaxed);
}

// See g_interval_swap, next to the nwindowQueueBuffer wrapper. 0 = touch nothing (the normal case).
void RexSwitchPerfIntervalSwap(unsigned vblanks) {
  g_interval_swap.store(vblanks, std::memory_order_relaxed);
}

void RexSwitchPerfCount(unsigned id) {
  if (id != 0) return;
  g_counters[0].fetch_add(1, std::memory_order_relaxed);
  const uint32_t res = g_resolution.load(std::memory_order_relaxed);
  rex::ui::switch_saltynx::Beat(res >> 16, res & 0xFFFF);
}

/*
 * 5 draws, 6 scissor area of the draws (pixels), 7 queue submissions,
 * 9 resolves, 10 resolve area, 11 render target transfer calls,
 * 12 render targets transferred, 13 transfer draws, 14 textures loaded,
 * 15 shared memory bytes uploaded, 16 pipelines created, 17 physical memory
 * chunks committed on touch, 18 chunks mapped into a view on touch,
 * 21 clipped audio samples, 22 audio buffers with a peak of 0.98 or more.
 */
void RexSwitchPerfAdd(unsigned id, u64 value) {
  // Disabled in the release mode.
}

/* max counter (23 = audio mix peak in ten-thousandths; the report resets it to 0). */
void RexSwitchPerfMax(unsigned id, u64 value) {
  // Disabled in the release mode.
}

/*
 * Bits: 1 draws, 2 transfers, 4 resolves, 8 texture loads,
 * 16 presentation effect, 32 tiled dispatch.
 */
void RexSwitchPerfSceneFrame(uint32_t draws) {
  // Disabled in the release mode.
}

bool RexSwitchPerfSkip(unsigned bit) {
  return false;
}

void RexSwitchPerfSetThreadName(u32 handle, const char* name) {
  // Disabled in the release mode.
}

}  // extern "C"

extern "C" void RexSwitchPerfToggleAb(void) {} // A/B diagnostic mode disabled.
