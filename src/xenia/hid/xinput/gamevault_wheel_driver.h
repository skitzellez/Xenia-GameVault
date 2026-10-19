/* Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
 * BSD licence, like Xenia (see LICENSE in the root).
 *
 * Racing wheels on the Xbox (Windows.Gaming.Input.RacingWheel). See gamevault_wheel_driver.cc.
 */

#ifndef XENIA_HID_XINPUT_GAMEVAULT_WHEEL_DRIVER_H_
#define XENIA_HID_XINPUT_GAMEVAULT_WHEEL_DRIVER_H_

#include <memory>

#include "xenia/hid/input_driver.h"

namespace xe {
namespace hid {
namespace xinput {

// the wheel driver; added after the XInput driver, so a pad in a slot always wins
std::unique_ptr<InputDriver> CreateGameVaultWheel(xe::ui::Window* window,
                                                  size_t window_z_order);

}  // namespace xinput
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_XINPUT_GAMEVAULT_WHEEL_DRIVER_H_
