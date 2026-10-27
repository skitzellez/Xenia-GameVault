#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

namespace winrt {
struct fire_and_forget;
}

namespace xe {
namespace hid { class InputSystem; }
namespace ui {
class Window;
class ImGuiDrawer;
}
}  // namespace xe

namespace UWP {
void StartXenia();
bool HasQuit();
void RequestQuit();
// These lifecycle operations run on the app's UI thread.
void RequestTitleRestart(xe::ui::ImGuiDrawer* drawer, bool launch_data_saved);
void RestoreTitleRestart(bool has_title);
bool ShouldReturnToFrontend();
void ReturnToFrontend(xe::ui::ImGuiDrawer* drawer);
void ShutdownXenia();
void ExecutePendingFunctionsFromUIThread();
void RegisterXeniaWindow(xe::ui::Window* window);
void RegisterInputSystem(xe::hid::InputSystem* input_system);
void SetCompanionTick(std::function<void()> tick);
void UpdateImGuiIO();
bool ArePickerControlsReleased();
// Registered and invoked on the UI thread; cleared before emulator teardown.
void SetHostGuideAction(std::function<void()> action);

void RefreshPaths();
std::vector<std::tuple<std::string, std::string>> GetGames();
std::vector<std::string> GetPaths();
void SetGamePaths(std::vector<std::string> paths);
}  // namespace UWP
