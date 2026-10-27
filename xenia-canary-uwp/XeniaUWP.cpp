#include "XeniaUWP.h"

#include "UWPUtil.h"
#include "WinRTKeyboard.h"
#include "host_guide_input.h"
#include "ui_controller_selection.h"

#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <algorithm>
#include <array>

#include "surface_uwp.h"
#include "window_uwp.h"
#include "windowed_app_context_uwp.h"

#include "third_party/imgui/imgui.h"

#include "xenia/apu/xaudio2/xaudio2_audio_system.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/main_win.h"
#include "xenia/base/string.h"
#include "xenia/config.h"
#include "xenia/emulator.h"
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#include "xenia/hid/input_system.h"
#include "xenia/hid/nop/nop_hid.h"
#include "xenia/hid/xinput/xinput_hid.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/vfs/devices/disc_zarchive_device.h"

using namespace xe;
using namespace xe::hid;

DECLARE_string(gamepaths);
DEFINE_string(gamepaths, "",
              "Paths the frontend will search for games.", "General");

static std::unique_ptr<ui::WindowedApp> app = nullptr;
static std::unique_ptr<ui::UWPWindowedAppContext> app_context = nullptr;
static ui::Window* s_window;
static hid::InputSystem* s_input_system;
static std::vector<std::string> s_paths;
static std::vector<std::tuple<std::string, std::string>> s_games;

namespace {
std::function<void()> host_guide_action;
std::function<void()> companion_tick;
UWP::HostGuideChord host_guide_chord;
UWP::UIControllerSelector ui_controller_selector;

X_RESULT GetHostState(uint32_t index, hid::X_INPUT_STATE* state) {
  if (s_input_system) {
    auto lock = s_input_system->lock();
    return s_input_system->GetState(index, X_INPUT_FLAG_GAMEPAD, state);
  }
  auto* driver = s_window
                     ? static_cast<xe::ui::UWPWindow*>(s_window)->xinputdriver()
                     : nullptr;
  return driver ? driver->GetState(index, state) : X_ERROR_DEVICE_NOT_CONNECTED;
}

static_assert(UWP::HostGuideChord::kButtons ==
              (X_INPUT_GAMEPAD_BACK | X_INPUT_GAMEPAD_START));

void PollHostGuideShortcut() {
  if (!s_window || !host_guide_action) {
    return;
  }
  std::array<uint16_t, XUserMaxUserCount> buttons{};
  {
    for (uint32_t i = 0; i < buttons.size(); ++i) {
      hid::X_INPUT_STATE state{};
      if (GetHostState(i, &state) == X_ERROR_SUCCESS) {
        buttons[i] = state.gamepad.buttons;
      }
    }
  }
  // Aggregate all pads before updating the edge, so an idle or disconnected
  // pad cannot repeatedly retrigger a chord held on another controller.
  if (auto controller = host_guide_chord.Poll(buttons)) {
    ui_controller_selector.RequestSlot(static_cast<uint32_t>(*controller));
    host_guide_action();
  }
}

constexpr uint64_t kAnalogNavInitialDelayMs = 275;
constexpr uint64_t kAnalogNavRepeatIntervalMs = 115;

enum class AnalogNavDirection { kLeft = 0, kRight, kUp, kDown };

struct AnalogNavRepeatState {
  bool active = false;
  bool repeating = false;
  uint64_t start_time_ms = 0;
  uint64_t last_emit_time_ms = 0;
};

std::array<AnalogNavRepeatState, 4> g_analog_nav_repeat_states;

bool UpdateAnalogNavRepeatState(AnalogNavDirection direction,
                                bool analog_active, uint64_t now_ms) {
  auto& state = g_analog_nav_repeat_states[static_cast<size_t>(direction)];

  if (!analog_active) {
    state = {};
    return false;
  }

  if (!state.active) {
    state.active = true;
    state.start_time_ms = now_ms;
    state.last_emit_time_ms = now_ms;
    return true;
  }

  if (!state.repeating) {
    if (now_ms - state.start_time_ms >= kAnalogNavInitialDelayMs) {
      state.repeating = true;
      state.last_emit_time_ms = now_ms;
      return true;
    }
    return false;
  }

  if (now_ms - state.last_emit_time_ms >= kAnalogNavRepeatIntervalMs) {
    state.last_emit_time_ms = now_ms;
    return true;
  }

  return false;
}

void ResetAnalogNavRepeatStates() {
  for (auto& state : g_analog_nav_repeat_states) {
    state = {};
  }
}

UWP::UIControllerSelector::Selection PollUIController() {
  std::array<UWP::UIControllerSelector::Sample, XUserMaxUserCount> samples{};
  for (uint32_t i = 0; i < samples.size(); ++i) {
    hid::X_INPUT_STATE raw{};
    samples[i].connected = GetHostState(i, &raw) == X_ERROR_SUCCESS;
    samples[i].gamepad = raw.gamepad;
  }
  return ui_controller_selector.Poll(samples);
}

}  // namespace

