// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>
namespace p2::native {
// Ring-thread only. A guest query may span several fenced GPU submissions.
// A record cannot be reused until every piece has completed and its end arrived.
template <std::size_t N> class OcclusionTracker {
 public:
  static constexpr uint32_t invalid = UINT32_MAX;
  struct Result { uint32_t address=0,samples=0;bool fallback=false,superseded=false; };
  uint32_t active() const {return active_;}
  bool pending() const {return used_!=0;}
  uint32_t address(uint32_t id) const {return records_[id].address;}
  static constexpr uint32_t max_active = 16;
  struct Owners { uint32_t count=0; std::array<uint32_t,max_active> ids{}; };
  uint32_t FindBegin(uint32_t address) const {
    for(uint32_t i=0;i<open_.count;++i)if(records_[open_.ids[i]].begin_address==address)return open_.ids[i];
    return invalid;
  }
  bool IsEndAddress(uint32_t address) const {return FindBegin(address+32u)!=invalid;}
  bool IsEnded(uint32_t id) const {return id<N && records_[id].valid && records_[id].ended;}
  bool EndedPending() const {
    for(const auto& r:records_)if(r.valid && r.ended && r.pieces)return true;
    return false;
  }
  uint32_t open_count() const {return open_.count;}
  bool Begin(uint32_t address) {
    if(active_!=invalid)return false;
    return BeginOverlapping(address);
  }
  bool BeginOverlapping(uint32_t address) {
    if(open_.count==max_active || FindBegin(address)!=invalid)return false;
    for(uint32_t i=0;i<N;++i){uint32_t id=(cursor_+i)%N;if(records_[id].valid)continue;
      for(auto& old:records_)if(old.valid && old.begin_address==address)old.superseded=true;
      records_[id]={address,0,0,true,false,false,++issued_,address};open_.ids[open_.count++]=id;active_=id;cursor_=(id+1)%N;++used_;return true;}
    return false;
  }
  Owners OpenOwners() const {
    Owners owners;
    for(uint32_t i=0;i<open_.count;++i) {
      const uint32_t id=open_.ids[i];
      if(!records_[id].fallback)owners.ids[owners.count++]=id;
    }
    return owners;
  }
  Owners AddOpenPiece() {
    Owners owners;
    for(uint32_t i=0;i<open_.count;++i){
      const uint32_t id=open_.ids[i];auto& record=records_[id];
      if(record.fallback)continue;
      AddPiece(id);owners.ids[owners.count++]=id;
    }
    return owners;
  }
  bool AddPiece(uint32_t id) {
    if(id>=N || !records_[id].valid || records_[id].ended)return false;
    ++records_[id].pieces;return true;
  }
  bool Complete(uint32_t id,uint64_t samples,bool success=true) {
    if(id>=N || !records_[id].valid || !records_[id].pieces)return false;
    auto& r=records_[id];--r.pieces;r.fallback|=!success;
    r.samples=uint32_t(std::min<uint64_t>(uint64_t(r.samples)+std::min<uint64_t>(samples,UINT32_MAX),UINT32_MAX));return true;
  }
  uint32_t End(uint32_t address) {
    if(active_==invalid)return invalid;
    return EndRecord(active_,address);
  }
  uint32_t EndMatching(uint32_t address){return EndRecord(FindBegin(address+32u),address);}
  uint32_t EndRecord(uint32_t id,uint32_t address) {
    if(id>=N || !records_[id].valid || records_[id].ended)return invalid;
    records_[id].address=address;records_[id].ended=true;
    for(uint32_t i=0;i<open_.count;++i)if(open_.ids[i]==id){
      for(uint32_t j=i+1;j<open_.count;++j)open_.ids[j-1]=open_.ids[j];--open_.count;break;
    }
    active_=open_.count?open_.ids[open_.count-1]:invalid;return id;
  }
  void FailRecord(uint32_t id){if(id<N && records_[id].valid)records_[id].fallback=true;}
  void FailOpen(){for(uint32_t i=0;i<open_.count;++i)records_[open_.ids[i]].fallback=true;}
  void FailActive(){if(active_!=invalid)records_[active_].fallback=true;}
  uint32_t CancelActive(){if(active_==invalid)return invalid;FailActive();return End(records_[active_].address);}
  bool TakeReady(uint32_t id,Result& result){
    if(id>=N)return false;auto& r=records_[id];if(!r.valid || !r.ended || r.pieces)return false;
    bool superseded=r.superseded;
    for(const auto& other:records_)if(other.valid && other.serial>r.serial && (other.ended?other.address:other.begin_address-32u)==r.address){superseded=true;break;}
    result={r.address,r.samples,r.fallback,superseded};r.valid=false;--used_;return true;
  }
 private:
  struct Record {uint32_t address=0,samples=0,pieces=0;bool valid=false,ended=false,fallback=false;uint64_t serial=0;uint32_t begin_address=0;bool superseded=false;};
  std::array<Record,N> records_{};uint32_t active_=invalid,cursor_=0,used_=0;uint64_t issued_=0;Owners open_{};
};
}
