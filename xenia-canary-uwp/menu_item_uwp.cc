#include "xenia/ui/menu_item.h"

namespace xe::ui {
// The UWP frontend draws menus with ImGui; it still uses the shared menu model
// for callbacks and dialogs that are also reachable through desktop menus.
std::unique_ptr<MenuItem> MenuItem::Create(Type type, const std::string& text,
                                           const std::string& hotkey,
                                           std::function<void()> callback) {
  return std::unique_ptr<MenuItem>(
      new MenuItem(type, text, hotkey, std::move(callback)));
}
}  // namespace xe::ui
