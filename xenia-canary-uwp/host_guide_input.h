// Copyright 2026 Xenia Canary. Released under the BSD license.

#ifndef XENIA_UWP_HOST_GUIDE_INPUT_H_
#define XENIA_UWP_HOST_GUIDE_INPUT_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace UWP {

class HostGuideChord {
 public:
  // XInput Back/View + Start/Menu. Both buttons must be on the same pad.
  static constexpr uint16_t kButtons = 0x0020 | 0x0010;

  std::optional<size_t> Poll(std::span<const uint16_t> controller_buttons) {
    std::optional<size_t> pressed_controller;
    for (size_t i = 0; i < controller_buttons.size(); ++i) {
      if ((controller_buttons[i] & kButtons) == kButtons) {
        pressed_controller = i;
        break;
      }
    }
    const bool rising = pressed_controller.has_value() && !was_down_;
    was_down_ = pressed_controller.has_value();
    return rising ? pressed_controller : std::nullopt;
  }

 private:
  bool was_down_ = false;
};

}  // namespace UWP

#endif  // XENIA_UWP_HOST_GUIDE_INPUT_H_
