// Copyright 2026 Xenia Canary. Released under the BSD license.

#ifndef XENIA_UWP_UI_CONTROLLER_SELECTION_H_
#define XENIA_UWP_UI_CONTROLLER_SELECTION_H_

#include <array>
#include <cstdint>
#include <optional>

#include "xenia/hid/input.h"

namespace UWP {

class UIControllerSelector {
 public:
  struct Sample {
    bool connected = false;
    xe::hid::X_INPUT_GAMEPAD gamepad{};
  };
  struct Selection : Sample {
    uint32_t slot = 0;
    bool changed = false;
  };
  static constexpr size_t kSlots = 4;

  void RequestSlot(uint32_t slot) {
    if (slot < kSlots) {
      requested_slot_ = slot;
    }
  }

  Selection Poll(const std::array<Sample, kSlots>& samples) {
    std::array<uint32_t, kSlots> activity{}, fresh{};
    for (size_t i = 0; i < kSlots; ++i) {
      if (samples[i].connected) {
        activity[i] = Activity(samples[i].gamepad);
        // Reconnecting with buttons held must not activate frontend controls.
        fresh[i] = connected_[i] ? activity[i] & ~previous_[i] : 0;
      }
    }

    uint32_t selected = active_slot_;
    if (requested_slot_ && samples[*requested_slot_].connected) {
      selected = *requested_slot_;
    } else if (!samples[selected].connected || !fresh[selected]) {
      for (uint32_t i = 0; i < kSlots; ++i) {
        if (samples[i].connected && fresh[i]) {
          selected = i;
          break;
        }
      }
    }
    requested_slot_.reset();
    if (!samples[selected].connected) {
      for (uint32_t i = 0; i < kSlots; ++i) {
        if (samples[i].connected) {
          selected = i;
          break;
        }
      }
    }

    const bool changed =
        selected != active_slot_ || samples[selected].connected != had_active_;
    // If a new source is already holding B while A causes the handover, only
    // A may reach the guide. Each previously held control must first release.
    uint32_t defer = 0;
    if (changed || !connected_[selected]) {
      blocked_ = activity[selected] & ~fresh[selected];
      // Release a key for one frame before a fresh press on another source
      // reuses it. Otherwise ImGui would see a continuous hold across pads.
      defer = previous_output_ & fresh[selected];
    } else {
      blocked_ &= activity[selected];
    }
    active_slot_ = selected;
    had_active_ = samples[selected].connected;
    Selection result;
    result.slot = selected;
    result.changed = changed;
    result.connected = had_active_;
    if (result.connected) {
      result.gamepad = Filter(samples[selected].gamepad, blocked_ | defer);
    }
    previous_output_ = result.connected ? Activity(result.gamepad) : 0;
    for (size_t i = 0; i < kSlots; ++i) {
      previous_[i] = activity[i];
      connected_[i] = samples[i].connected;
    }
    return result;
  }

 private:
  static uint32_t Activity(const xe::hid::X_INPUT_GAMEPAD& pad) {
    uint32_t bits = uint16_t(pad.buttons) & 0xF3FF;
    if (pad.left_trigger > 30) {
      bits |= 1u << 16;
    }
    if (pad.right_trigger > 30) {
      bits |= 1u << 17;
    }
    const auto axis = [&bits](int16_t value, int threshold, unsigned offset) {
      if (value < -threshold) {
        bits |= 1u << offset;
      }
      if (value > threshold) {
        bits |= 1u << (offset + 1);
      }
    };
    // UWP's left-stick navigation is inclusive at the XInput deadzone edge.
    axis(pad.thumb_lx, xe::hid::X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE - 1, 18);
    axis(pad.thumb_ly, xe::hid::X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE - 1, 20);
    axis(pad.thumb_rx, 8000, 22);
    axis(pad.thumb_ry, 8000, 24);
    return bits;
  }
  static xe::hid::X_INPUT_GAMEPAD Filter(xe::hid::X_INPUT_GAMEPAD pad,
                                         uint32_t blocked) {
    pad.buttons = uint16_t(pad.buttons) & ~uint16_t(blocked);
    if (blocked & (1u << 16)) {
      pad.left_trigger = 0;
    }
    if (blocked & (1u << 17)) {
      pad.right_trigger = 0;
    }
    const auto axis = [blocked](int16_t value, unsigned offset) -> int16_t {
      const unsigned bit = offset + (value >= 0 ? 1 : 0);
      return blocked & (1u << bit) ? 0 : value;
    };
    pad.thumb_lx = axis(pad.thumb_lx, 18);
    pad.thumb_ly = axis(pad.thumb_ly, 20);
    pad.thumb_rx = axis(pad.thumb_rx, 22);
    pad.thumb_ry = axis(pad.thumb_ry, 24);
    return pad;
  }

  uint32_t active_slot_ = 0;
  bool had_active_ = false;
  std::optional<uint32_t> requested_slot_;
  std::array<uint32_t, kSlots> previous_{};
  std::array<bool, kSlots> connected_{};
  uint32_t blocked_ = 0;
  uint32_t previous_output_ = 0;
};

}  // namespace UWP

#endif  // XENIA_UWP_UI_CONTROLLER_SELECTION_H_
