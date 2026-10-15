// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
namespace p2::native {
struct BufferContentStamp {
  uint64_t hash = 0;
  uint64_t epoch = 0;
  bool frame_local = false;
};
// A frame-local copy is a distinct key domain, not a synthetic content hash.
// This prevents accidental reuse of an older copy even if content hash is zero.
struct BufferRangeKey {
  uint32_t address = 0;
  uint32_t size = 0;
  uint64_t hash = 0;
  uint64_t epoch = 0;
  bool frame_local = false;
  bool operator==(const BufferRangeKey& other) const {
    return address==other.address && size==other.size && hash==other.hash &&
           epoch==other.epoch && frame_local==other.frame_local;
  }
};
class DynamicBufferStamp {
 public:
  template<class ExactHash>
  BufferContentStamp Check(uint64_t frame, ExactHash&& hash) {
    if (frame_valid_ && frame_==frame) return stamp_;
    frame_valid_=true;frame_=frame;
    if (changes_>=4 && fresh_checks_<15) {
      ++fresh_checks_;
      stamp_={0,frame,true};
    } else {
      fresh_checks_=0;
      const uint64_t content=hash();
      changes_=content!=last_content_ ? std::min(changes_+1,8u) : 0;
      last_content_=content;
      stamp_={content,0,false};
    }
    return stamp_;
  }
 private:
  uint64_t frame_=0,last_content_=0;
  uint32_t changes_=0,fresh_checks_=0;
  bool frame_valid_=false;
  BufferContentStamp stamp_{};
};
}