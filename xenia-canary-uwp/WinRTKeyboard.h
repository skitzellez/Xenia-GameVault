#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <utility>

namespace UWP {
void ShowKeyboard();
void HandleCharacter(uint32_t keycode);
// Queues a key event by Windows virtual key code.
void HandleKey(uint32_t virtual_key, bool is_down);

extern std::vector<uint32_t> g_char_buffer;
extern std::vector<std::pair<uint32_t, bool>> g_key_buffer;
extern std::mutex g_buffer_mutex;
}