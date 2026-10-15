// Prototype (Switch): runtime switches for the generated-code speed patches (tools/prepare_proto_codegen.py).
#include "generated/default/prototype_pch.h"

#include <algorithm>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(proto_msr_lock_mode, 1, "Prototype",
                      "Guest interrupt-disabled atomic regions (lwarx/stwcx.): 0 = no lock, 1 = one lock per "
                      "address stripe, 2 = the SDK's single global mutex (original, slowest)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

void proto_ApplySpeedSettings() {
  rex_proto_msr_lock = int(std::min<uint32_t>(REXCVAR_GET(proto_msr_lock_mode), 2u));
  REXLOG_INFO("[proto-speed] guest msr regions: {}",
              rex_proto_msr_lock == 2 ? "SDK global mutex" : rex_proto_msr_lock == 1 ? "lock per address" : "lock-free");
}
