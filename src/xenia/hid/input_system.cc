/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "xenia/hid/input_system.h"

#include "xenia/base/cvar.h"
#include "xenia/base/profiling.h"
#include "xenia/hid/input_driver.h"
#include "xenia/kernel/util/shim_utils.h"

#if XE_PLATFORM_WIN32 && !XE_PLATFORM_WINRT  // (no USB portal in the Xbox app)
#include "xenia/hid/portal/hardware_portal.h"
#endif  // XE_PLATFORM_WIN32

namespace xe {
namespace hid {

DEFINE_bool(vibration, true, "Toggle controller vibration.", "HID");

DEFINE_double(left_stick_deadzone_percentage, 0.0,
              "Defines deadzone level for left stick. Allowed range [0.0-1.0].",
              "HID");
DEFINE_double(
    right_stick_deadzone_percentage, 0.0,
    "Defines deadzone level for right stick. Allowed range [0.0-1.0].", "HID");

DEFINE_transient_string(
    slot_bindings_passthrough, "",
    "Internal: slot bindings handed from a parent xenia process to its child "
    "across out-of-process relaunch. Not persisted.",
    "HID");

InputSystem::InputSystem(xe::ui::Window* window) : window_(window) {
#if XE_PLATFORM_WIN32 && !XE_PLATFORM_WINRT
  portal_ = std::make_unique<HardwarePortal>();
#endif  // XE_PLATFORM_WIN32
}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  LoadSlotBindingsFromPassthrough();
  // SDL drains its initial DEVICEADDED events inside the driver's own Setup,
  // before AddDriver wires devices_changed_callback_ — that auto-reconcile is
  // lost, so already-connected controllers need an explicit pass to attach.
  ReconcileBindings();
  return X_STATUS_SUCCESS;
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  driver->set_devices_changed_callback([this]() { NotifyDevicesChanged(); });
  drivers_.push_back(std::move(driver));
}

void InputSystem::NotifyDevicesChanged() {
  if (reconcile_pending_.exchange(true)) {
    return;
  }
  if (!window_) {
    reconcile_pending_.store(false);
    return;
  }
  window_->app_context().CallInUIThreadDeferred([this]() {
    reconcile_pending_.store(false);
    {
      auto lock = this->lock();
      ReconcileBindings();
    }
    // The device list has changed; menu/toolbar must refresh even if
    // ReconcileBindings didn't alter any bindings (e.g. a dismissed device
    // replugged is visible again but stays unbound).
    if (bindings_changed_cb_) {
      bindings_changed_cb_();
    }
  });
}

void InputSystem::UpdateUsedSlot(InputDriver* driver, uint8_t slot,
                                 bool connected) {
  if (slot == XUserIndexAny) {
    XELOGW("{} received requrest for slot any! Unsupported", __func__);
    return;
  }

  // Do not report passthrough as a controller.
  if (driver && driver->GetInputType() == InputType::Keyboard) {
    return;
  }

  if (connected_slots.test(slot) == connected) {
    // No state change, so nothing to do.
    return;
  }

  XELOGI(controller_slot_state_change_message[connected].c_str(), slot);
  connected_slots.flip(slot);
  if (kernel::kernel_state()) {
    kernel::kernel_state()->BroadcastNotification(
        kXNotificationSystemInputDevicesChanged, 0);
  }

  if (driver) {
    X_INPUT_CAPABILITIES capabilities = {};
    const X_RESULT result = driver->GetCapabilities(slot, 0, &capabilities);
    if (result != X_STATUS_SUCCESS) {
      return;
    }

    controllers_max_joystick_value[slot] = {
        {capabilities.gamepad.thumb_lx, capabilities.gamepad.thumb_ly},
        {capabilities.gamepad.thumb_rx, capabilities.gamepad.thumb_ry}};
  }
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");

  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  bool connected = false;
  for (const auto& binding : slot_bindings_[user_index]) {
    if (!binding.driver || (flags & binding.driver->GetInputType()) == 0) {
      continue;
    }
    X_INPUT_CAPABILITIES caps = {};
    X_INPUT_CAPABILITIES* target = connected ? &caps : out_caps;
    if (binding.driver->GetCapabilities(binding.driver_slot, flags, target) !=
        X_ERROR_SUCCESS) {
      continue;
    }
    if (binding.subtype_override != 0) {
      target->sub_type = binding.subtype_override;
    }
    if (connected) {
      MergeCapabilities(out_caps, caps);
    }
    connected = true;
  }
  return connected ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

// The devices in a slot read as one controller: the first one that isn't a
// plain gamepad sets its type, and it has what any of them has.
void InputSystem::MergeCapabilities(X_INPUT_CAPABILITIES* caps,
                                    const X_INPUT_CAPABILITIES& other) {
  if (caps->sub_type == X_INPUT_DEVSUBTYPE::XINPUT_DEVSUBTYPE_GAMEPAD) {
    caps->type = other.type;
    caps->sub_type = other.sub_type;
  }
  caps->flags = caps->flags | other.flags;
  auto& pad = caps->gamepad;
  const auto& other_pad = other.gamepad;
  pad.buttons = pad.buttons | other_pad.buttons;
  pad.left_trigger = std::max(pad.left_trigger, other_pad.left_trigger);
  pad.right_trigger = std::max(pad.right_trigger, other_pad.right_trigger);
  pad.thumb_lx = pad.thumb_lx | other_pad.thumb_lx;
  pad.thumb_ly = pad.thumb_ly | other_pad.thumb_ly;
  pad.thumb_rx = pad.thumb_rx | other_pad.thumb_rx;
  pad.thumb_ry = pad.thumb_ry | other_pad.thumb_ry;
  auto& vibration = caps->vibration;
  vibration.left_motor_speed = std::max<uint16_t>(
      vibration.left_motor_speed, other.vibration.left_motor_speed);
  vibration.right_motor_speed = std::max<uint16_t>(
      vibration.right_motor_speed, other.vibration.right_motor_speed);
}

X_RESULT InputSystem::GetState(uint32_t user_index, uint32_t flags,
                               X_INPUT_STATE* out_state, bool ui_active) {
  SCOPE_profile_cpu_f("hid");

  X_RESULT result = GetStateForUI(user_index, flags, out_state);
  if (result != X_ERROR_SUCCESS) {
    return result;
  }
  out_state->packet_number =
      out_state->packet_number + ui_input_blocking_edges_.load();

  // While UI holds the input, connected controllers read as at rest and empty
  // slots stay empty, as xam reports them. The packet number stays live here
  // since xam freezes it only for its own UI.
  if (ui_active || IsUIInputBlocked()) {
    uint32_t packet_number = out_state->packet_number;
    std::memset(out_state, 0, sizeof(X_INPUT_STATE));
    out_state->packet_number = packet_number;
    return X_ERROR_SUCCESS;
  }

  // Mask buttons that were held when a UI dialog closed until they're
  // released, so the close-press doesn't carry through into the game.
  if (consumed_buttons_[user_index] != 0) {
    uint16_t buttons = out_state->gamepad.buttons;
    consumed_buttons_[user_index] &= buttons;
    out_state->gamepad.buttons = buttons & ~consumed_buttons_[user_index];
  }

  return result;
}

X_RESULT InputSystem::GetStateForUI(uint32_t user_index, uint32_t flags,
                                    X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");

  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // The devices in the slot act as one controller.
  InputDriver* connected_driver = nullptr;
  bool other_type = false;
  for (const auto& binding : slot_bindings_[user_index]) {
    if (!binding.driver) {
      continue;
    }
    if ((flags & binding.driver->GetInputType()) == 0) {
      other_type = true;
      continue;
    }
    X_INPUT_STATE state = {};
    X_INPUT_STATE* target = connected_driver ? &state : out_state;
    if (binding.driver->GetState(binding.driver_slot, target) !=
        X_ERROR_SUCCESS) {
      continue;
    }
    if (connected_driver) {
      MergeState(out_state, state);
    } else {
      connected_driver = binding.driver;
    }
  }
  // Wrong device type for this slot is not a disconnect.
  if (!connected_driver && other_type) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (connected_driver) {
    UpdateUsedSlot(connected_driver, user_index, true);
    AdjustDeadzoneLevels(user_index, &out_state->gamepad);
    if (out_state->gamepad.buttons != 0) {
      last_used_slot = user_index;
    }
    return X_ERROR_SUCCESS;
  }
  UpdateUsedSlot(nullptr, user_index, false);
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

// Any button pressed on either device is pressed, each axis takes whichever is
// pushed further, and the packet number moves when either device's does.
void InputSystem::MergeState(X_INPUT_STATE* state, const X_INPUT_STATE& other) {
  state->packet_number = state->packet_number + other.packet_number;
  auto& pad = state->gamepad;
  const auto& other_pad = other.gamepad;
  pad.buttons = pad.buttons | other_pad.buttons;
  pad.left_trigger = std::max(pad.left_trigger, other_pad.left_trigger);
  pad.right_trigger = std::max(pad.right_trigger, other_pad.right_trigger);
  auto further = [](be<int16_t>& axis, int16_t other_axis) {
    if (std::abs(static_cast<int32_t>(other_axis)) >
        std::abs(static_cast<int32_t>(axis))) {
      axis = other_axis;
    }
  };
  further(pad.thumb_lx, other_pad.thumb_lx);
  further(pad.thumb_ly, other_pad.thumb_ly);
  further(pad.thumb_rx, other_pad.thumb_rx);
  further(pad.thumb_ry, other_pad.thumb_ry);
}

void InputSystem::AddUIInputBlocker() {
  if (ui_input_blockers_.fetch_add(1) == 0) {
    ui_input_blocking_edges_.fetch_add(1);
  }
}

void InputSystem::RemoveUIInputBlocker() {
  // Before removing the blocker, capture any currently pressed buttons.
  // These will be masked from game input until they are released, preventing
  // the button press that closed the UI from carrying over into the game.
  X_INPUT_STATE state;
  for (uint32_t user_index = 0; user_index < XUserMaxUserCount; user_index++) {
    if (GetStateForUI(user_index, 1, &state) == X_ERROR_SUCCESS) {
      consumed_buttons_[user_index] |= state.gamepad.buttons;
    }
  }

  // Discard before unblocking so no title gets keystrokes made in the UI.
  for (auto& driver : drivers_) {
    driver->DiscardKeystrokes();
  }
  if (ui_input_blockers_.fetch_sub(1) == 1) {
    ui_input_blocking_edges_.fetch_add(1);
  }
}

X_RESULT InputSystem::SetState(uint32_t user_index,
                               X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");
  X_INPUT_VIBRATION modified_vibration = ModifyVibrationLevel(vibration);
  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT result = X_ERROR_DEVICE_NOT_CONNECTED;
  for (const auto& binding : slot_bindings_[user_index]) {
    if (binding.driver &&
        binding.driver->SetState(binding.driver_slot, &modified_vibration) ==
            X_ERROR_SUCCESS) {
      result = X_ERROR_SUCCESS;
    }
  }
  return result;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke,
                                   bool ui_active) {
  SCOPE_profile_cpu_f("hid");

  // While UI holds the input, connected slots have no keystrokes and empty ones
  // stay empty, as xam reports them. Never success, since a title draining
  // keystrokes until EMPTY would spin forever.
  if (ui_active || IsUIInputBlocked()) {
    for (auto& driver : drivers_) {
      driver->DiscardKeystrokes();
    }
    std::memset(out_keystroke, 0, sizeof(X_INPUT_KEYSTROKE));
    X_INPUT_STATE state;
    // Keyboard drivers feed every slot, like below.
    bool connected =
        GetStateForUI(user_index, flags, &state) == X_ERROR_SUCCESS ||
        ((flags & InputType::Keyboard) &&
         std::any_of(drivers_.begin(), drivers_.end(), [](const auto& driver) {
           return driver->GetInputType() == InputType::Keyboard;
         }));
    return connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  bool any_connected = false;

  auto try_driver = [&](InputDriver* driver, uint8_t driver_slot) -> X_RESULT {
    X_RESULT r = driver->GetKeystroke(driver_slot, flags, out_keystroke);
    if (r == X_ERROR_INVALID_PARAMETER || r == X_ERROR_DEVICE_NOT_CONNECTED) {
      return r;
    }
    any_connected = true;
    if (r == X_ERROR_SUCCESS) {
      last_used_slot = user_index;
      // Drivers stamp their own slot. The title expects the user it asked for.
      out_keystroke->user_index = static_cast<uint8_t>(user_index);
    }
    return r;
  };

  // Bound devices for this slot.
  if (user_index < XUserMaxUserCount) {
    for (const auto& binding : slot_bindings_[user_index]) {
      if (!binding.driver || (flags & binding.driver->GetInputType()) == 0) {
        continue;
      }
      X_RESULT r = try_driver(binding.driver, binding.driver_slot);
      if (r == X_ERROR_SUCCESS) {
        return r;
      }
    }
  }

  // Passthrough fallback: Keyboard-type drivers feed all slots in parallel
  // and don't participate in the binding table.
  if (flags & InputType::Keyboard) {
    for (auto& driver : drivers_) {
      if (driver->GetInputType() != InputType::Keyboard) {
        continue;
      }
      X_RESULT r = try_driver(driver.get(), static_cast<uint8_t>(user_index));
      if (r == X_ERROR_SUCCESS) {
        return r;
      }
    }
  }

  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

bool InputSystem::GetVibrationCvar() { return cvars::vibration; }

void InputSystem::ToggleVibration() {
  OVERRIDE_PERSIST_bool(vibration, !cvars::vibration);
  // Send instant update to vibration state to prevent awaiting for next tick.
  X_INPUT_VIBRATION vibration = X_INPUT_VIBRATION();

  for (uint8_t user_index = 0; user_index < XUserMaxUserCount; user_index++) {
    SetState(user_index, &vibration);
  }
}

void InputSystem::AdjustDeadzoneLevels(const uint8_t slot,
                                       X_INPUT_GAMEPAD* gamepad) {
  if (slot >= XUserMaxUserCount) {
    return;
  }

  // Left stick
  if (cvars::left_stick_deadzone_percentage > 0.0 &&
      cvars::left_stick_deadzone_percentage < 1.0) {
    const double deadzone_lx_percentage =
        controllers_max_joystick_value[slot].first.first *
        cvars::left_stick_deadzone_percentage;
    const double deadzone_ly_percentage =
        controllers_max_joystick_value[slot].first.second *
        cvars::left_stick_deadzone_percentage;

    const double theta = std::atan2(static_cast<double>(gamepad->thumb_ly),
                                    static_cast<double>(gamepad->thumb_lx));

    const double deadzone_y_value = std::sin(theta) * deadzone_ly_percentage;
    const double deadzone_x_value = std::cos(theta) * deadzone_lx_percentage;

    if (gamepad->thumb_ly > -deadzone_y_value &&
        gamepad->thumb_ly < deadzone_y_value) {
      gamepad->thumb_ly = 0;
    }

    if (gamepad->thumb_lx > -deadzone_x_value &&
        gamepad->thumb_lx < deadzone_x_value) {
      gamepad->thumb_lx = 0;
    }
  }

  // Right stick
  if (cvars::right_stick_deadzone_percentage > 0.0 &&
      cvars::right_stick_deadzone_percentage < 1.0) {
    const double deadzone_rx_percentage =
        controllers_max_joystick_value[slot].second.first *
        cvars::right_stick_deadzone_percentage;
    const double deadzone_ry_percentage =
        controllers_max_joystick_value[slot].second.second *
        cvars::right_stick_deadzone_percentage;

    const double theta = std::atan2(static_cast<double>(gamepad->thumb_ry),
                                    static_cast<double>(gamepad->thumb_rx));

    const double deadzone_y_value = std::sin(theta) * deadzone_ry_percentage;
    const double deadzone_x_value = std::cos(theta) * deadzone_rx_percentage;

    if (gamepad->thumb_ry > -deadzone_y_value &&
        gamepad->thumb_ry < deadzone_y_value) {
      gamepad->thumb_ry = 0;
    }

    if (gamepad->thumb_rx > -deadzone_x_value &&
        gamepad->thumb_rx < deadzone_x_value) {
      gamepad->thumb_rx = 0;
    }
  }
}

X_INPUT_VIBRATION InputSystem::ModifyVibrationLevel(
    X_INPUT_VIBRATION* vibration) {
  X_INPUT_VIBRATION modified_vibration = *vibration;
  if (cvars::vibration) {
    return modified_vibration;
  }

  // TODO(Gliniak): Use modifier instead of boolean value.
  modified_vibration.left_motor_speed = 0;
  modified_vibration.right_motor_speed = 0;
  return modified_vibration;
}
std::unique_lock<xe_unlikely_mutex> InputSystem::lock() {
  return std::unique_lock<xe_unlikely_mutex>{lock_};
}

void InputSystem::ReconcileBindings() {
  struct LiveDevice {
    InputDriver* driver;
    InputDeviceInfo info;
  };
  std::vector<LiveDevice> live;
  for (auto& d : drivers_) {
    for (auto& info : d->EnumerateDevices()) {
      live.push_back({d.get(), std::move(info)});
    }
  }

  // Pass 1: demote bindings whose device has gone away.
  for (auto& bindings : slot_bindings_) {
    for (auto& b : bindings) {
      if (!b.driver) {
        continue;
      }
      bool found = false;
      for (auto& l : live) {
        if (l.driver == b.driver && l.info.driver_slot == b.driver_slot) {
          found = true;
          break;
        }
      }
      if (!found) {
        b.driver = nullptr;
        b.driver_slot = 0;
      }
    }
  }

  // Pass 2: reattach returning devices by stable_id, before pass 3 places any
  // device in their slots.
  std::vector<const LiveDevice*> unbound;
  for (const auto& l : live) {
    if (FindBinding(l.driver, l.info.driver_slot)) {
      continue;
    }
    SlotBinding* detached = nullptr;
    for (auto& bindings : slot_bindings_) {
      for (auto& b : bindings) {
        if (!b.driver && !b.stable_id.empty() &&
            b.stable_id == l.info.stable_id) {
          detached = &b;
          break;
        }
      }
      if (detached) {
        break;
      }
    }
    if (!detached) {
      unbound.push_back(&l);
      continue;
    }
    detached->driver = l.driver;
    detached->driver_slot = l.info.driver_slot;
    detached->display_name = l.info.display_name;
  }

  // Pass 3: auto-bind devices that opt in, unless the user dismissed them this
  // session. A device goes to a slot holding nothing from its own driver, so
  // controllers spread across slots and the keyboard joins one.
  for (const LiveDevice* l : unbound) {
    if (!l->info.auto_bind) {
      continue;
    }
    if (!l->info.stable_id.empty() && dismissed_ids_.count(l->info.stable_id)) {
      continue;
    }
    auto is_free = [&](uint32_t slot) {
      for (const auto& b : slot_bindings_[slot]) {
        if (b.driver == l->driver) {
          return false;
        }
      }
      return true;
    };
    uint32_t slot = XUserMaxUserCount;
    const int8_t preferred = l->info.preferred_slot;
    if (preferred >= 0 && preferred < XUserMaxUserCount && is_free(preferred)) {
      slot = preferred;
    } else {
      for (uint32_t s = 0; s < XUserMaxUserCount; ++s) {
        if (is_free(s)) {
          slot = s;
          break;
        }
      }
    }
    if (slot == XUserMaxUserCount) {
      continue;
    }
    // A device that left the slot gives it up, and is placed like a new one if
    // it returns.
    auto& bindings = slot_bindings_[slot];
    bindings.erase(
        std::remove_if(bindings.begin(), bindings.end(),
                       [](const SlotBinding& b) { return !b.driver; }),
        bindings.end());
    SlotBinding binding;
    binding.driver = l->driver;
    binding.driver_slot = l->info.driver_slot;
    binding.stable_id = l->info.stable_id;
    binding.display_name = l->info.display_name;
    bindings.push_back(std::move(binding));
  }

  // Pass 4: a device with a native mode uses it unless its type is overridden.
  for (auto& bindings : slot_bindings_) {
    for (auto& b : bindings) {
      if (b.driver) {
        b.driver->SetNativeMode(b.driver_slot, !b.subtype_override);
      }
    }
  }
}

InputSystem::SlotBinding* InputSystem::FindBinding(InputDriver* driver,
                                                   uint8_t driver_slot,
                                                   uint32_t* guest_slot) {
  if (!driver) {
    return nullptr;
  }
  for (uint32_t s = 0; s < XUserMaxUserCount; ++s) {
    for (auto& b : slot_bindings_[s]) {
      if (b.driver == driver && b.driver_slot == driver_slot) {
        if (guest_slot) {
          *guest_slot = s;
        }
        return &b;
      }
    }
  }
  return nullptr;
}

std::vector<InputSystem::EnumeratedDevice> InputSystem::EnumerateDevices() {
  ReconcileBindings();
  std::vector<EnumeratedDevice> out;
  for (auto& d : drivers_) {
    for (auto& info : d->EnumerateDevices()) {
      int bound_slot = -1;
      uint32_t guest_slot;
      if (FindBinding(d.get(), info.driver_slot, &guest_slot)) {
        bound_slot = static_cast<int>(guest_slot);
      }
      out.push_back({d.get(), std::move(info), bound_slot});
    }
  }
  return out;
}

void InputSystem::BindSlot(uint32_t guest_slot, InputDriver* driver,
                           uint8_t driver_slot, std::string stable_id,
                           std::string display_name) {
  if (guest_slot >= XUserMaxUserCount || !driver) {
    return;
  }

  if (!stable_id.empty()) {
    dismissed_ids_.erase(stable_id);
  }

  // The device joins the ones bound here, moving from any other slot along
  // with its type override.
  uint32_t bound_slot = 0;
  SlotBinding* binding = FindBinding(driver, driver_slot, &bound_slot);
  if (!binding || bound_slot != guest_slot) {
    SlotBinding moved;
    if (binding) {
      moved.subtype_override = binding->subtype_override;
      auto& from = slot_bindings_[bound_slot];
      from.erase(from.begin() + (binding - from.data()));
    }
    moved.driver = driver;
    moved.driver_slot = driver_slot;
    moved.stable_id = std::move(stable_id);
    moved.display_name = std::move(display_name);
    slot_bindings_[guest_slot].push_back(std::move(moved));
    binding = &slot_bindings_[guest_slot].back();
  }

  driver->OnBoundToSlot(driver_slot, guest_slot);
  driver->SetNativeMode(driver_slot, !binding->subtype_override);
  NotifyBindingsChanged();
}

void InputSystem::SetSubtypeOverride(InputDriver* driver, uint8_t driver_slot,
                                     uint8_t subtype) {
  SlotBinding* binding = FindBinding(driver, driver_slot);
  if (!binding || binding->subtype_override == subtype) {
    return;
  }
  binding->subtype_override = subtype;
  driver->SetNativeMode(driver_slot, !subtype);
  NotifyBindingsChanged();
}

void InputSystem::UnbindDevice(InputDriver* driver, uint8_t driver_slot) {
  uint32_t guest_slot = 0;
  SlotBinding* binding = FindBinding(driver, driver_slot, &guest_slot);
  if (!binding) {
    return;
  }
  if (!binding->stable_id.empty()) {
    dismissed_ids_.insert(binding->stable_id);
  }
  auto& bindings = slot_bindings_[guest_slot];
  bindings.erase(bindings.begin() + (binding - bindings.data()));
  driver->OnUnboundFromSlot(driver_slot);
  NotifyBindingsChanged();
}

void InputSystem::NotifyBindingsChanged() {
  if (kernel::kernel_state()) {
    kernel::kernel_state()->BroadcastNotification(
        kXNotificationSystemInputDevicesChanged, 0);
  }
  if (bindings_changed_cb_) {
    bindings_changed_cb_();
  }
}

// Format: four ';'-separated slots, each a ','-separated list of
// "stable_id[:subtype]" with the subtype omitted when zero. Stable IDs are
// "keyboard" or 32-hex-char SDL GUIDs, so ';', ',' and ':' need no escaping.
std::string InputSystem::SerializeSlotBindingsForPassthrough() const {
  std::string out;
  bool any = false;
  for (uint32_t s = 0; s < XUserMaxUserCount; ++s) {
    if (s) {
      out += ';';
    }
    const size_t slot_start = out.size();
    for (const auto& b : slot_bindings_[s]) {
      if (b.stable_id.empty()) {
        continue;
      }
      if (out.size() > slot_start) {
        out += ',';
      }
      any = true;
      out += b.stable_id;
      if (b.subtype_override) {
        out += ':';
        out += std::to_string(b.subtype_override);
      }
    }
  }
  return any ? out : std::string();
}

void InputSystem::LoadSlotBindingsFromPassthrough() {
  const std::string& s = cvars::slot_bindings_passthrough;
  if (s.empty()) {
    return;
  }
  uint32_t slot = 0;
  size_t pos = 0;
  while (slot < XUserMaxUserCount && pos <= s.size()) {
    size_t sep = s.find(';', pos);
    if (sep == std::string::npos) {
      sep = s.size();
    }
    const std::string slot_entries = s.substr(pos, sep - pos);
    pos = sep + 1;
    size_t entry_pos = 0;
    while (entry_pos < slot_entries.size()) {
      size_t comma = slot_entries.find(',', entry_pos);
      if (comma == std::string::npos) {
        comma = slot_entries.size();
      }
      std::string entry = slot_entries.substr(entry_pos, comma - entry_pos);
      entry_pos = comma + 1;
      if (entry.empty()) {
        continue;
      }
      uint8_t subtype = 0;
      size_t colon = entry.find(':');
      if (colon != std::string::npos) {
        unsigned long v = std::strtoul(entry.c_str() + colon + 1, nullptr, 10);
        subtype = static_cast<uint8_t>(v);
        entry.resize(colon);
      }
      SlotBinding b;
      b.stable_id = std::move(entry);
      b.subtype_override = subtype;
      slot_bindings_[slot].push_back(std::move(b));
    }
    ++slot;
  }
}

}  // namespace hid
}  // namespace xe
