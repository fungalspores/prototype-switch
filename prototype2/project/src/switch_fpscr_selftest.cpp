// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(__SWITCH__) && defined(__aarch64__)
#include "generated/default/prototype2_pch.h"
#include <rex/logging.h>
namespace {
double RoundUsingFPCR(double value) {
  double result;
  __asm__ __volatile__("frinti %d0, %d1" : "=w"(result) : "w"(value));
  return result;
}
}
bool prototype2_RunFPSCRSelfTest() {
  PPCFPSCRRegister fp{};
  const uint32_t saved = fp.getcsr();
  const double positive[] = {2.0, 1.0, 2.0, 1.0};
  const double negative[] = {-2.0, -1.0, -1.0, -2.0};
  bool ok = true;
  for (uint32_t mode = 0; mode < 4; ++mode) {
    fp.csr = saved;
    fp.storeFromGuest(mode);
    const auto actual = fp.loadFromHost();
    const auto raw = fp.getcsr();
    const double p = RoundUsingFPCR(1.5), n = RoundUsingFPCR(-1.5);
    if (actual != mode || (raw & ~uint32_t(fp.RoundMaskVal)) !=
                         (saved & ~uint32_t(fp.RoundMaskVal)) ||
        p != positive[mode] || n != negative[mode]) {
      ok = false;
      REXLOG_ERROR("[p2-fpscr-selftest] FAIL mode={} readback={} positive={} negative={}", mode, actual, p, n);
    }
  }
  fp.setcsr(saved);
  ok = ok && fp.getcsr() == saved;
  if (ok) REXLOG_INFO("[p2-fpscr-selftest] PASS 4 ARM64 modes: roundtrip, FRINTI +/-1.5, policy bits and FPCR restore");
  return ok;
}
#endif