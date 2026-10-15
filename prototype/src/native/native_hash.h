// SPDX-License-Identifier: GPL-3.0-or-later
//
// Change detection for guest data the native renderer copies (textures and
// vertex buffers).
//
// Hashing every byte of everything a frame uses was half of the renderer's CPU
// time in Free Skate. Most of that data never changes after loading, so once a
// range has been identical for kStableChecks full hashes it is checked each
// frame by hashing a few small samples spread over it, and hashed in full only
// one frame in kFullEvery (staggered, so the full hashes do not all land on the
// same frame). A change the samples miss is picked up by the next full hash,
// at most kFullEvery frames later. The emulated path trusts stable guest pages
// the same way (sampled page hashing in the SDK's shared memory).
//
// Ranges unchanged for kTrustedChecks checks are only sampled one frame in
// kTrustedEvery: each sample is a cache miss on guest memory, and with a few
// thousand ranges in a frame the sampling alone cost milliseconds. A change
// to such a range shows up at most kTrustedEvery frames late.

#pragma once

#include <algorithm>
#include <cstddef>
#include <bit>
#include <cstdint>
#include <vector>

#include <xxhash.h>

namespace p2::native {

class ChangeDetector {
 public:
  static constexpr uint32_t kStableChecks = 8;
  // 12, not 32: until trusted, a texture's lookup runs in full every frame,
  // and a busy scene's thousands of textures stayed on that path for a second
  // after appearing (15 ms of texture lookups in 3000-draw frames).
  static constexpr uint32_t kTrustedChecks = 12;
  static constexpr uint64_t kFullEvery = 16;
  static constexpr uint64_t kTrustedEvery = 8;

  // Returns the content hash of [data, data + size), hashing it all only when
  // needed. `stagger` spreads the full hashes over frames.
  // The last frame, from `frame` on, whose Check is certain to return the hash
  // it returned for `frame` without looking at the data: `frame` itself
  // unless the content is trusted.
  uint64_t TrustedUntil(uint64_t frame, uint64_t stagger) const {
    if (unchanged_ < TrustedChecks()) return frame;
    return frame + (kTrustedEvery - (frame + stagger) % kTrustedEvery) - 1;
  }

  // Mercenaries 2 (HANDOFF section 56): a full hash every time, no sampling and no trusted
  // skips. The sampled/trusted shortcuts kept a part of the water mesh stale (the game fills
  // or updates buffers piecewise; the SDK's emulation saw every write through page watching).
  uint64_t CheckExact(const uint8_t* data, size_t size, uint64_t& bytes_hashed) {
    uint64_t hash = XXH3_64bits(data, size);
    bytes_hashed += size;
    if (hash == hash_ && checked_) {
      if (unchanged_ < 0xFFFF) ++unchanged_;
    } else {
      if (checked_ && changes_ < 0xFFFF) ++changes_;
      unchanged_ = 0;
    }
    hash_ = hash;
    sample_ = 0;
    checked_ = true;
    return hash;
  }

  uint64_t Check(const uint8_t* data, size_t size, uint64_t frame, uint64_t stagger,
                 uint64_t& bytes_hashed) {
    const uint32_t trusted_checks = TrustedChecks();
    if (unchanged_ >= trusted_checks && (frame + stagger) % kTrustedEvery) {
      return hash_;
    }
    uint64_t sample = Sample(data, size);
    if (unchanged_ >= StableChecks() && sample == sample_ && (frame + stagger) % kFullEvery) {
      if (unchanged_ < trusted_checks) {
        ++unchanged_;
      }
      bytes_hashed += kSamples * kSampleBytes;
      return hash_;
    }
    uint64_t hash = XXH3_64bits(data, size);
    bytes_hashed += size;
    if (hash == hash_ && checked_) {
      if (unchanged_ < trusted_checks) {
        ++unchanged_;
      }
    } else {
      if (checked_ && changes_ < 0xFFFF) ++changes_;
      unchanged_ = 0;
    }
    hash_ = hash;
    sample_ = sample;
    checked_ = true;
    return hash;
  }

 // Times the content was found changed (saturating).
  uint32_t changes() const { return changes_; }
  // Exact checks in a row that found the content unchanged (saturating), and the last hash.
  uint32_t unchanged() const { return unchanged_; }
  uint64_t last_hash() const { return hash_; }

 private:
  static constexpr size_t kSamples = 8;
  static constexpr size_t kSampleBytes = 64;

