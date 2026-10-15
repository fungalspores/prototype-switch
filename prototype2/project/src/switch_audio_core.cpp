// Prototype 2 (Switch, WORKLOG #120): the game's audio engine thread on core 3.
// run86: the thread entering sub_82CC1850 (the game's audio engine: mixes, feeds the XMA decoder through its
// registers; sub_82CF0958 is its DSP) used ~25 % of a core with mask 0xF, time-sliced against the job workers on
// cores 0-2 that the Main thread waits for every frame. Core 3 already runs the XMA decoder and the skinning helper
// and had room. 0 = leave the mask as the SDK set it.
#include "generated/default/prototype2_pch.h"
#include <cstdint>
#include <rex/cvar.h>
#include <rex/logging.h>

extern "C" bool RexSwitchSetCurrentThreadMask(uint32_t mask);  // SDK threading_switch
extern "C" void RexSwitchSetCurrentThreadPriority(int priority);

REXCVAR_DEFINE_UINT32(p2_audio_engine_mask, 0x8, "Prototype2",
                      "Core mask for the game's audio engine thread (sub_82CC1850; 0x8 = core 3, 0 = unchanged)");

REXCVAR_DEFINE_UINT32(p2_audio_engine_priority, 0, "Prototype2",
                      "Horizon priority for the audio engine thread (0 = unchanged; run90: on core 3 at 0x3B it was "
                      "preempted by the XMA decoder 0x2B and the skinning helper 0x2A -> crackling)");

REX_EXTERN(__imp__sub_82CC1850);
extern "C" REX_FUNC(sub_82CC1850) {
  static thread_local bool done = false;  // the thread's loop; set once per thread
  const uint32_t mask = REXCVAR_GET(p2_audio_engine_mask) & 0xFu;
  const uint32_t priority = REXCVAR_GET(p2_audio_engine_priority);
  if (priority && !done) RexSwitchSetCurrentThreadPriority(int(priority));
  if (mask && !done) {
    const bool ok = RexSwitchSetCurrentThreadMask(mask);
    REXLOG_INFO("[p2-audio] audio engine thread core mask 0x{:X}: {}, priority 0x{:X}", mask, ok ? "set" : "refused",
                priority);
  }
  done = true;
  __imp__sub_82CC1850(ctx, base);
}
