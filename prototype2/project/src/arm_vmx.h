#pragma once
#include <cstdint>
#include <cstring>
#include <simde/x86/sse4.1.h>
// Full-vector reversal in guest byte order; no FP operation or policy changes.
#if defined(P2_ARM_VMX) && P2_ARM_VMX && defined(__aarch64__)
#include <arm_neon.h>
inline simde__m128i P2VmxPermute(simde__m128i a,simde__m128i b,simde__m128i control) {
 const auto ap=simde__m128i_to_private(a), bp=simde__m128i_to_private(b), cp=simde__m128i_to_private(control);
 uint8x16x2_t table{{ap.neon_u8,bp.neon_u8}};
 simde__m128i_private result{};
 result.neon_u8=vqtbl2q_u8(table,veorq_u8(vandq_u8(cp.neon_u8,vdupq_n_u8(31)),vdupq_n_u8(15)));
 return simde__m128i_from_private(result);
}
#define P2_VMX_PERMUTE(a,b,c) P2VmxPermute(a,b,c)
#else
#define P2_VMX_PERMUTE(a,b,c) rex::ppc::simde_mm_perm_epi8_(a,b,c)
#endif
#if (defined(P2_ARM_VMX) && P2_ARM_VMX && defined(__aarch64__)) || defined(P2_VMX_HOST_TEST)
struct __attribute__((packed,may_alias)) P2U64 { uint64_t value; };
struct __attribute__((packed,may_alias)) P2U32 { uint32_t value; };
struct __attribute__((packed,may_alias)) P2U16 { uint16_t value; };
inline void P2VmxStoreExact(uint8_t* destination,const uint8_t* reversed,uint32_t count) {
 if (count==16) {
#if defined(__aarch64__)
  auto data=vld1q_u8(reversed);
  __asm__ volatile("str %q1, [%0]"::"r"(destination),"w"(data):"memory");
#else
  std::memcpy(destination,reversed,16);
#endif
  return;
 }
 if(count>=8){uint64_t value;std::memcpy(&value,reversed,8);reinterpret_cast<volatile P2U64*>(destination)->value=value;destination+=8;reversed+=8;count-=8;}
 if(count>=4){uint32_t value;std::memcpy(&value,reversed,4);reinterpret_cast<volatile P2U32*>(destination)->value=value;destination+=4;reversed+=4;count-=4;}
 if(count>=2){uint16_t value;std::memcpy(&value,reversed,2);reinterpret_cast<volatile P2U16*>(destination)->value=value;destination+=2;reversed+=2;count-=2;}
 if(count)*reinterpret_cast<volatile uint8_t*>(destination)=*reversed;
}
inline void P2VmxReverse(uint8_t* reversed,const uint8_t* vector) {
#if defined(__aarch64__)
 auto v=vrev64q_u8(vld1q_u8(vector));vst1q_u8(reversed,vextq_u8(v,v,8));
#else
 for(unsigned i=0;i<16;++i) reversed[i]=vector[15-i];
#endif
}
inline void P2VmxStoreLeft(uint8_t* base,uint32_t address,const uint8_t* vector) {
 alignas(16) uint8_t reversed[16];P2VmxReverse(reversed,vector);
 P2VmxStoreExact(base+address,reversed,16-(address&15));
}
inline void P2VmxStoreRight(uint8_t* base,uint32_t address,const uint8_t* vector) {
 const uint32_t count=address&15;if(!count)return;
 alignas(16) uint8_t reversed[16];P2VmxReverse(reversed,vector);
 P2VmxStoreExact(base+address-count,reversed+16-count,count);
}
#define P2_VMX_STORE_LEFT(base,address,vector) P2VmxStoreLeft(base,address,vector)
#define P2_VMX_STORE_RIGHT(base,address,vector) P2VmxStoreRight(base,address,vector)
#else
#define P2_VMX_STORE_LEFT(base,address,vector) do { for(size_t p2_i=0;p2_i<16-((address)&15);++p2_i) *(volatile uint8_t*)((base)+uint32_t((address)+p2_i)+REX_PHYS_HOST_OFFSET((address)+p2_i))=(vector)[15-p2_i]; } while(0)
#define P2_VMX_STORE_RIGHT(base,address,vector) do { for(size_t p2_i=0;p2_i<((address)&15);++p2_i) *(volatile uint8_t*)((base)+uint32_t((address)-p2_i-1)+REX_PHYS_HOST_OFFSET((address)-p2_i-1))=(vector)[p2_i]; } while(0)
#endif