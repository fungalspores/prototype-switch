#pragma once
#include <cstdint>
extern bool p2_guest_stack_audit_enabled;
void P2ReportGuestStackLeak(const char* function, void* context, uint32_t before,
                           uint32_t after, uint32_t caller_lr, uint32_t return_lr);
// Diagnostic only. Does not repair a guest frame or alter guest registers.
class P2GuestStackScope {
  const char* name_;
  void* context_;
  uint64_t& sp_;
  uint64_t& lr_;
  uint32_t before_, caller_;
public:
  P2GuestStackScope(const char* name, uint64_t& sp, uint64_t& lr, void* context)
      :name_(p2_guest_stack_audit_enabled?name:nullptr),context_(context),sp_(sp),lr_(lr),
       before_(name_?uint32_t(sp):0),caller_(name_?uint32_t(lr):0) {}
  ~P2GuestStackScope() {
    // Positive changes can be legitimate tail epilogues of split fragments.
    // A negative delta locates an allocated frame that survived a C++ return.
    if (name_ && int32_t(uint32_t(sp_)-before_)<0)
      P2ReportGuestStackLeak(name_,context_,before_,uint32_t(sp_),caller_,uint32_t(lr_));
  }
  P2GuestStackScope(const P2GuestStackScope&)=delete;
  P2GuestStackScope& operator=(const P2GuestStackScope&)=delete;
};
