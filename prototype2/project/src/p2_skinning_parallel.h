// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2 (WORKLOG #92): big skinning calls split with one host helper thread.
// run63: p2skin::Skin ran on the game's render/simulation thread (23 % of its 69 %, ~11 ms per frame) while the
// cores 1-2 had idle time. The owner thread publishes the call, both threads take 32-vertex chunks from one
// range, the owner from the front, the helper from the back (the owner never waits for the helper to start:
// if it is late, the owner does everything); the owner returns only once the helper has left the call.
// Same per-vertex operations. Bounds: min/max keep the LATER vertex on ties (+0/-0), so the sequential result is
// reproduced exactly: the owner's part is [0, m) folded in order, the helper's part [m, n) is folded chunk by
// chunk from the back keeping the higher-index chunk on ties, then min(owner, helper) keeps the helper's on ties
// (tests/skinning_diff.cpp checks the same shape). The helper runs with the owner's FPCR (flush mode).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

extern "C" void RexSwitchSetCurrentThreadPriority(int priority);  // SDK threading_switch
extern "C" bool RexSwitchSetCurrentThreadMask(uint32_t mask);

namespace p2skinpar {

constexpr uint32_t kChunk = 32;

struct Task {
  void (*fn)(void* user, uint32_t k0, uint32_t k1, simde__m128& lo, simde__m128& hi) = nullptr;
  void* user = nullptr;
  uint32_t n = 0;
  uint64_t fpcr = 0;
  simde__m128 init_lo, init_hi;
  std::atomic<uint64_t> ends{0};  // front chunk << 32 | back chunk (exclusive), both claim with CAS
  std::atomic<uint32_t> helper_vertices{0};
  simde__m128 helper_lo, helper_hi;
  bool helper_folded = false;
};

inline std::atomic<Task*> g_task{nullptr};
inline std::atomic<int> g_active{0};
inline std::atomic<bool> g_sleeping{false};
inline std::atomic<bool> g_started{false};
inline std::mutex g_mutex;
inline std::condition_variable g_cv;
inline std::atomic<uint32_t> g_spin_us{150};
inline std::atomic<uint64_t> g_calls{0}, g_split{0}, g_vertices{0}, g_helper_vertices{0};

// Claims a chunk index from the front (owner) or the back (helper); -1 when none is left.
inline int64_t Claim(Task* t, bool back) {
  uint64_t e = t->ends.load(std::memory_order_relaxed);
  for (;;) {
    const uint32_t front = uint32_t(e >> 32), end = uint32_t(e);
    if (front >= end) return -1;
    const uint64_t next = back ? (uint64_t(front) << 32 | (end - 1)) : (uint64_t(front + 1) << 32 | end);
    if (t->ends.compare_exchange_weak(e, next, std::memory_order_relaxed)) return back ? end - 1 : front;
  }
}

inline uint64_t ReadFpcr() { uint64_t v; asm volatile("mrs %0, fpcr" : "=r"(v)); return v; }
inline void WriteFpcr(uint64_t v) { asm volatile("msr fpcr, %0" : : "r"(v)); }

inline void HelperWork(Task* t) {
  const uint64_t saved = ReadFpcr();
  if (saved != t->fpcr) WriteFpcr(t->fpcr);
  simde__m128 lo = t->init_lo, hi = t->init_hi;
  uint32_t done = 0;
  for (int64_t c; (c = Claim(t, true)) >= 0;) {
    const uint32_t k0 = uint32_t(c) * kChunk, k1 = std::min(t->n, k0 + kChunk);
    simde__m128 clo = t->init_lo, chi = t->init_hi;
    t->fn(t->user, k0, k1, clo, chi);
    lo = simde_mm_min_ps(clo, lo);  // ties keep `lo`: the higher-index chunks, processed first
    hi = simde_mm_max_ps(chi, hi);
    done += k1 - k0;
  }
  if (done) {
    t->helper_lo = lo;
    t->helper_hi = hi;
    t->helper_folded = true;
    t->helper_vertices.store(done, std::memory_order_relaxed);
  }
  if (saved != t->fpcr) WriteFpcr(saved);
}

inline void HelperLoop(int priority, uint32_t mask) {
  RexSwitchSetCurrentThreadPriority(priority);
  // Falls back to cores 1-2 when the process may not use the requested cores (stock hbl: no core 3).
  if (mask && !RexSwitchSetCurrentThreadMask(mask)) RexSwitchSetCurrentThreadMask(0x6);
  auto last_work = std::chrono::steady_clock::now();
  for (;;) {
    Task* t = g_task.load(std::memory_order_acquire);
    if (t) {
      g_active.fetch_add(1, std::memory_order_seq_cst);
      if (g_task.load(std::memory_order_seq_cst) == t) {
        HelperWork(t);
        last_work = std::chrono::steady_clock::now();
      }
      g_active.fetch_sub(1, std::memory_order_release);
      // Do not take the same finished call again.
      while (g_task.load(std::memory_order_acquire) == t) asm volatile("yield");
      continue;
    }
    // run66: reading the clock every poll was over half of the helper's time; check it every 256 polls.
    static thread_local uint32_t polls = 0;
    if ((++polls & 255) != 0) {
      asm volatile("yield");
      continue;
    }
    if (std::chrono::steady_clock::now() - last_work <
        std::chrono::microseconds(g_spin_us.load(std::memory_order_relaxed))) {
      continue;
    }
    std::unique_lock<std::mutex> lock(g_mutex);
    g_sleeping.store(true, std::memory_order_seq_cst);
    g_cv.wait_for(lock, std::chrono::milliseconds(2),
                  [] { return g_task.load(std::memory_order_seq_cst) != nullptr; });
    g_sleeping.store(false, std::memory_order_relaxed);
    last_work = std::chrono::steady_clock::now();
  }
}

inline void Start(int priority, uint32_t mask, uint32_t spin_us) {
  g_spin_us.store(spin_us, std::memory_order_relaxed);
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) return;
  // Never joined and never detached: libnx's pthread_detach fails (run64: std::thread::detach threw
  // std::system_error -> std::terminate on the first big skinning call). Leaked on purpose.
  new std::thread([priority, mask] { HelperLoop(priority, mask); });
}

