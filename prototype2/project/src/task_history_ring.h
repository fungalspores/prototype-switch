#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
// Caller supplies synchronization. Fixed storage avoids allocations in guest task paths.
template<class T, size_t Capacity> class P2HistoryRing {
  std::array<T,Capacity> records_{};
  uint64_t count_=0;
public:
  void Push(T event) { event.sequence=++count_; records_[(count_-1)%Capacity]=event; }
  uint64_t Count() const { return count_; }
  template<class F> size_t Visit(uint32_t object,F callback) const {
    size_t found=0;
    const uint64_t first=count_>Capacity?count_-Capacity:0;
    for (uint64_t n=first;n<count_;++n) {
      const auto& event=records_[n%Capacity];
      if (event.object==object) { callback(event); ++found; }
    }
    return found;
  }
};
