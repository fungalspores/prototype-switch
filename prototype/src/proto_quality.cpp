// Prototype (Switch): internal render size (tools/prepare_proto_codegen.py routes the game's back-buffer size
// from sub_82A0E118 here). The engine sizes its scene targets from the back buffer; the presenter scales it to
// the screen (present_effect = fsr). Rounded down to the Xbox 360 EDRAM tile grid (80 x 16 pixels).
#include <algorithm>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(proto_render_width, 1280, "Prototype",
                      "Internal render width (the game's own 1280). Lower = fewer pixels for the GPU; scaled to "
                      "the screen with present_effect. Multiple of 80; restart")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(proto_render_height, 720, "Prototype",
                      "Internal render height (the game's own 720). Multiple of 16; restart")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" int64_t proto_render_height() {
  return std::clamp<uint32_t>(REXCVAR_GET(proto_render_height), 352, 720) / 16 * 16;
}

extern "C" int64_t proto_render_width() {
  const uint32_t w = std::clamp<uint32_t>(REXCVAR_GET(proto_render_width), 640, 1280) / 80 * 80;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("[proto-quality] internal render size {}x{} (game 1280x720)", w, proto_render_height());
  }
  return w;
}
