// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
inline bool P2SkinningFinitePositions(const uint8_t* input, uint32_t count) {
  if (count < 2) return false;
  for (uint32_t i=0;i<count;++i) {
    uint32_t xyz[3]; std::memcpy(xyz,input+size_t(i)*32,12);
    for (uint32_t word : xyz)
      if ((__builtin_bswap32(word)&0x7F800000u)==0x7F800000u) return false;
  }
  return true;
}
