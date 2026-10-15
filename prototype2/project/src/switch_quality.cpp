// SPDX-License-Identifier: GPL-3.0-or-later
// Prototype 2: engine-level quality cuts through the game's own tunables (WORKLOG #90), the CoD4-port idea
// ("lower the engine's dvars") applied to what P2's script API exposes:
// - ambient manager (its init sub_8247A288 registers amb_SetMaxPedestrians etc. with the manager as `this`):
//   +0 = max pedestrians, +4 = min pedestrians (sub_824783C8 / sub_82478388), +1664 = max moving vehicles
//   (sub_82478408). Fewer pedestrians / cars = less AI, animation, skinning (28 % of a core) and draws.
// - rm_SetGeoLODBias (sub_824C18B8) writes the float at 0x82DC1E6C (image value 1.0) that multiplies the
//   distance metric of the mesh LOD choice (sub_8221Fxxx): larger = coarser LODs sooner.
// The game keeps writing its own values (scripts per district / mission): every frame the current value is
// compared with the one last written here; a different one is the game's new value, stored and replaced by the
// scaled one. The cvars apply live (settings overlay); 1.0 = the game's values.
#include "generated/default/prototype2_pch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

REXCVAR_DEFINE_DOUBLE(p2_ped_max_scale, 1.0, "Prototype2",
                      "Scale of the game's maximum number of ambient pedestrians (1.0 = the game's own)");
REXCVAR_DEFINE_DOUBLE(p2_vehicle_max_scale, 1.0, "Prototype2",
                      "Scale of the game's maximum number of moving ambient vehicles (1.0 = the game's own)");
REXCVAR_DEFINE_DOUBLE(p2_geo_lod_bias_scale, 1.0, "Prototype2",
                      "Multiplier of the game's mesh LOD bias (rm_SetGeoLODBias): above 1 = lower-detail "
                      "meshes nearer the camera (1.0 = the game's own)");
REXCVAR_DEFINE_DOUBLE(p2_shadow_range_scale, 1.0, "Prototype2",
                      "WORKLOG #103: multiplier of the game's shadow range (rm_SetGlobalShadowRangeHack, float at "
                      "0x82DC5864, 300 m in the image): below 1 = shadows of far objects not drawn (fewer shadow-map "
                      "draws for the ring thread and the GPU). 1.0 = the game's own");
REX_EXTERN(__imp__sub_8247A288);

namespace {
std::atomic<uint8_t*> g_base{nullptr};
std::atomic<uint32_t> g_ambient{0};

inline uint32_t Load32(uint8_t* base, uint32_t a) { uint32_t v; std::memcpy(&v, base + a, 4); return __builtin_bswap32(v); }
inline void Store32(uint8_t* base, uint32_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(base + a, &v, 4); }

struct Enforced {
  const char* name;
  bool have = false;
  uint32_t game = 0, written = 0;
  // Returns true when `cur` is a new game value (logged by the caller).
  bool Track(uint32_t cur) {
    if (have && (cur == written || cur == game)) return false;
    game = cur;
    have = true;
    return true;
  }
};
Enforced g_peds{"max pedestrians"}, g_cars{"max moving vehicles"}, g_lod{"geo LOD bias"},
    g_shadow{"shadow range"};

void ScaleFloat(uint8_t* base, uint32_t address, Enforced& e, double scale) {
  const uint32_t cur = Load32(base, address);
  float game;
  if (e.Track(cur)) {
    std::memcpy(&game, &e.game, 4);
    REXLOG_INFO("[p2-quality] {}: game {} x {}", e.name, game, scale);
  }
  std::memcpy(&game, &e.game, 4);
  const float want = float(double(game) * scale);
  std::memcpy(&e.written, &want, 4);
  if (cur != e.written) Store32(base, address, e.written);
}

uint32_t ScaleCount(uint32_t v, double s) {
  if (s >= 1.0 || int32_t(v) <= 0) return v;
  return uint32_t(std::max(1.0, std::ceil(double(v) * std::max(s, 0.0))));
}
}  // namespace

extern "C" REX_FUNC(sub_8247A288) {
  g_base.store(base, std::memory_order_relaxed);
  g_ambient.store(ctx.r3.u32, std::memory_order_release);
  REXLOG_INFO("[p2-quality] ambient manager at {:08X}", ctx.r3.u32);
  __imp__sub_8247A288(ctx, base);
}

