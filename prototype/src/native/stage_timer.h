// Prototype (Switch): where the native renderer's ring thread spends a slow frame (cvar
// proto_stage_timer, off by default). The ring thread is the only user, so plain counters.
// p2_native_system.cpp calls StageTimerFrame() once per swap; frames over 50 ms (and every 300th
// frame for comparison) are written to logs/stage_timer.txt.
#pragma once

#include <chrono>
#include <cstdint>

namespace p2::stage {

enum Stage : uint32_t {
  kPackets = 0,   // the whole ring loop work (everything between idle waits)
  kRegisters,     // RegisterWritten bookkeeping
  kDrawTotal,     // Renderer::Draw
  kDrawSetup,     // render state lookup/build (start of Draw to "Indices")
  kDrawIndices,   // index data
  kDrawFetch,     // vertex fetch constants: vertex buffer ranges, hashes, copies
  kDrawFloat,     // float/bool constants
  kDrawTextures,  // textures and samplers (bindings)
  kDrawSystem,    // system constants
  kDrawRecord,    // pass begin and the draw command
  kFetchDirty,    // inside d.fetch: rewriting fetch words after register writes
  kFetchHash,     // inside d.fetch: RangeHash (change detection)
  kFetchCopy,     // inside d.fetch: FindRange / allocate / copy into the stream
  kPktType0,      // register writes (packet types 0 and 1)
  kPktSetConst,   // SET_CONSTANT / SET_SHADER_CONSTANTS / LOAD_ALU_CONSTANT
  kPktEvent,      // EVENT_WRITE* and VIZ_QUERY
  kPktMem,        // REG_RMW / REG_TO_MEM / MEM_WRITE / COND_WRITE
  kPktWaitMem,    // WAIT_REG_MEM (the spin itself)
  kPktIndirect,   // INDIRECT_BUFFER: nested packet streams
  kPktImLoad,     // IM_LOAD: shader microcode
  kPktHeader,     // packet header decode and advance
  kFetchScan,     // inside d.fetch: walking the shader's fetch slots and reading registers
  kFetchUpload,   // inside d.fetch: copying the fetch constant block into the stream
  kResolve,       // Renderer::Resolve
  kSwap,          // Renderer::Swap
  kOther,         // Packet work outside the above (computed)
  kCount
};

extern bool g_enabled;
extern uint64_t g_ns[kCount];
extern uint64_t g_count[kCount];

inline uint64_t Now() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// Times a scope into one stage.
struct Scope {
  Stage s;
  uint64_t t0;
  explicit Scope(Stage stage) : s(stage), t0(g_enabled ? Now() : 0) {}
  ~Scope() {
    if (t0) {
      g_ns[s] += Now() - t0;
      ++g_count[s];
    }
  }
};

// Successive marks inside one function: each Mark charges the time since the previous one.
struct Marks {
  uint64_t last;
  Marks() : last(g_enabled ? Now() : 0) {}
  void Mark(Stage s) {
    if (!last) return;
    const uint64_t now = Now();
    g_ns[s] += now - last;
    ++g_count[s];
    last = now;
  }
};

void StageTimerInit();
void StageTimerFrame(double frame_ms);

}  // namespace p2::stage
