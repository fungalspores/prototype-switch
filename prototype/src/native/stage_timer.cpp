#include "stage_timer.h"

#include <cstdio>
#include <cstring>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(proto_stage_timer, false, "Prototype",
                    "Diagnostic: time the native renderer's ring thread by stage and write slow frames "
                    "(over 50 ms) to logs/stage_timer.txt. Restart required")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace p2::stage {

bool g_enabled = false;
uint64_t g_ns[kCount] = {};
uint64_t g_count[kCount] = {};

namespace {
const char* const kNames[kCount] = {"packets", "registers", "draw",     "d.setup", "d.indices",
                                    "d.fetch", "d.float",   "d.textures", "d.system", "d.record", "f.dirty", "f.hash", "f.copy", "p.reg", "p.const", "p.event", "p.mem", "p.waitmem", "p.indirect", "p.imload", "p.header", "f.scan", "f.upload",
                                    "resolve", "swap",      "other"};
FILE* g_file = nullptr;
uint64_t g_frame = 0;
uint64_t g_sum_ns[kCount] = {};  // slow frames, summed
uint64_t g_slow = 0;
}  // namespace

void StageTimerInit() {
  g_enabled = REXCVAR_GET(proto_stage_timer);
  if (!g_enabled) return;
  g_file = std::fopen("sdmc:/switch/prototype-nx/logs/stage_timer.txt", "a");
  if (g_file) {
    static char buffer[64 * 1024];
    std::setvbuf(g_file, buffer, _IOFBF, sizeof(buffer));
    std::fprintf(g_file, "start\n");
  }
}

void StageTimerFrame(double frame_ms) {
  if (!g_enabled) return;
  ++g_frame;
  // Work not inside a named stage: the ring loop total minus the named top-level stages.
  const uint64_t named = g_ns[kRegisters] + g_ns[kDrawTotal] + g_ns[kResolve] + g_ns[kSwap];
  g_ns[kOther] = g_ns[kPackets] > named ? g_ns[kPackets] - named : 0;
  const bool slow = frame_ms > 50.0;
  if (slow) {
    ++g_slow;
    for (uint32_t i = 0; i < kCount; ++i) g_sum_ns[i] += g_ns[i];
  }
  if (g_file && (slow || g_frame % 300 == 0)) {
    std::fprintf(g_file, "%s %llu %.1f", slow ? "S" : "N", static_cast<unsigned long long>(g_frame),
                 frame_ms);
    for (uint32_t i = 0; i < kCount; ++i) {
      std::fprintf(g_file, " %s=%.2f/%llu", kNames[i], g_ns[i] / 1e6,
                   static_cast<unsigned long long>(g_count[i]));
    }
    std::fprintf(g_file, "\n");
  }
  if (g_file && g_frame % 1800 == 0) {
    std::fprintf(g_file, "TOTAL frames=%llu slow=%llu", static_cast<unsigned long long>(g_frame),
                 static_cast<unsigned long long>(g_slow));
    for (uint32_t i = 0; i < kCount; ++i) {
      std::fprintf(g_file, " %s=%.2f", kNames[i], g_slow ? g_sum_ns[i] / 1e6 / double(g_slow) : 0.0);
    }
    std::fprintf(g_file, "\n");
    std::fflush(g_file);
  }
  std::memset(g_ns, 0, sizeof(g_ns));
  std::memset(g_count, 0, sizeof(g_count));
}

}  // namespace p2::stage
