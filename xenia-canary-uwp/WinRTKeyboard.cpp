#include "WinRTKeyboard.h"

#include <winrt/Windows.UI.ViewManagement.Core.h>

namespace UWP {
std::vector<uint32_t> g_char_buffer;
std::vector<std::pair<uint32_t, bool>> g_key_buffer;
std::mutex g_buffer_mutex;

void ShowKeyboard() {
  winrt::Windows::UI::ViewManagement::Core::CoreInputView::GetForCurrentView()
      .TryShowPrimaryView();
}

void HandleCharacter(uint32_t keycode) {
  std::unique_lock lk(g_buffer_mutex);
  g_char_buffer.push_back(keycode);
}

void HandleKey(uint32_t virtual_key, bool is_down) {
  std::unique_lock lk(g_buffer_mutex);
  g_key_buffer.emplace_back(virtual_key, is_down);
}
}  // namespace UWP