  static uint64_t Sample(const uint8_t* data, size_t size) {
    if (size <= kSamples * kSampleBytes * 2) {
      return XXH3_64bits(data, size);
    }
    uint64_t hash = size;
    size_t step = (size - kSampleBytes) / (kSamples - 1);
    for (size_t i = 0; i < kSamples; ++i) {
      hash = XXH3_64bits_withSeed(data + i * step, kSampleBytes, hash);
    }
    return hash;
  }

  // Data that changed before was once kept on full checks for 10 s after: it
  // made every texture a level loads into reused memory hash in full every
  // frame (7% of the ring thread), and the cutscene it was meant for turned
  // out to be decoded correctly into textures that do reach the renderer.
  uint32_t StableChecks() const { return kStableChecks; }
  uint32_t TrustedChecks() const { return kTrustedChecks; }

  uint64_t hash_ = 0;
  uint64_t sample_ = 0;
  uint32_t unchanged_ = 0;
  uint32_t changes_ = 0;
  bool checked_ = false;
};

// A hash map for 64-bit keys that are already hashes or unique numbers:
// open addressing with linear probing in one array. std::unordered_map's node
// per entry cost a cache miss or two per lookup, and the renderer does several
// lookups per draw. ~0 is reserved as the empty key. Growing moves the values,
// so references from operator[] last only until the next insertion.
template <typename V>
class FlatMap64 {
 public:
  static constexpr uint64_t kEmpty = ~uint64_t(0);

  const V* find(uint64_t key) const { return const_cast<FlatMap64*>(this)->find(key); }

  V* find(uint64_t key) {
    if (!capacity_) return nullptr;
    size_t i = Slot(key);
    while (true) {
      if (keys_[i] == key) return &values_[i];
      if (keys_[i] == kEmpty) return nullptr;
      i = (i + 1) & (capacity_ - 1);
    }
  }

  V& operator[](uint64_t key) {
    if ((size_ + 1) * 2 > capacity_) {
      Grow();
    }
    size_t i = Slot(key);
    while (true) {
      if (keys_[i] == key) return values_[i];
      if (keys_[i] == kEmpty) {
        keys_[i] = key;
        values_[i] = V();
        ++size_;
        return values_[i];
      }
      i = (i + 1) & (capacity_ - 1);
    }
  }

  size_t size() const { return size_; }

  // Calls f(value) for every entry (no insertions while it runs).
  template <typename F>
  void for_each_value(F&& f) {
    for (size_t i = 0; i < capacity_; ++i) {
      if (keys_[i] != kEmpty) f(values_[i]);
    }
  }

  void clear() {
    std::fill(keys_.begin(), keys_.end(), kEmpty);
    size_ = 0;
  }

  // Keeps only the entries for which keep(value) is true (rebuilds the table at the same capacity).
  template <typename F>
  void retain(F&& keep) {
    std::vector<uint64_t> old_keys = std::move(keys_);
    std::vector<V> old_values = std::move(values_);
    keys_.assign(old_keys.size(), kEmpty);
    values_.assign(old_keys.size(), V());
    size_ = 0;
    for (size_t i = 0; i < old_keys.size(); ++i) {
      if (old_keys[i] != kEmpty && keep(old_values[i])) (*this)[old_keys[i]] = std::move(old_values[i]);
    }
  }

 private:
  // The top bits of the product: a product bit depends only on the key bits
  // at or below it, so the middle bits used before ignored the high half of
  // keys like (address << 32 | size), and every vertex buffer of one size
  // probed the same chain.
  size_t Slot(uint64_t key) const {
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    return size_t(h >> shift_);
  }

  void Grow() {
    size_t new_capacity = capacity_ ? capacity_ * 2 : 1024;
    std::vector<uint64_t> old_keys = std::move(keys_);
    std::vector<V> old_values = std::move(values_);
    keys_.assign(new_capacity, kEmpty);
    values_.assign(new_capacity, V());
    capacity_ = new_capacity;
    shift_ = 64 - uint32_t(std::countr_zero(uint64_t(new_capacity)));
    size_ = 0;
    for (size_t i = 0; i < old_keys.size(); ++i) {
      if (old_keys[i] != kEmpty) {
        (*this)[old_keys[i]] = std::move(old_values[i]);
      }
    }
  }

  std::vector<uint64_t> keys_;
  std::vector<V> values_;
  size_t capacity_ = 0;
  size_t size_ = 0;
  uint32_t shift_ = 64;
};

}  // namespace p2::native
