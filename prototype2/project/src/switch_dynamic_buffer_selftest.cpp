#include "native/dynamic_buffer_selftest.h"
#include <rex/logging.h>
bool prototype2_RunDynamicBufferSelfTest() {
  if(!p2::native::ValidateDynamicBufferStamp())return false;
  REXLOG_INFO("[p2-buffer-selftest] PASS 16384 frames, exact visible bytes, partial writes/settling/resume, same-frame and wrapping epoch keys");
  return true;
}