void UWP::StartXenia() {
  app_context = std::make_unique<ui::UWPWindowedAppContext>();
  app = xe::ui::GetWindowedAppCreator()(*app_context.get());

  xe::InitializeWin32App(app->GetName());

  if (app->OnInitialize()) {
    RefreshPaths();
  } else {
    app_context->QuitFromUIThread();
  }
}

bool UWP::HasQuit() {
  return !app_context || app_context->HasQuitFromUIThread();
}

void UWP::RequestQuit() {
  if (app_context) {
    app_context->QuitFromUIThread();
  }
}

void UWP::ShutdownXenia() {
  SetHostGuideAction({});
  if (app) {
    app->InvokeOnDestroy();
    app.reset();
  }
  s_window = nullptr;
  s_input_system = nullptr;
  companion_tick = {};
  app_context.reset();
  xe::ShutdownWin32App();
}

void UWP::ExecutePendingFunctionsFromUIThread() {
  app_context->ExecutePendingFunctionsFromUIThread();

  if (s_window && !app_context->HasQuitFromUIThread()) {
    if (auto tick = companion_tick) {
      tick();
    }
    PollHostGuideShortcut();
    s_window->RequestPaint();
    static_cast<ui::UWPWindow*>(s_window)->PaintIfRequestedFromUIThread();
  }
}

void UWP::SetHostGuideAction(std::function<void()> action) {
  host_guide_action = std::move(action);
  host_guide_chord = {};
}

void UWP::RegisterXeniaWindow(xe::ui::Window* window) { s_window = window; }

void UWP::RegisterInputSystem(xe::hid::InputSystem* input_system) {
  s_input_system = input_system;
  ui_controller_selector = {};
  ResetAnalogNavRepeatStates();
}

void UWP::SetCompanionTick(std::function<void()> tick) {
  companion_tick = std::move(tick);
}

bool UWP::ArePickerControlsReleased() {
  {
    for (uint32_t index = 0; index < XUserMaxUserCount; ++index) {
      hid::X_INPUT_STATE state{};
      if (GetHostState(index, &state) == X_ERROR_SUCCESS &&
          state.gamepad.buttons != 0) {
        return false;
      }
    }
  }
  using winrt::Windows::System::VirtualKey;
  using winrt::Windows::UI::Core::CoreVirtualKeyStates;
  if (auto window =
          winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread()) {
    for (const auto key :
         {VirtualKey::Enter, VirtualKey::Space, VirtualKey::Escape,
          VirtualKey::LeftButton, VirtualKey::RightButton}) {
      if ((window.GetKeyState(key) & CoreVirtualKeyStates::Down) !=
          CoreVirtualKeyStates::None) {
        return false;
      }
    }
  }
  return true;
}

