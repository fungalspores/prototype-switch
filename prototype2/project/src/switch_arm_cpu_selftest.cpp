// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(__SWITCH__) && defined(__aarch64__)
#include "generated/default/prototype2_pch.h"
#include "arm_cpu_fixture_test.h"
#include <rex/logging.h>
bool prototype2_RunArmCpuSelfTest() {
 const bool ok=P2ArmCpuFixtures(256) && P2VmxIntegerFixtures(256);
 if(ok)REXLOG_INFO("[p2-arm-cpu-selftest] PASS actual matrix/stream context+memory,4rounding modes,all16alignments/aliases; native TBL2 and8192partial-store checks");
 else REXLOG_ERROR("[p2-arm-cpu-selftest] FAIL guest CPU lowering");
 return ok;
}
#endif