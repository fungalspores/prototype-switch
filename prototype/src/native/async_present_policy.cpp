// SPDX-License-Identifier: GPL-3.0-or-later
#include <rex/cvar.h>
#include "p2_native_system.h"
REXCVAR_DEFINE_BOOL(p2_native_async_present, false, "Prototype2",
                   "Ordered asynchronous native presentation; false restores synchronous output. Restart the game after changing.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
extern "C" bool RexOrderedGuestPresentRequested() {
  return p2::native::Enabled() && REXCVAR_GET(p2_native_async_present);
}