void UWP::UpdateImGuiIO() {
  // Input is only queued for ImGui while a dialog is open, otherwise it is
  // dropped. The selector still polls the pads.
  if (!UWP::IsUIOpen()) {
    {
      std::unique_lock lk(UWP::g_buffer_mutex);
      UWP::g_char_buffer.clear();
      UWP::g_key_buffer.clear();
    }
    PollUIController();
    ResetAnalogNavRepeatStates();
    return;
  }

  ImGuiIO& io = ImGui::GetIO();
  io.AddKeyEvent(ImGuiKey_Backspace, false);
  io.AddKeyEvent(ImGuiKey_Enter, false);
  // On-screen keyboard caret keys, one press per key-down event.
  static constexpr struct {
    uint32_t virtual_key;
    ImGuiKey key;
  } kCaretKeys[] = {
      {0x25, ImGuiKey_LeftArrow}, {0x27, ImGuiKey_RightArrow},
      {0x26, ImGuiKey_UpArrow},   {0x28, ImGuiKey_DownArrow},
      {0x24, ImGuiKey_Home},      {0x23, ImGuiKey_End},
      {0x2E, ImGuiKey_Delete},
  };
  for (const auto& caret_key : kCaretKeys) {
    io.AddKeyEvent(caret_key.key, false);
  }

  {
    std::unique_lock lk(UWP::g_buffer_mutex);
    for (uint32_t c : UWP::g_char_buffer) {
      io.AddInputCharacter(c);

      if (c == '\b') {
        io.AddKeyEvent(ImGuiKey_Backspace, true);
      } else if (c == '\r') {
        io.AddKeyEvent(ImGuiKey_Enter, true);
      }
    }
    UWP::g_char_buffer.clear();
    for (const auto& [virtual_key, is_down] : UWP::g_key_buffer) {
      if (!is_down) {
        continue;
      }
      for (const auto& caret_key : kCaretKeys) {
        if (caret_key.virtual_key == virtual_key) {
          io.AddKeyEvent(caret_key.key, true);
        }
      }
    }
    UWP::g_key_buffer.clear();
  }

  const auto selected = PollUIController();
  const bool connected = selected.connected;
  hid::X_INPUT_STATE state{};
  state.gamepad = selected.gamepad;
  if (selected.changed) {
    ResetAnalogNavRepeatStates();
  }
  // ImGui discards every gamepad key at NewFrame unless the backend reports
  // a connected controller. UWP supplies input here instead of UpdateGamepads.
  if (connected) {
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  } else {
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    state = {};
    ResetAnalogNavRepeatStates();
  }

  const uint16_t buttons = state.gamepad.buttons;

  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (buttons & X_INPUT_GAMEPAD_A) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (buttons & X_INPUT_GAMEPAD_B) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);
  io.AddKeyEvent(ImGuiKey_F12, (buttons & X_INPUT_GAMEPAD_X) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceUp, (buttons & X_INPUT_GAMEPAD_Y) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadStart, (buttons & X_INPUT_GAMEPAD_START) != 0);

  io.AddKeyEvent(ImGuiKey_GamepadBack, (buttons & X_INPUT_GAMEPAD_BACK) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadL1,
                 (buttons & X_INPUT_GAMEPAD_LEFT_SHOULDER) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadR1,
                 (buttons & X_INPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadL3,
                 (buttons & X_INPUT_GAMEPAD_LEFT_THUMB) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadR3,
                 (buttons & X_INPUT_GAMEPAD_RIGHT_THUMB) != 0);

  const int16_t kStickNavDeadzone = X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
  const bool ls_left = state.gamepad.thumb_lx <= -kStickNavDeadzone;
  const bool ls_right = state.gamepad.thumb_lx >= kStickNavDeadzone;
  const bool ls_up = state.gamepad.thumb_ly >= kStickNavDeadzone;
  const bool ls_down = state.gamepad.thumb_ly <= -kStickNavDeadzone;
  const uint64_t now_ms = Clock::QueryHostUptimeMillis();

  const bool analog_nav_left =
      UpdateAnalogNavRepeatState(AnalogNavDirection::kLeft, ls_left, now_ms);
  const bool analog_nav_right =
      UpdateAnalogNavRepeatState(AnalogNavDirection::kRight, ls_right, now_ms);
  const bool analog_nav_up =
      UpdateAnalogNavRepeatState(AnalogNavDirection::kUp, ls_up, now_ms);
  const bool analog_nav_down =
      UpdateAnalogNavRepeatState(AnalogNavDirection::kDown, ls_down, now_ms);

  io.AddKeyEvent(
      ImGuiKey_GamepadDpadLeft,
      ((buttons & X_INPUT_GAMEPAD_DPAD_LEFT) != 0) || analog_nav_left);
  io.AddKeyEvent(
      ImGuiKey_GamepadDpadRight,
      ((buttons & X_INPUT_GAMEPAD_DPAD_RIGHT) != 0) || analog_nav_right);
  io.AddKeyEvent(ImGuiKey_GamepadDpadUp,
                 ((buttons & X_INPUT_GAMEPAD_DPAD_UP) != 0) || analog_nav_up);
  io.AddKeyEvent(
      ImGuiKey_GamepadDpadDown,
      ((buttons & X_INPUT_GAMEPAD_DPAD_DOWN) != 0) || analog_nav_down);

  // Right stick still exposed for camera/mouse emulation
  constexpr float kStickDeadzone = 8000.0f / 32767.0f;
  const float rx = state.gamepad.thumb_rx / 32767.0f;
  const float ry = state.gamepad.thumb_ry / 32767.0f;

  io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickLeft, rx < -kStickDeadzone,
                       rx < 0 ? -rx : 0.0f);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickRight, rx > kStickDeadzone,
                       rx > 0 ? rx : 0.0f);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickUp, ry > kStickDeadzone,
                       ry > 0 ? ry : 0.0f);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickDown, ry < -kStickDeadzone,
                       ry < 0 ? -ry : 0.0f);

  // Triggers as L2/R2
  constexpr float kTriggerDeadzone = 30.0f / 255.0f;
  const float lt = state.gamepad.left_trigger / 255.0f;
  const float rt = state.gamepad.right_trigger / 255.0f;
  io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, lt > kTriggerDeadzone, lt);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, rt > kTriggerDeadzone, rt);
}

// Game Vault picks the games: no game list to scan here (the Blades front end had one).
void UWP::RefreshPaths() { s_games.clear(); }
std::vector<std::tuple<std::string, std::string>> UWP::GetGames() {
  return s_games;
}

void UWP::SetGamePaths(std::vector<std::string> paths) {
  s_paths.clear();
  s_paths.insert(s_paths.end(), paths.begin(), paths.end());
  std::stringstream ss;
  for (auto p : s_paths) {
    ss << p << ";";
  }

  auto cpaths = dynamic_cast<cvar::ConfigVar<std::string>*>(
      cvar::ConfigVars->find("gamepaths")->second);
  cpaths->SetConfigValue(ss.str());
  config::SaveConfig();
  RefreshPaths();
}

std::vector<std::string> UWP::GetPaths() { return s_paths; }
