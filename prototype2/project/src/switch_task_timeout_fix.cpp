// SPDX-License-Identifier: GPL-3.0-or-later
#include "generated/default/prototype2_pch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
bool p2_task_pump_timeout_fix_enabled=false;
REXCVAR_DEFINE_BOOL(p2_task_pump_timeout_fix, false, "Prototype2", "Clear consumed task before the next wait so timeout cannot repeat its deleting destructor");
void prototype2_EnableTaskPumpTimeoutFix() {
  p2_task_pump_timeout_fix_enabled=REXCVAR_GET(p2_task_pump_timeout_fix);
  REXLOG_INFO("[p2-task-timeout] consumed-task pointer reset enabled={}",p2_task_pump_timeout_fix_enabled);
}
