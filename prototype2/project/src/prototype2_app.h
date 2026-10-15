// prototype2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/rex_app.h>
#include <rex/filesystem.h>
#include <filesystem>
#include <string>
#if defined(_WIN32)
#include <chrono>
#include <fstream>
#include <thread>
#include <sstream>
#include <vector>
#include <rex/input/input_system.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>
#include "test_input_windows.h"
#endif
#if defined(__SWITCH__)
#include <rex/logging.h>
void prototype2_RunSkinningSelfTests();
bool prototype2_RunFPSCRSelfTest();
bool prototype2_RunArmCpuSelfTest();
bool prototype2_RunDynamicBufferSelfTest();
void prototype2_EnableGuestStackAudit();
unsigned int prototype2_EnableSystemScreenshots();  // switch_screenshot.cpp
#include "native/p2_native_system.h"
REXCVAR_DECLARE(std::string, render_target_path_vulkan);
#endif

// rex_p2_msr_lock: defined in the generated prototype2_pch.h (tools/prepare_switch_codegen.py), included here.
#if defined(__SWITCH__)
REXCVAR_DECLARE(uint32_t, p2_msr_lock_mode);
void prototype2_EnableTaskPumpTimeoutFix();
#endif

class Prototype2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Prototype2App>(new Prototype2App(ctx, "prototype2",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // Without a gpu_plugin the Vd* kernel calls are ignored and nothing renders (same default as Mercs2).
  void OnPostInitLogging() override {
    if (!rex::cvar::HasNonDefaultValue("gpu_plugin")) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
  }
#if defined(__SWITCH__)
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Native renderer (src/native, from Mercs2/project8-sw): replaces the SDK's emulated GPU when
    // p2_renderer = "native". Default stays the FSI emulation below.
    rex_p2_msr_lock = int(std::min<uint32_t>(REXCVAR_GET(p2_msr_lock_mode), 5u));
    REXLOG_INFO("Prototype 2 guest msr regions: {}",
                rex_p2_msr_lock == 5 ? "global mutex except hot job functions" : rex_p2_msr_lock == 4 ? "global mutex, spin first" : rex_p2_msr_lock == 3 ? "guest spinlock" : rex_p2_msr_lock == 2 ? "global mutex" : rex_p2_msr_lock == 1 ? "lock per address" : "lock-free");
    prototype2_EnableGuestStackAudit();
    prototype2_EnableTaskPumpTimeoutFix();
    if (!prototype2_RunFPSCRSelfTest()) REX_FATAL("ARM FPSCR selftest failed before guest launch");
    if (!prototype2_RunArmCpuSelfTest()) REX_FATAL("ARM CPU lowering selftest failed before guest launch");
    if (!prototype2_RunDynamicBufferSelfTest()) REX_FATAL("ARM dynamic buffer cache selftest failed before guest launch");
    prototype2_RunSkinningSelfTests();
    if (p2::native::Enabled()) {
      config.graphics = p2::native::CreateGraphicsSystem();
      REXLOG_INFO("Prototype 2 Switch renderer: native");
    }
    // Same default as Mercs2 (guide stage 6): the FBO EDRAM path gave a red screen and ~1460
    // transfers per frame; FSI is the verified emulation path until the native renderer lands.
    if (rex::cvar::GetFlagSource("render_target_path_vulkan") == rex::cvar::Source::kDefault &&
        REXCVAR_GET(render_target_path_vulkan).empty()) {
      REXCVAR_SET(render_target_path_vulkan, std::string("fsi"));
    }
    REXLOG_INFO("Prototype 2 Switch render target path: {}", REXCVAR_GET(render_target_path_vulkan));
    // 0 = Application (full memory); 0xFFFFFFFF = applet/album launch, which runs out of memory.
    REXLOG_INFO("Prototype 2 Switch screenshot permission: result 0x{:08X}",
                prototype2_EnableSystemScreenshots());
  }
#endif
#if defined(_WIN32)
  // Agent test harness (opt-in, environment only; same scheme as Mercs2):
  //  PROTOTYPE2_TEST_INPUT_FILE / PROTOTYPE2_TEST_LIVE_INPUT_FILE - scripted / live guest pad state
  //  PROTOTYPE2_TEST_FRAME_DIR - frame-<n>.ppm of the game's own output whenever <dir>/capture.req
  //  appears (the file is deleted after capture), plus at PROTOTYPE2_TEST_CAPTURE_SECONDS.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (!std::getenv("PROTOTYPE2_TEST_INPUT_FILE") && !std::getenv("PROTOTYPE2_TEST_LIVE_INPUT_FILE") &&
        !std::getenv("PROTOTYPE2_TEST_AUTOPRESS")) return;
    config.input_factory = [](bool) -> std::unique_ptr<rex::system::IInputSystem> {
      auto input = std::make_unique<rex::input::InputSystem>(nullptr);
      auto driver = std::make_unique<Prototype2TestInput>();
      if (driver->Setup() != 0) REX_FATAL("Invalid scripted input file");
      input->AddDriver(std::move(driver));
      input->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
      return input;
    };
  }
  void OnPostLaunchModule(rex::system::XThread*) override {
    const char* supplied = std::getenv("PROTOTYPE2_TEST_FRAME_DIR");
    if (!supplied || !*supplied) return;
    const std::filesystem::path folder = supplied;
    std::filesystem::create_directories(folder);
    test_capture_ = std::jthread([this, folder](std::stop_token stop) {
      const auto start = std::chrono::steady_clock::now();
      std::vector<int> seconds_list;
      if (const char* s = std::getenv("PROTOTYPE2_TEST_CAPTURE_SECONDS")) {
        std::istringstream in(s); int v; while (in >> v) seconds_list.push_back(v);
      }
      size_t next = 0; int index = 0;
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        std::error_code ec;
        const bool requested = std::filesystem::exists(folder / "capture.req", ec);
        const bool timed = next < seconds_list.size() && elapsed >= seconds_list[next];
        if (!requested && !timed) continue;
        if (timed) ++next;
        if (requested) std::filesystem::remove(folder / "capture.req", ec);
        auto* graphics = runtime()->graphics_system();
        rex::ui::RawImage image;
        const bool ok = graphics && graphics->presenter() &&
                        graphics->presenter()->CaptureGuestOutput(image);
        const std::string name = "frame-" + std::to_string(index++) + "-t" + std::to_string(elapsed);
        if (!ok || !image.width || !image.height) {
          std::ofstream(folder / (name + ".failed")) << "capture failed\n";
          continue;
        }
        std::ofstream out(folder / (name + ".ppm"), std::ios::binary);
        out << "P6\n" << image.width << " " << image.height << "\n255\n";
        for (uint32_t y = 0; y < image.height; ++y)
          for (uint32_t x = 0; x < image.width; ++x)
            out.write(reinterpret_cast<const char*>(image.data.data() + y * image.stride + x * 4), 3);
      }
    });
  }
  void OnShutdown() override {
    if (test_capture_.joinable()) { test_capture_.request_stop(); test_capture_.join(); }
  }
