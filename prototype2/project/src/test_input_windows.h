#pragma once
#if defined(_WIN32)
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <mutex>
#include <rex/input/input_driver.h>
// Opt-in integration-test input supplied to the guest through its input API.
class Prototype2TestInput final : public rex::input::InputDriver {
 public:
  Prototype2TestInput() : InputDriver(nullptr, 0) {}
  rex::X_STATUS Setup() override {
    if (const char* file = std::getenv("PROTOTYPE2_TEST_INPUT_FILE")) {
      std::ifstream input(file);
      int ms, buttons, lx, ly, rx, ry, lt, rt;
      while (input >> ms >> buttons >> lx >> ly >> rx >> ry >> lt >> rt) {
        ScriptStep step{}; step.ms = ms; step.gamepad.buttons = buttons;
        step.gamepad.thumb_lx = lx; step.gamepad.thumb_ly = ly;
        step.gamepad.thumb_rx = rx; step.gamepad.thumb_ry = ry;
        step.gamepad.left_trigger = lt; step.gamepad.right_trigger = rt;
        steps_.push_back(step);
      }
      if (steps_.empty() || !input.eof()) return static_cast<rex::X_STATUS>(0xC000000D);
    }
    start_ = std::chrono::steady_clock::now(); return 0;
  }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = device_; info.name = "Prototype2 test controller";
    out.push_back(info);
  }
  rex::X_RESULT GetDeviceState(rex::input::DeviceId, rex::input::X_INPUT_STATE* state) override {
    const std::lock_guard lock(state_mutex_);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
    *state = {};
    for (const auto& step : steps_) {
      if (ms < step.ms) break;
      state->gamepad = step.gamepad;
    }
    // Opt-in live guest-controller state for interactive integration testing.
    // The operating system receives no synthetic keys or controller events.
    if (const char* file = std::getenv("PROTOTYPE2_TEST_LIVE_INPUT_FILE")) {
      if (ms >= next_live_poll_ms_) {
        next_live_poll_ms_ = ms + 100;
        std::ifstream input(file);
        int buttons, lx, ly, rx, ry, lt, rt;
        live_input_active_ = bool(input >> buttons >> lx >> ly >> rx >> ry >> lt >> rt);
        if (live_input_active_ && buttons >= 0 && buttons <= 65535 &&
            lx >= -32768 && lx <= 32767 && ly >= -32768 && ly <= 32767 &&
            rx >= -32768 && rx <= 32767 && ry >= -32768 && ry <= 32767 &&
            lt >= 0 && lt <= 255 && rt >= 0 && rt <= 255) {
          live_gamepad_ = {};
          live_gamepad_.buttons = buttons;
          live_gamepad_.thumb_lx = lx; live_gamepad_.thumb_ly = ly;
          live_gamepad_.thumb_rx = rx; live_gamepad_.thumb_ry = ry;
          live_gamepad_.left_trigger = lt; live_gamepad_.right_trigger = rt;
        } else {
          live_input_active_ = false;
        }
      }
      if (live_input_active_) state->gamepad = live_gamepad_;
    }
    // PROTOTYPE2_TEST_AUTOPRESS="<buttons> <period_ms> <hold_ms> [start_ms]": press repeatedly
    // (skips videos/prompts in unattended agent runs).
    // Several entries separated by ';' (their buttons are OR-ed while each one is held).
    if (const char* a = std::getenv("PROTOTYPE2_TEST_AUTOPRESS")) {
      uint16_t pressed = 0;
      for (const char* entry = a; entry && *entry; ) {
        int b = 0, period = 0, hold = 300, from = 0, until = INT32_MAX;
        if (std::sscanf(entry, "%d %d %d %d %d", &b, &period, &hold, &from, &until) >= 2 &&
            period > 0 && ms >= from && ms < until && ((ms - from) % period) < hold)
          pressed = static_cast<uint16_t>(pressed | b);
        entry = std::strchr(entry, ';');
        if (entry) ++entry;
      }
      if (pressed) state->gamepad.buttons = static_cast<uint16_t>(uint16_t(state->gamepad.buttons) | pressed);
    }
    const bool changed = std::memcmp(&previous_gamepad_, &state->gamepad, sizeof(previous_gamepad_)) != 0;
    if (std::memcmp(&previous_gamepad_, &state->gamepad, sizeof(previous_gamepad_)) != 0) {
      ++packet_number_; previous_gamepad_ = state->gamepad;
    }
    state->packet_number = packet_number_;
    // Record state transitions only; useful when a title accepts START but waits afterward.
    if (changed || !logged_first_) {
      if (const char* folder = std::getenv("PROTOTYPE2_TEST_FRAME_DIR")) {
        std::ofstream log(std::string(folder) + "/input.txt", std::ios::app);
        log << "ms=" << ms << " buttons=" << state->gamepad.buttons
            << " lx=" << state->gamepad.thumb_lx << " ly=" << state->gamepad.thumb_ly
            << " rx=" << state->gamepad.thumb_rx << " ry=" << state->gamepad.thumb_ry
            << " lt=" << unsigned(state->gamepad.left_trigger) << " rt=" << unsigned(state->gamepad.right_trigger) << "\n";
      }
      logged_first_ = true;
    }
    return 0;
  }
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId, uint32_t, rex::input::X_INPUT_CAPABILITIES* caps) override {
    *caps = {}; caps->type = 1; caps->sub_type = 1; caps->gamepad.buttons = 0xF3FF;
    caps->gamepad.left_trigger = 255; caps->gamepad.right_trigger = 255;
    caps->gamepad.thumb_lx = -1; caps->gamepad.thumb_ly = -1;
    caps->gamepad.thumb_rx = -1; caps->gamepad.thumb_ry = -1;
    caps->vibration.left_motor_speed = 65535; caps->vibration.right_motor_speed = 65535;
    return 0;
  }
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId, rex::input::X_INPUT_VIBRATION*) override { return 0; }
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override { return static_cast<rex::X_RESULT>(4306); }
 private:
  static constexpr auto device_ = static_cast<rex::input::DeviceId>(0x50320001);
  std::chrono::steady_clock::time_point start_;
  bool logged_first_ = false;
  std::mutex state_mutex_;
  rex::input::X_INPUT_GAMEPAD previous_gamepad_{};
  uint32_t packet_number_ = 1;
  int64_t next_live_poll_ms_ = 0;
  bool live_input_active_ = false;
  rex::input::X_INPUT_GAMEPAD live_gamepad_{};
  struct ScriptStep { int ms; rex::input::X_INPUT_GAMEPAD gamepad; };
  std::vector<ScriptStep> steps_;
};
#endif
