// Symbols the Prototype 2 native renderer expects from Prototype 2 specific modules
// (switch_skinning.cpp). Prototype 1 has no skinning hook, so the write log stays empty
// and the renderer falls back to its content checks for skinned vertex buffers.
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(p2_skin_write_log, false, "Prototype",
                    "Record skinning writes for the native renderer (no skinning hook in Prototype 1)");