#endif
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // Diagnostic: PROTOTYPE2_DUMP_IMAGE=<file> writes the loaded, decrypted image (guest 0x82000000, 0xFD0000 bytes)
  // for tools/scan_code_pointers.py. Off unless the variable is set.
  void OnPostSetup() override {
    if (const char* path = std::getenv("PROTOTYPE2_DUMP_IMAGE")) {
      if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(runtime()->virtual_membase() + 0x82000000u, 1, 0xFD0000u, f);
        std::fclose(f);
      }
    }
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) return;
    if (const char* supplied = std::getenv("PROTOTYPE2_GAME_ROOT")) {
      paths.game_data_root = supplied;
      return;
    }
    std::error_code ec;
    const auto extracted = rex::filesystem::GetExecutableFolder() / "game_root";
    if (std::filesystem::is_directory(extracted, ec)) {
      paths.game_data_root = extracted;
      return;
    }
#if defined(__SWITCH__)
    // Some NRO loaders give no argv[0]; fall back to the packaged SD location.
    const std::filesystem::path packaged = "/switch/prototype2-nx/game_root";
    ec.clear();
    if (std::filesystem::is_directory(packaged, ec)) paths.game_data_root = packaged;
#endif
  }
#if defined(_WIN32)
 private:
  std::jthread test_capture_;
#endif
};
