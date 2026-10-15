// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
namespace p2::native {
// Maps tiled byte offsets to the first grid origin visited by the original y/x scan.
// Stable sorting preserves that behavior even if a layout contains duplicate offsets.
class CompositeTileIndex {
 public:
  struct Origin { uint32_t offset, x, y; };
  template<class Offset> void Build(uint32_t pitch, uint32_t height, Offset offset) {
    entries_.clear();
    entries_.reserve(size_t((pitch+31)/32)*((height+31)/32));
    for (uint32_t y=0; y<height; y+=32)
      for (uint32_t x=0; x<pitch; x+=32) entries_.push_back({uint32_t(offset(x,y)),x,y});
    std::stable_sort(entries_.begin(), entries_.end(), [](const Origin& a,const Origin& b){return a.offset<b.offset;});
  }
  const Origin* Find(uint32_t offset) const {
    auto it=std::lower_bound(entries_.begin(),entries_.end(),offset,[](const Origin& a,uint32_t b){return a.offset<b;});
    return it!=entries_.end() && it->offset==offset ? &*it : nullptr;
  }
 private:
  std::vector<Origin> entries_;
};
}
