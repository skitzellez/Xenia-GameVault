/* Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
 * BSD licence, like Xenia (see LICENSE in the root).
 *
 * Racing wheels on the Xbox. A wheel (Logitech G920/G923 Xbox, Thrustmaster, Fanatec, MOZA ...) is not a gamepad,
 * so XInput never reports it and the game saw nothing. Windows.Gaming.Input.RacingWheel gives the wheel, pedals and
 * buttons in fixed units on every Xbox wheel, so the first wheel is read here and handed to the game as an ordinary
 * pad in the first slot after the connected pads: steering = left stick X, throttle = right trigger, brake = left
 * trigger, A/B/X/Y, Menu/View and the d-pad as on a pad, gear paddles = bumpers, the rim's two extra buttons = stick
 * clicks. With no wheel this driver reports no controller at all.
 */

#include "xenia/hid/xinput/gamevault_wheel_driver.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>

#include "xenia/base/logging.h"
#include "xenia/hid/input.h"

namespace WGI = winrt::Windows::Gaming::Input;

namespace xe {
namespace hid {
namespace xinput {

namespace {

// steering turned this far (degrees, lock to lock) is a full stick, as on the Xbox 360 wheel; most wheels turn 900
constexpr double kStickLockDegrees = 270.0;

std::string WheelName(const WGI::RacingWheel& w) {
  try {
    if (const auto raw = WGI::RawGameController::FromGameController(w)) {
      std::string name = winrt::to_string(raw.DisplayName());
      char ids[32];
      std::snprintf(ids, sizeof(ids), " (%04X:%04X)", raw.HardwareVendorId(),
                    raw.HardwareProductId());
      return (name.empty() ? std::string("racing wheel") : name) + ids;
    }
  } catch (...) {
  }
  return "racing wheel";
}

class GameVaultWheelDriver final : public InputDriver {
 public:
  GameVaultWheelDriver(xe::ui::Window* window, size_t window_z_order)
      : InputDriver(window, window_z_order) {}

  ~GameVaultWheelDriver() override {
    try {
      if (added_) WGI::RacingWheel::RacingWheelAdded(added_);
      if (removed_) WGI::RacingWheel::RacingWheelRemoved(removed_);
      if (pad_added_) WGI::Gamepad::GamepadAdded(pad_added_);
      if (pad_removed_) WGI::Gamepad::GamepadRemoved(pad_removed_);
    } catch (...) {
    }
  }

  X_STATUS Setup() override {
    try {
      added_ = WGI::RacingWheel::RacingWheelAdded(
          [this](const auto&, const WGI::RacingWheel& w) { OnAdded(w); });
      removed_ = WGI::RacingWheel::RacingWheelRemoved(
          [this](const auto&, const WGI::RacingWheel& w) { OnRemoved(w); });
      pad_added_ = WGI::Gamepad::GamepadAdded(
          [this](const auto&, const auto&) { CountPads(); });
      pad_removed_ = WGI::Gamepad::GamepadRemoved(
          [this](const auto&, const auto&) { CountPads(); });
      for (const WGI::RacingWheel& w : WGI::RacingWheel::RacingWheels()) {
        OnAdded(w);  // usually empty this early; Added follows
      }
      CountPads();
    } catch (...) {
      XELOGW("Game Vault: wheel: racing wheel support could not start");
    }
    return X_STATUS_SUCCESS;  // stays in the list even when it could not start: it then reports nothing
  }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override {
    if (!Wheel(user_index)) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
    out_caps->sub_type = XINPUT_DEVSUBTYPE_GAMEPAD;
    out_caps->gamepad.buttons = 0xF3FF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0xFFC0);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0xFFC0);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0xFFC0);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0xFFC0);
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override {
    const WGI::RacingWheel w = Wheel(user_index);
    if (!w) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    WGI::RacingWheelReading r{};
    WGI::UINavigationReading nav{};
    bool have_nav = false;
    try {
      r = w.GetCurrentReading();
      if (const auto ui = WGI::UINavigationController::FromGameController(w)) {
        nav = ui.GetCurrentReading();
        have_nav = true;
      }
    } catch (...) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }

    const double scale =
        max_angle_ > kStickLockDegrees ? max_angle_ / kStickLockDegrees : 1.0;
    const double steer = std::clamp(r.Wheel * scale, -1.0, 1.0);

    const auto has = [&r](WGI::RacingWheelButtons b) {
      return (r.Buttons & b) == b;
    };
    const auto req = [&](WGI::RequiredUINavigationButtons b) {
      return have_nav && (nav.RequiredButtons & b) == b;
    };
    const auto opt = [&](WGI::OptionalUINavigationButtons b) {
      return have_nav && (nav.OptionalButtons & b) == b;
    };
    using B = WGI::RacingWheelButtons;
    using R = WGI::RequiredUINavigationButtons;
    using O = WGI::OptionalUINavigationButtons;
    uint16_t buttons = 0;
    const auto set = [&buttons](bool on, uint16_t bit) {
      if (on) buttons |= bit;
    };
    set(req(R::Accept), X_INPUT_GAMEPAD_A);
    set(req(R::Cancel), X_INPUT_GAMEPAD_B);
    set(opt(O::Context1), X_INPUT_GAMEPAD_X);
    set(opt(O::Context2), X_INPUT_GAMEPAD_Y);
    set(req(R::Menu), X_INPUT_GAMEPAD_START);
    set(req(R::View), X_INPUT_GAMEPAD_BACK);
    set(has(B::DPadUp) || req(R::Up), X_INPUT_GAMEPAD_DPAD_UP);
    set(has(B::DPadDown) || req(R::Down), X_INPUT_GAMEPAD_DPAD_DOWN);
    set(has(B::DPadLeft) || req(R::Left), X_INPUT_GAMEPAD_DPAD_LEFT);
    set(has(B::DPadRight) || req(R::Right), X_INPUT_GAMEPAD_DPAD_RIGHT);
    set(has(B::PreviousGear) || opt(O::PageLeft),
        X_INPUT_GAMEPAD_LEFT_SHOULDER);  // shift down
    set(has(B::NextGear) || opt(O::PageRight),
        X_INPUT_GAMEPAD_RIGHT_SHOULDER);  // shift up
    set(has(B::Button1), X_INPUT_GAMEPAD_LEFT_THUMB);
    set(has(B::Button2), X_INPUT_GAMEPAD_RIGHT_THUMB);