namespace {
std::mutex g_apply_lock;
void ApplyAmbient(uint8_t* base, uint32_t m) {
  std::unique_lock<std::mutex> lock(g_apply_lock, std::try_to_lock);  // sim thread and flip thread
  if (!lock) return;
  // Sanity (the setters cap the counts by the pools: +468..+476 pedestrian slots, +1332..+1336 vehicles).
  const uint32_t ped_pool = m ? (Load32(base, m + 476) - Load32(base, m + 468)) / 4 : 0;
  const uint32_t car_pool = m ? (Load32(base, m + 1336) - Load32(base, m + 1332)) / 4 : 0;
  if (m && ped_pool - 1 < 1024 && car_pool - 1 < 1024 && Load32(base, m + 0) <= ped_pool &&
      Load32(base, m + 1664) <= car_pool) {
    const uint32_t peds = Load32(base, m + 0);
    if (g_peds.Track(peds)) {
      g_peds.written = ScaleCount(peds, REXCVAR_GET(p2_ped_max_scale));
      REXLOG_INFO("[p2-quality] {}: game {} -> {}", g_peds.name, peds, g_peds.written);
    }
    // Follow live cvar changes too.
    const uint32_t want_peds = ScaleCount(g_peds.game, REXCVAR_GET(p2_ped_max_scale));
    if (want_peds != g_peds.written) g_peds.written = want_peds;
    if (peds != g_peds.written) Store32(base, m + 0, g_peds.written);
    if (int32_t(Load32(base, m + 4)) > int32_t(g_peds.written)) Store32(base, m + 4, g_peds.written);

    const uint32_t cars = Load32(base, m + 1664);
    if (g_cars.Track(cars)) {
      g_cars.written = ScaleCount(cars, REXCVAR_GET(p2_vehicle_max_scale));
      REXLOG_INFO("[p2-quality] {}: game {} -> {}", g_cars.name, cars, g_cars.written);
    }
    const uint32_t want_cars = ScaleCount(g_cars.game, REXCVAR_GET(p2_vehicle_max_scale));
    if (want_cars != g_cars.written) g_cars.written = want_cars;
    if (cars != g_cars.written) Store32(base, m + 1664, g_cars.written);
  }
}
}  // namespace

// run62: the ambient manager's per-frame population blend sub_821F9BF0 (this = manager) rewrites the moving
// vehicle cap (+1664, and the area densities +1644..+1672) from the population volumes every frame, so the
// cap written at the flip was back at the game's value whenever the spawner looked. Applied right after it.
REX_EXTERN(__imp__sub_821F9BF0);
extern "C" REX_FUNC(sub_821F9BF0) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_821F9BF0(ctx, base);
  if (self && self == g_ambient.load(std::memory_order_acquire)) ApplyAmbient(base, self);
}

// Once per guest frame (flip hook).
extern "C" void p2_quality_frame() {
  uint8_t* base = g_base.load(std::memory_order_relaxed);
  if (!base) return;
  ApplyAmbient(base, g_ambient.load(std::memory_order_acquire));
  ScaleFloat(base, 0x82DC5864, g_shadow, REXCVAR_GET(p2_shadow_range_scale));
  constexpr uint32_t kLodBias = 0x82DC1E6C;
  const uint32_t lod = Load32(base, kLodBias);
  float game_lod;
  if (g_lod.Track(lod)) {
    std::memcpy(&game_lod, &g_lod.game, 4);
    REXLOG_INFO("[p2-quality] {}: game {} x {}", g_lod.name, game_lod, REXCVAR_GET(p2_geo_lod_bias_scale));
  }
  std::memcpy(&game_lod, &g_lod.game, 4);
  const float want = float(double(game_lod) * REXCVAR_GET(p2_geo_lod_bias_scale));
  std::memcpy(&g_lod.written, &want, 4);
  if (lod != g_lod.written) Store32(base, kLodBias, g_lod.written);
}

// WORKLOG #107: the game's internal render size (its renderer init parameters, sub_82258D70, hard-code 1200x672;
// tools/prepare_switch_codegen.py routes them here). Read once at renderer init. Rounded down to the Xbox 360
// EDRAM tile grid (80 x 16 pixels) and kept within 640x352 .. 1200x672.
REXCVAR_DEFINE_UINT32(p2_render_width, 1200, "Prototype2",
                      "Internal render width (the game's own 1200). Lower = fewer pixels for the GPU; the "
                      "picture is scaled to the screen (see present_effect = fsr). Multiple of 80; restart");
REXCVAR_DEFINE_UINT32(p2_render_height, 672, "Prototype2",
                      "Internal render height (the game's own 672). Multiple of 16; restart");
extern "C" int64_t p2_render_width() {
  const uint32_t w = std::clamp<uint32_t>(REXCVAR_GET(p2_render_width), 640, 1200) / 80 * 80;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("[p2-quality] internal render size {}x{} (game 1200x672)", w,
                std::clamp<uint32_t>(REXCVAR_GET(p2_render_height), 352, 672) / 16 * 16);
  }
  return w;
}
extern "C" int64_t p2_render_height() {
  return std::clamp<uint32_t>(REXCVAR_GET(p2_render_height), 352, 672) / 16 * 16;
}