// The runner given to p2skin::Skin.
struct Parallel {
  uint32_t min_vertices;
  template <class F>
  void operator()(uint32_t n, F&& range, simde__m128& v32, simde__m128& v33) const {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_vertices.fetch_add(n, std::memory_order_relaxed);
    if (n < min_vertices || !g_started.load(std::memory_order_relaxed)) {
      range(0u, n, v32, v33);
      return;
    }
    Task t;
    t.fn = [](void* user, uint32_t k0, uint32_t k1, simde__m128& lo, simde__m128& hi) {
      (*static_cast<F*>(user))(k0, k1, lo, hi);
    };
    t.user = const_cast<void*>(static_cast<const void*>(&range));
    t.n = n;
    t.fpcr = ReadFpcr();
    t.init_lo = v32;
    t.init_hi = v33;
    t.ends.store((n + kChunk - 1) / kChunk, std::memory_order_relaxed);
    g_task.store(&t, std::memory_order_seq_cst);
    if (g_sleeping.load(std::memory_order_seq_cst)) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_cv.notify_one();
    }
    for (int64_t c; (c = Claim(&t, false)) >= 0;) {
      const uint32_t k0 = uint32_t(c) * kChunk;
      range(k0, std::min(n, k0 + kChunk), v32, v33);
    }
    g_task.store(nullptr, std::memory_order_seq_cst);
    while (g_active.load(std::memory_order_seq_cst) != 0) asm volatile("yield");
    std::atomic_thread_fence(std::memory_order_acquire);
    if (t.helper_folded) {
      v32 = simde_mm_min_ps(v32, t.helper_lo);  // ties keep the helper's: its vertices come later
      v33 = simde_mm_max_ps(v33, t.helper_hi);
      g_split.fetch_add(1, std::memory_order_relaxed);
      g_helper_vertices.fetch_add(t.helper_vertices.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
  }
};

}  // namespace p2skinpar
