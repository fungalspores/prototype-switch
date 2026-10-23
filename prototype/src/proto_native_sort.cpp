// Prototype (4156084E) on Switch: the scene's per-frame depth sort, done natively.
//
// Found with the all-threads stack sampler (p2_thread_sample, 2026-10-19): in frames over 50 ms the main
// thread spends ~14% of its working time in sub_822572F8 - the game's std::sort (MSVC introsort: quicksort
// with median of 9 above 32 elements, insertion sort below, heap sort fallback) - and its comparator
// sub_82254C78, against ~3.5% in normal frames. It is called once per scene pass from sub_82257618 on an
// array of 24-byte entries, ordered by the float at +16 (ascending: sub_82254C78 returns a[+16] < b[+16]).
// In the recompiled code every comparison is an indirect guest call and every element move goes through
// guest loads and stores; the sort below reads the keys once and moves 24-byte blocks on the host.
//
// Only the call with this comparator is replaced (the only caller outside the sort itself). Entries with
// equal keys keep their relative order (stable sort); MSVC's sort leaves them in an unspecified order, so
// the result can differ from the original only between entries at exactly the same depth.
#include "generated/default/prototype_pch.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(proto_native_depth_sort, true, "Prototype",
                    "Sort the scene's per-frame depth list (sub_822572F8 with comparator sub_82254C78) on the "
                    "host instead of running the game's recompiled std::sort");

REX_EXTERN(__imp__sub_822572F8);

namespace {

constexpr uint32_t kComparator = 0x82254C78;
constexpr uint32_t kEntrySize = 24;
constexpr uint32_t kKeyOffset = 16;

struct Entry {
  uint8_t bytes[kEntrySize];
};

struct Keyed {
  float key;
  uint32_t index;
};

std::atomic<uint32_t> g_calls{0};

float KeyOf(const uint8_t* entry) {
  uint32_t raw;
  std::memcpy(&raw, entry + kKeyOffset, 4);
  raw = __builtin_bswap32(raw);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

}  // namespace

// r3 = first, r4 = last (one past the end), r5 = ideal (recursion budget), r6 = comparator.
extern "C" REX_FUNC(sub_822572F8) {
  const uint32_t first = ctx.r3.u32, last = ctx.r4.u32;
  // Arrays crossing into the 0xE0000000 physical view (+0x1000 on the host) take the guest path.
  if (ctx.r6.u32 != kComparator || !REXCVAR_GET(proto_native_depth_sort) || last < first ||
      (last - first) % kEntrySize != 0 || REX_PHYS_HOST_OFFSET(first) != REX_PHYS_HOST_OFFSET(last)) {
    __imp__sub_822572F8(ctx, base);
    return;
  }
  const uint32_t count = (last - first) / kEntrySize;
  if (count < 2) return;

  thread_local std::vector<Keyed> keys;
  thread_local std::vector<Entry> copy;
  keys.resize(count);
  copy.resize(count);
  uint8_t* data = REX_RAW_ADDR(first);
  std::memcpy(copy.data(), data, size_t(count) * kEntrySize);
  for (uint32_t i = 0; i < count; ++i) {
    float k = KeyOf(copy[i].bytes);
    // The game compares with fcmpu: a NaN is never "less". Placing NaNs last keeps the order strict.
    if (k != k) k = __builtin_inff();
    keys[i] = {k, i};
  }
  std::stable_sort(keys.begin(), keys.end(),
                   [](const Keyed& a, const Keyed& b) { return a.key < b.key; });
  for (uint32_t i = 0; i < count; ++i) {
    std::memcpy(data + size_t(i) * kEntrySize, copy[keys[i].index].bytes, kEntrySize);
  }

  const uint32_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || n % 18000 == 0) {
    REXLOG_INFO("[proto-sort] native depth sort: {} entries ({} call(s) so far)", count, n);
  }
  // The guest function returns nothing the caller reads; r3 keeps first as on return there.
  ctx.r3.u64 = first;
}
