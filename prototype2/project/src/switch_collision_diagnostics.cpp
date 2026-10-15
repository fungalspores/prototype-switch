// SPDX-License-Identifier: GPL-3.0-or-later
#include "generated/default/prototype2_pch.h"
#include <atomic>
#include <array>
#include <string_view>
#ifndef P2_COLLISION_DIAGNOSTICS_TEST
#include <rex/cvar.h>
#include <rex/logging.h>
REXCVAR_DEFINE_BOOL(p2_collision_diagnostics, false, "Prototype2", "Expose bounded original physics/streaming diagnostics without changing guest behavior");
#endif
namespace {
bool P2ContainsAscii(std::string_view text, std::string_view needle) {
  auto lower=[](unsigned char c){return c>='A' && c<='Z' ? c+('a'-'A') : c;};
  for(size_t i=0;i+needle.size()<=text.size();++i){
    size_t j=0;for(;j<needle.size();++j)if(lower(text[i+j])!=lower(needle[j]))break;
    if(j==needle.size())return true;
  }
  return false;
}
template<size_t N>
void P2ImageString(uint8_t* base,uint32_t address,std::array<char,N>& result) {
  result.fill(0);
  if(address<REX_IMAGE_BASE || uint64_t(address)>=REX_IMAGE_BASE+REX_IMAGE_SIZE)return;
  for(size_t i=0;i+1<N && uint64_t(address)+i<REX_IMAGE_BASE+REX_IMAGE_SIZE;++i){
    result[i]=char(REX_LOAD_U8(address+uint32_t(i)));
    if(!result[i])break;
  }
}
std::atomic<uint32_t> p2_collision_messages{0};
std::atomic<uint32_t> p2_collision_errors{0};
}
REX_EXTERN(__imp__sub_822E91C8);
extern "C" REX_FUNC(sub_822E91C8) {
  if(!REXCVAR_GET(p2_collision_diagnostics)) { __imp__sub_822E91C8(ctx,base); return; }
  const uint32_t sp=ctx.r1.u32,caller=uint32_t(ctx.lr),severity=ctx.r3.u32;
  const uint32_t file=ctx.r5.u32,line=ctx.r6.u32,format=ctx.r8.u32;
  // The original writes a 1024-byte formatted message at incoming SP-1120+80,
  // restores its guest frame and returns 2. Preserve all guest execution once.
  __imp__sub_822E91C8(ctx,base);
  std::array<char,256> source{},pattern{};
  P2ImageString(base,file,source);P2ImageString(base,format,pattern);
  const std::string_view path(source.data()),fmt(pattern.data());
  const bool physics=P2ContainsAscii(path,"ravenphysics") || P2ContainsAscii(path,"physics") ||
      P2ContainsAscii(fmt,"collision") || P2ContainsAscii(fmt,"narrowphase");
  const bool streaming=P2ContainsAscii(path,"stream") || P2ContainsAscii(path,"content") ||
      P2ContainsAscii(fmt,"cell") || P2ContainsAscii(fmt,"cement");
  if(severity!=1 && !physics && !streaming)return;
  auto& counter=severity==1 ? p2_collision_errors : p2_collision_messages;
  const uint32_t count=counter.fetch_add(1,std::memory_order_relaxed);
  constexpr uint32_t limit=128;
  if(count>=limit)return;
  // Only inspect the buffer after the original has returned through its formatting
  // path. It just wrote this stack region. Do not follow arbitrary object pointers.
  if(sp<1040 || uint64_t(sp)-1040+1024>0x100000000ull)return;
  std::array<char,1024> message{};
  for(uint32_t i=0;i<1023;++i){message[i]=char(REX_LOAD_U8(sp-1040+i));if(!message[i])break;}
  REXLOG_WARN("[p2-collision] kind={} physics={} streaming={} caller={:08X} file={} line={} message={}",
      severity,physics,streaming,caller,source.data(),line,message.data());
  if(count+1==limit)REXLOG_WARN("[p2-collision] {} message limit reached; later messages suppressed",severity==1 ? "error" : "info");
}