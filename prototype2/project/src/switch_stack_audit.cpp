// SPDX-License-Identifier: GPL-3.0-or-later
#include "guest_stack_audit.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <atomic>
bool p2_guest_stack_audit_enabled=false;
REXCVAR_DEFINE_BOOL(p2_guest_stack_audit, false, "Prototype2", "Diagnostic build: identify first generated function returning with an allocated guest stack frame");
namespace { std::atomic<uint64_t> leaked_frames{0}; }
void prototype2_EnableGuestStackAudit() {
  p2_guest_stack_audit_enabled=REXCVAR_GET(p2_guest_stack_audit);
#if defined(P2_GUEST_STACK_AUDIT) && P2_GUEST_STACK_AUDIT
  REXLOG_INFO("[p2-stack] instrumentation={} enabled={} ; diagnostic overhead, not an FPS benchmark",P2_GUEST_STACK_AUDIT,p2_guest_stack_audit_enabled);
#else
  if (p2_guest_stack_audit_enabled) {
    REXLOG_ERROR("[p2-stack] requested but this build has no generated-function instrumentation");
    p2_guest_stack_audit_enabled=false;
  }
#endif
}
void P2ReportGuestStackLeak(const char* function, void* context, uint32_t before,
                           uint32_t after, uint32_t caller_lr, uint32_t return_lr) {
  const auto n=leaked_frames.fetch_add(1,std::memory_order_relaxed)+1;
  if (n<=64)
    REXLOG_ERROR("[p2-stack] LEAK #{} function={} ctx={} sp={:08X}->{:08X} delta={} caller_lr={:08X} return_lr={:08X}",n,function,context,before,after,int32_t(after-before),caller_lr,return_lr);
}
