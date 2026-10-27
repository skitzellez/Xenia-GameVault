// Copyright 2026 Xenia Canary. Released under the BSD license.

#ifndef XENIA_UWP_GUEST_INPUT_CAPTURE_H_
#define XENIA_UWP_GUEST_INPUT_CAPTURE_H_

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>

#include "xenia/hid/input.h"
#include "xenia/xbox.h"

namespace UWP {

using xe::X_RESULT;

// Publish the open bit and its generation together. A dialog may open and close
// entirely between two guest polls; the changed snapshot still captures it.
class UIInputCaptureState {
 public:
  uint64_t snapshot() const { return state_.load(); }
  static bool IsOpen(uint64_t snapshot) { return (snapshot & 1) != 0; }

  void SetOpen(bool open) {
    uint64_t previous = state_.load();
    while (IsOpen(previous) != open &&
           !state_.compare_exchange_weak(previous, previous + 1)) {
    }
  }

 private:
  std::atomic<uint64_t> state_{0};
};

// Owned by one InputSystem and used only under its lock. Host controller reads
// stay raw, so the guide can still navigate while the guest receives neutral
// input. Take UI snapshots around the raw state read, not just after it.
class GuestInputCapture {
 public:
  explicit GuestInputCapture(uint64_t initial_ui_snapshot = 0)
      : ui_snapshot_(initial_ui_snapshot) {}

  bool Capture(uint32_t user_index, uint64_t ui_before, uint64_t ui_after,
               bool guest_ui_open, const xe::hid::X_INPUT_GAMEPAD& gamepad,
               bool connected = true) {
    const bool ui_open = UIInputCaptureState::IsOpen(ui_before) ||
                         UIInputCaptureState::IsOpen(ui_after) || guest_ui_open;
    if (ui_snapshot_ != ui_after || ui_before != ui_after ||
        (ui_open && !ui_was_open_)) {
      for (auto& keys : suppressed_keys_) {
        keys.set();
      }
      release_pending_.fill(true);
      keystrokes_pending_.fill(true);
    }
    ui_snapshot_ = ui_after;
    ui_was_open_ = ui_open;
    if (ui_open) {
      release_pending_.fill(true);
      keystrokes_pending_.fill(true);
    }
    if (user_index >= release_pending_.size()) {
      return true;
    }
    // A state read spanning a UI transition can be an old neutral sample from
    // before the closing B press. Only a stable, closed-UI read can release it.
    if (ui_open || ui_before != ui_after) {
      return true;
    }
    if (!release_pending_[user_index]) {
      return false;
    }
    if (connected && IsNeutral(gamepad)) {
      release_pending_[user_index] = false;
    }
    // Consume the first neutral sample as well as every held sample.
    return true;
  }

  bool NeedsKeystrokeDrain(uint32_t user_index) const {
    return user_index < keystrokes_pending_.size() &&
           keystrokes_pending_[user_index];
  }

  // Poll receives a pointer to a native, guest-layout keystroke. Share the same
  // queue filter between both guest keystroke exports and state-only games, so
  // UI events cannot survive until the game first calls a keystroke API later.
  template <typename Poll>
  xe::X_RESULT GetKeystroke(uint32_t user_index, bool captured, Poll poll,
                            xe::hid::X_INPUT_KEYSTROKE* output) {
    *output = {};
    if (user_index >= release_pending_.size()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    const bool drain = captured || NeedsKeystrokeDrain(user_index);
    // A stuck/repeating driver must not monopolize the input lock. Keep the
    // drain pending when this bound is reached and continue on the next poll.
    for (uint32_t i = 0; i < 64; ++i) {
      xe::hid::X_INPUT_KEYSTROKE event{};
      const xe::X_RESULT result = poll(&event);
      if (result != X_ERROR_SUCCESS) {
        if (result == X_ERROR_EMPTY && !release_pending_[user_index]) {
          keystrokes_pending_[user_index] = false;
        }
        return result;
      }
      if (drain) {
        continue;
      }
      auto& suppressed = suppressed_keys_[user_index];
      const uint16_t key = event.virtual_key;
      if (suppressed.test(key)) {
        // XInput may report an old key-up or repeat after its state becomes
        // neutral. Only a fresh key-down can start another guest key sequence.
        if (!(event.flags & xe::hid::X_INPUT_KEYSTROKE_KEYDOWN) ||
            (event.flags & (xe::hid::X_INPUT_KEYSTROKE_REPEAT |
                            xe::hid::X_INPUT_KEYSTROKE_KEYUP))) {
          continue;
        }
        suppressed.reset(key);
      }
      *output = event;
      return X_ERROR_SUCCESS;
    }
    return X_ERROR_EMPTY;
  }

 private:
  static bool IsNeutral(const xe::hid::X_INPUT_GAMEPAD& gamepad) {
    using namespace xe::hid;
    const auto centered = [](int16_t value, int32_t deadzone) {
      return value >= -deadzone && value <= deadzone;
    };
    return !gamepad.buttons &&
           gamepad.left_trigger <= X_INPUT_GAMEPAD_TRIGGER_THRESHOLD &&
           gamepad.right_trigger <= X_INPUT_GAMEPAD_TRIGGER_THRESHOLD &&
           centered(gamepad.thumb_lx, X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE) &&
           centered(gamepad.thumb_ly, X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE) &&
           centered(gamepad.thumb_rx, X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) &&
           centered(gamepad.thumb_ry, X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
  }

  uint64_t ui_snapshot_ = 0;
  bool ui_was_open_ = false;
  std::array<bool, xe::XUserMaxUserCount> release_pending_{};
  std::array<bool, xe::XUserMaxUserCount> keystrokes_pending_{};
  std::array<std::bitset<65536>, xe::XUserMaxUserCount> suppressed_keys_{};
};

}  // namespace UWP

#endif  // XENIA_UWP_GUEST_INPUT_CAPTURE_H_
