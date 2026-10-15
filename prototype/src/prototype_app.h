// Prototype (Xbox 360, 4156084E) for Nintendo Switch - ReXGlue recompiled app.
// Structure after penettrator/prototype2-nx project/src/prototype2_app.h (GPL-3.0).
#pragma once

#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#include "native/p2_native_system.h"

REXCVAR_DECLARE(std::string, render_target_path_vulkan);
void proto_ApplySpeedSettings();  // proto_speed.cpp

class PrototypeApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<PrototypeApp>(new PrototypeApp(ctx, "prototype", PPCImageConfig));
  }

  // Without a gpu_plugin the Vd* kernel calls are ignored and nothing renders.
  void OnPostInitLogging() override {
    if (!rex::cvar::HasNonDefaultValue("gpu_plugin")) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    proto_ApplySpeedSettings();
    // p2_renderer = "native" (prototype.toml): the game's Direct3D ring is drawn straight with Vulkan and
    // the SDK's Xenos emulation, with its watched pages and emulated reads, is not created.
    if (p2::native::Enabled()) {
      config.graphics = p2::native::CreateGraphicsSystem();
      REXLOG_INFO("Prototype Switch renderer: native");
    } else {
      REXLOG_INFO("Prototype Switch renderer: SDK Xenos emulation");
    }
    if (rex::cvar::GetFlagSource("render_target_path_vulkan") == rex::cvar::Source::kDefault &&
        REXCVAR_GET(render_target_path_vulkan).empty()) {
      REXCVAR_SET(render_target_path_vulkan, std::string("fsi"));
    }
  }

  // The extracted disc lives in game/ next to the NRO (sdmc:/switch/prototype-nx/game).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) return;
    std::error_code ec;
    for (const char* name : {"game", "game_root"}) {
      const auto dir = rex::filesystem::GetExecutableFolder() / name;
      if (std::filesystem::is_directory(dir, ec)) {
        paths.game_data_root = dir;
        return;
      }
    }
    // Some NRO loaders (forwarders) give no argv[0].
    for (const char* dir : {"/switch/prototype-nx/game", "/switch/Prototype/game"}) {
      ec.clear();
      if (std::filesystem::is_directory(dir, ec)) {
        paths.game_data_root = dir;
        return;
      }
    }
  }
};