    out_state->packet_number = ++packet_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger =
        static_cast<uint8_t>(std::clamp(r.Brake, 0.0, 1.0) * 255.0 + 0.5);
    out_state->gamepad.right_trigger =
        static_cast<uint8_t>(std::clamp(r.Throttle, 0.0, 1.0) * 255.0 + 0.5);
    out_state->gamepad.thumb_lx = static_cast<int16_t>(steer * 32767.0);
    out_state->gamepad.thumb_ly = 0;
    out_state->gamepad.thumb_rx = 0;
    out_state->gamepad.thumb_ry = 0;

    // reports: the first time anything is pressed, what the wheel sends (for wheels whose buttons land elsewhere)
    if (!logged_first_input_ &&
        (r.Buttons != B::None || r.Throttle > 0.5 ||
         (have_nav && nav.RequiredButtons != R::None))) {
      logged_first_input_ = true;
      XELOGI(
          "Game Vault: wheel first input (slot {}): wheel {:.2f} throttle "
          "{:.2f} brake {:.2f} clutch {:.2f} buttons 0x{:X} nav 0x{:X}/0x{:X}",
          user_index, r.Wheel, r.Throttle, r.Brake, r.Clutch,
          static_cast<unsigned>(r.Buttons),
          have_nav ? static_cast<unsigned>(nav.RequiredButtons) : 0u,
          have_nav ? static_cast<unsigned>(nav.OptionalButtons) : 0u);
    }
    return X_ERROR_SUCCESS;
  }

  // the game's rumble has no motor here; taken so it does not go elsewhere
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION*) override {
    return Wheel(user_index) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetKeystroke(uint32_t, uint32_t, X_INPUT_KEYSTROKE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  InputType GetInputType() const override { return InputType::Controller; }

 private:
  // the wheel in this slot (the first slot after the pads), or null
  WGI::RacingWheel Wheel(uint32_t user_index) {
    if (user_index != std::min<uint32_t>(pads_.load(), 3)) {
      return nullptr;
    }
    std::lock_guard<std::mutex> lk(lock_);
    return wheels_.empty() ? nullptr : wheels_.front();
  }

  void CountPads() {
    uint32_t n = 0;
    try {
      n = WGI::Gamepad::Gamepads().Size();
    } catch (...) {
    }
    if (pads_.exchange(n) != n && HaveWheel()) {
      XELOGI("Game Vault: wheel now in controller slot {} ({} pad(s) before it)",
             std::min<uint32_t>(n, 3), n);
    }
  }

  bool HaveWheel() {
    std::lock_guard<std::mutex> lk(lock_);
    return !wheels_.empty();
  }

  void OnAdded(const WGI::RacingWheel& w) {
    bool first;
    {
      std::lock_guard<std::mutex> lk(lock_);
      if (std::find(wheels_.begin(), wheels_.end(), w) != wheels_.end()) {
        return;
      }
      wheels_.push_back(w);
      first = wheels_.size() == 1;
    }
    std::string extra;
    try {
      if (first) max_angle_ = w.MaxWheelAngle();
      extra = ", max angle " + std::to_string(static_cast<int>(w.MaxWheelAngle())) +
              ", motor " + (w.WheelMotor() ? "yes" : "no") + ", clutch " +
              (w.HasClutch() ? "yes" : "no") + ", handbrake " +
              (w.HasHandbrake() ? "yes" : "no") + ", shifter " +
              (w.HasPatternShifter() ? "yes" : "no");
    } catch (...) {
    }
    XELOGI(
        "Game Vault: wheel connected: {}{} - {}, as a pad in controller slot "
        "{} (steering = left stick)",
        WheelName(w), extra, first ? "used" : "not used (one wheel at a time)",
        std::min<uint32_t>(pads_.load(), 3));
    logged_first_input_ = false;
  }

  void OnRemoved(const WGI::RacingWheel& w) {
    {
      std::lock_guard<std::mutex> lk(lock_);
      const auto it = std::find(wheels_.begin(), wheels_.end(), w);
      if (it == wheels_.end()) {
        return;
      }
      wheels_.erase(it);
      if (!wheels_.empty()) {
        try {
          max_angle_ = wheels_.front().MaxWheelAngle();
        } catch (...) {
        }
      }
    }
    XELOGI("Game Vault: wheel disconnected");
  }

  std::mutex lock_;
  std::vector<WGI::RacingWheel> wheels_;  // in the order they arrived
  std::atomic<uint32_t> pads_{0};
  std::atomic<double> max_angle_{0};
  uint32_t packet_ = 0;
  bool logged_first_input_ = false;
  winrt::event_token added_{}, removed_{}, pad_added_{}, pad_removed_{};
};

}  // namespace

std::unique_ptr<InputDriver> CreateGameVaultWheel(xe::ui::Window* window,
                                                  size_t window_z_order) {
  return std::make_unique<GameVaultWheelDriver>(window, window_z_order);
}

}  // namespace xinput
}  // namespace hid
}  // namespace xe
