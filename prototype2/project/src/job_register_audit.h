#pragma once
#include <cstdint>

// ABI required by the actual savegprlr_26/restgprlr_26 pair in sub_822BAA78.
struct P2JobRegisters {
  uint64_t sp;
  uint32_t lr, backchain;
  uint64_t nonvolatile[6];
  template<class Context>
  static P2JobRegisters Capture(const Context& c, uint32_t chain) {
    return {c.r1.u64, uint32_t(c.lr), chain,
            {c.r26.u64,c.r27.u64,c.r28.u64,c.r29.u64,c.r30.u64,c.r31.u64}};
  }
  uint32_t Difference(const P2JobRegisters& after) const {
    uint32_t bits=0;
    for (unsigned i=0;i<6;++i) if (nonvolatile[i]!=after.nonvolatile[i]) bits|=1u<<i;
    if (sp!=after.sp) bits|=1u<<6;
    if (lr!=after.lr) bits|=1u<<7;
    if (backchain!=after.backchain) bits|=1u<<8;
    return bits;
  }
};
