// Shared host and Switch startup fixtures. No game asset access.
#pragma once
#include <array>
#include <cstring>
#include <cstdint>
#include "arm_cpu_fixtures.inc"
inline bool P2ArmCpuFixtures(unsigned iterations) {
 uint32_t rng=0x415608A7;auto random=[&](){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;};
 alignas(32) std::array<uint8_t,4096> before{},after{};
 PPCFPSCRRegister host{};const uint32_t saved=host.getcsr();
 auto put=[&](uint32_t address,uint32_t value){value=__builtin_bswap32(value);std::memcpy(before.data()+address,&value,4);};
 for(unsigned test=0;test<iterations;++test) {
  PPCContext original{};for(size_t i=0;i<sizeof(original);++i)reinterpret_cast<uint8_t*>(&original)[i]=uint8_t(random());
  for(auto& x:before)x=uint8_t(random());
  original.fpscr={};original.fpscr.csr=saved;original.fpscr.storeFromGuest(test&3);
  original.r3.u64=0x100;original.r4.u64=0x200+(test%16);original.r5.u64=0x300+((test/16)%16);
  for(unsigned j=0;j<16;++j){float f=float(int(random()%65535)-32767)/128.0f;uint32_t bits;std::memcpy(&bits,&f,4);put(0x100+j*4,bits);}
  for(unsigned j=0;j<4;++j){float f=float(int(random()%65535)-32767)/64.0f;uint32_t bits;std::memcpy(&bits,&f,4);put(original.r4.u32+j*4,bits);}
  // Explicit overlapping output/input cases, in addition to every alignment.
  if(test%7==0)original.r5.u64=original.r4.u64;
  if(test%11==0)original.r5.u64=original.r3.u64+16;
  after=before;auto optimized=original;const uint32_t csr=host.getcsr();
  p2_reference_matrix(original,before.data());host.setcsr(csr);p2_optimized_matrix(optimized,after.data());
  if(std::memcmp(&original,&optimized,sizeof(original)) || before!=after){host.setcsr(saved);return false;}
  // Exact partial store inside the sampled real guest function829FF6A8.
  original=optimized;original.r3.u64=0x100;put(0x100,test%16);put(0x104,0x400);put(0x420,0x500+(test%16));
  after=before;optimized=original;
  p2_reference_stream(original,before.data());p2_optimized_stream(optimized,after.data());
  if(std::memcmp(&original,&optimized,sizeof(original)) || before!=after){host.setcsr(saved);return false;}
 }
 host.setcsr(saved);return true;
}
inline bool P2VmxIntegerFixtures(unsigned iterations) {
 uint32_t rng=0xdeadbeef;auto random=[&](){rng=rng*1664525u+1013904223u;return uint8_t(rng>>24);};
 alignas(32) uint8_t a[16],b[16],c[16],result[16];
 for(unsigned test=0;test<iterations;++test) {
  for(unsigned i=0;i<16;++i){a[i]=random();b[i]=random();c[i]=uint8_t(test+i*17);}
  auto actual=P2_VMX_PERMUTE(simde_mm_load_si128((simde__m128i*)a),simde_mm_load_si128((simde__m128i*)b),simde_mm_load_si128((simde__m128i*)c));
  simde_mm_store_si128((simde__m128i*)result,actual);
  for(unsigned i=0;i<16;++i)if(result[i]!=((c[i]&16)?b:a)[15-(c[i]&15)])return false;
  for(uint32_t offset=0;offset<16;++offset){
   alignas(32) uint8_t reference[96],optimized[96];for(auto& x:reference)x=random();std::memcpy(optimized,reference,96);
   const uint32_t address=32+offset;for(uint32_t i=0;i<16-(address&15);++i)reference[address+i]=a[15-i];
   P2_VMX_STORE_LEFT(optimized,address,a);if(std::memcmp(reference,optimized,96))return false;
   for(uint32_t i=0;i<(address&15);++i)reference[address-i-1]=b[i];
   P2_VMX_STORE_RIGHT(optimized,address,b);if(std::memcmp(reference,optimized,96))return false;
  }
 }
 return true;
}