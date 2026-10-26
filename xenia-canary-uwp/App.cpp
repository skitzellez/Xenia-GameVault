#define NOMINMAX

#include <windows.h>
#include <unknwn.h>

#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/windows.graphics.display.core.h>

#include <Gamingdeviceinformation.h>

#include "NetGame.h"
#include "UWPUtil.h"
#include "WinRTKeyboard.h"
#include "XeniaUWP.h"
#include "app_launch_state.h"
#include "protocol_activation.h"
#include "uwp_file_redirect.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/ui/imgui_dialog.h"

using namespace winrt;

using namespace winrt::Windows;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Foundation::Numerics;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Composition;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;

// IStorageItemHandleAccess (WindowsStorageCOM.h, which does not build for apps here): a Win32 handle to a
// StorageFile. Declared as the SDK has it.
struct __declspec(uuid("5CA296B2-2C25-4D22-B785-B885C8201E6A")) GvStorageItemHandleAccess : ::IUnknown {
  virtual HRESULT __stdcall Create(DWORD access, DWORD sharing, DWORD options, void* oplock, HANDLE* handle) = 0;
};

namespace {
UWP::AppLaunchState launch_state;

// Game Vault (Xenia for Game Vault): the launcher shares the game file through the console and sends the
// token with the link. Opened here, Xenia reads the game through that handle (xe::filesystem shared file):
// opening a USB drive's file by path can wait for good on a permission question the console never shows.
// Runs on a thread of its own: blocking on WinRT calls is not allowed on this one.
void ShareFromGameVault(const std::string& path, const winrt::hstring& token) {
  HANDLE handle = nullptr;
  std::thread([&] {
    winrt::init_apartment();
    try {
      auto file = winrt::Windows::ApplicationModel::DataTransfer::SharedStorageAccessManager::
          RedeemTokenForFileAsync(token).get();
      GvStorageItemHandleAccess* access = nullptr;
      if (SUCCEEDED(static_cast<::IUnknown*>(winrt::get_abi(file))
                        ->QueryInterface(__uuidof(GvStorageItemHandleAccess), (void**)&access))) {
        HANDLE h = nullptr;
        if (SUCCEEDED(access->Create(0x120089 /* read */, 0x1 | 0x2 /* share read, write */, 0, nullptr, &h))) {
          handle = h;
        }
        access->Release();
      }
    } catch (...) {
    }
    winrt::uninit_apartment();
  }).join();
  if (handle) {
    xe::filesystem::SetSharedFile(std::filesystem::path(winrt::to_hstring(path).c_str()), handle);
  }
}
std::string restart_activation_arguments;
constexpr auto kRestartContext = L"TitleRestartContext";

winrt::fire_and_forget RestartTitleAsync(xe::ui::ImGuiDrawer* drawer,
                                         winrt::hstring arguments) {
  try {
    auto reason = co_await CoreApplication::RequestRestartAsync(arguments);
    if (reason == AppRestartFailureReason::RestartPending) {
      XELOGI("UWP title restart is pending");
      xe::FlushLog();
      co_return;
    }
    XELOGE("UWP title restart rejected: {}", static_cast<int>(reason));
  } catch (const winrt::hresult_error& error) {
    XELOGE("UWP title restart failed: 0x{:08X}",
           static_cast<uint32_t>(error.code().value));
  }
  xe::FlushLog();
  if (drawer) {
    xe::ui::ImGuiDialog::ShowMessageBox(
        drawer, "Unable to restart Xenia",
        "The game restart is saved, but Xbox could not restart Xenia. "
        "Close and reopen Xenia to continue.");
  }
}

winrt::fire_and_forget ReturnToFrontendAsync(xe::ui::ImGuiDrawer* drawer,
                                            std::string return_uri) {
  try {
    const bool accepted = co_await winrt::Windows::System::Launcher::LaunchUriAsync(
        winrt::Windows::Foundation::Uri{winrt::to_hstring(return_uri)});
    XELOGI("UWP in-process frontend handoff accepted: {}", accepted);
    xe::FlushLog();
    if (accepted) co_return;
  } catch (const winrt::hresult_error& error) {
    XELOGE("UWP in-process frontend handoff failed: 0x{:08X}",
           static_cast<uint32_t>(error.code().value));
    xe::FlushLog();
  }
  if (drawer) {
    xe::ui::ImGuiDialog::ShowMessageBox(drawer, "Unable to open frontend",
                                      "The game has closed. Open the frontend manually.");
  }
}

}  // namespace

void UWP::RequestTitleRestart(xe::ui::ImGuiDrawer* drawer,
                              bool launch_data_saved) {
  if (launch_state.restarting()) {
    return;
  }
  if (launch_data_saved) {
    try {
      auto arguments = winrt::to_hstring(launch_state.RestartArguments());
      ApplicationData::Current().LocalSettings().Values().Insert(
          kRestartContext, winrt::box_value(arguments));
      config::SaveConfig();
      launch_state.BeginTitleRestart();
      XELOGI("UWP requesting app restart for saved title; frontend return: {}",
             !launch_state.return_uri().empty());
      xe::FlushLog();
      RestartTitleAsync(drawer, std::move(arguments));
      return;
    } catch (const winrt::hresult_error& error) {
      XELOGE("UWP could not save restart context: 0x{:08X}",
             static_cast<uint32_t>(error.code().value));
    }
  }
  if (drawer) {
    xe::ui::ImGuiDialog::ShowMessageBox(
        drawer, "Unable to save game restart",
        "The restart request could not be saved. Check that Xenia's storage "
        "location is writable, then reopen the game.");
  }
}

void UWP::RestoreTitleRestart(bool has_title) {
  std::string arguments = std::move(restart_activation_arguments);
  try {
    auto values = ApplicationData::Current().LocalSettings().Values();
    if (values.HasKey(kRestartContext)) {
      arguments = winrt::to_string(
          winrt::unbox_value<winrt::hstring>(values.Lookup(kRestartContext)));
      values.Remove(kRestartContext);
    }
  } catch (const winrt::hresult_error& error) {
    XELOGW("UWP could not read restart context: 0x{:08X}",
           static_cast<uint32_t>(error.code().value));
  }
  if (launch_state.RestoreTitleRestart(has_title, arguments)) {
    XELOGI("UWP restored title restart context; frontend return: {}",
           !launch_state.return_uri().empty());
  }
}

bool UWP::ShouldReturnToFrontend() {
  return launch_state.ShouldReturnToFrontend();
}

void UWP::ReturnToFrontend(xe::ui::ImGuiDrawer* drawer) {
  auto return_uri = launch_state.TakeFrontendReturnUri();
  if (!return_uri.empty()) {
    ReturnToFrontendAsync(drawer, std::move(return_uri));
  }
}

struct App : implements<App, IFrameworkViewSource, IFrameworkView> {
  CompositionTarget m_target{nullptr};
  VisualCollection m_visuals{nullptr};
  Visual m_selected{nullptr};
  float2 m_offset{};

  IFrameworkView CreateView() { return *this; }

  void Initialize(CoreApplicationView const& v) {
    // Game Vault: games on any USB drive (FAT32, exFAT, an NTFS drive no PC prepared), whole folders too
    // (Xbox 360 Games on Demand and unpacked games with default.xex): file calls go through the *FromApp
    // versions, which honour broadFileSystemAccess (uwp_file_redirect.cc)
    UwpRedirectFileApis();
    v.Activated({this, &App::OnActivate});
  }

  void Load(hstring const&) {}

  void Uninitialize() {}

  void Run() {
    auto navigation =
        winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();

    // UWP on Xbox One triggers a back request whenever the B button is
    // pressed which can result in the app being suspended if unhandled
    navigation.BackRequested(
        [](const winrt::Windows::Foundation::IInspectable&,
           const BackRequestedEventArgs& args) { args.Handled(true); });

    winrt::Windows::Graphics::Display::DisplayInformation displayInfo =
        winrt::Windows::Graphics::Display::DisplayInformation::
            GetForCurrentView();
    if (displayInfo) {
      UWP::SetDPI(displayInfo.LogicalDpi());
    }

    UWP::StartXenia();

    try {
      XELOGI(
          "UWP app memory usage: {} MiB; limit: {} MiB",
          winrt::Windows::System::MemoryManager::AppMemoryUsage() >> 20,
          winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20);
    } catch (const winrt::hresult_error& error) {
      XELOGW("Unable to query UWP app memory limit: 0x{:08X}",
             static_cast<uint32_t>(error.code().value));
    }

    while (!UWP::HasQuit()) {
      CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(
          CoreProcessEventsOption::ProcessAllIfPresent);

      UWP::ExecutePendingFunctionsFromUIThread();

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (UWP::ShouldReturnToFrontend()) {
      try {
        XELOGI("UWP returning to launch frontend");
        auto operation = winrt::Windows::System::Launcher::LaunchUriAsync(
            winrt::Windows::Foundation::Uri{
                winrt::to_hstring(launch_state.return_uri())});
        // Keep the UI apartment responsive until the frontend handoff
        // completes, rather than exiting before Windows handles the URI.
        while (operation.Status() ==
               winrt::Windows::Foundation::AsyncStatus::Started) {
          CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(
              CoreProcessEventsOption::ProcessAllIfPresent);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        XELOGI("UWP launch frontend handoff accepted: {}", operation.GetResults());
      } catch (const winrt::hresult_error& error) {
        XELOGE("UWP launch frontend handoff failed: 0x{:08X}",
               static_cast<uint32_t>(error.code().value));
      }
    }
    // The upstream application teardown ends the process, so complete any
    // requested frontend handoff before invoking it.
    UWP::ShutdownXenia();
    CoreApplication::Exit();
  }

  void OnActivate(
      const winrt::Windows::ApplicationModel::Core::CoreApplicationView& v,
      const winrt::Windows::ApplicationModel::Activation::IActivatedEventArgs&
          args) {
    if (args.Kind() == winrt::Windows::ApplicationModel::Activation::
                           ActivationKind::Protocol) {
      auto protocol_args =
          args.as<winrt::Windows::ApplicationModel::Activation::
                      ProtocolActivatedEventArgs>();
      auto query = protocol_args.Uri().QueryParsed();
      std::vector<std::pair<std::string, std::string>> arguments;
      arguments.reserve(query.Size());
      winrt::hstring disctoken, netcred;
      bool show_fps = false;
      for (const auto& argument : query) {
        arguments.emplace_back(winrt::to_string(argument.Name()),
                               winrt::to_string(argument.Value()));
        if (argument.Name() == L"disctoken") {
          // Game Vault: the game file, shared through the console
          disctoken = argument.Value();
        } else if (argument.Name() == L"netcred") {
          // Game Vault: the sign-in for a game played from the network
          netcred = argument.Value();
        } else if (argument.Name() == L"fps") {
          // Game Vault: its "Show frame rate" option is on
          show_fps = argument.Value() == L"1";
        }
      }
      UWP::SetShowFps(show_fps);
      auto activation = UWP::ParseProtocolActivation(arguments);
      std::string game_path = activation.game_path.value_or(std::string{});
      // Game Vault: on App type "App" (Dev Home) Xenia gets only 1 GB, and a game stops at once ("Couldn't
      // allocate context", "Failed to create the EDRAM buffer"; an update can set it back to App). Back to Game
      // Vault, which says on the TV what to change, instead of a crash.
      // "apptype-test.txt" in LocalState: acts as App type "App" on a console set to Game (a test)
      std::wstring test_file =
          std::wstring(ApplicationData::Current().LocalFolder().Path()) + L"\\apptype-test.txt";
      bool low_test = GetFileAttributesW(test_file.c_str()) != INVALID_FILE_ATTRIBUTES;
      if (!game_path.empty() && activation.launch_on_exit.rfind("gamevault:", 0) == 0 &&
          (low_test || winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() < (2048ull << 20))) {
        winrt::Windows::System::Launcher::LaunchUriAsync(
            winrt::Windows::Foundation::Uri(L"gamevault:?from=lowmem-xenia"))
            .Completed([](auto const&, auto) { CoreApplication::Exit(); });
        return;
      }
      if (!game_path.empty() && !disctoken.empty()) {
        ShareFromGameVault(game_path, disctoken);
      }
      if (game_path.rfind("smb://", 0) == 0) {
        // Game Vault: a game on a PC's shared folder or a NAS, read over the network (NetGame.cpp)
        game_path = NetGame::Open(game_path, winrt::to_string(netcred));
      }
      launch_state.ActivateProtocol(std::move(activation.launch_on_exit));
      UWP::SetAutomaticLaunch(game_path);
    } else if (args.Kind() == winrt::Windows::ApplicationModel::Activation::
                                  ActivationKind::Launch) {
      restart_activation_arguments =
          winrt::to_string(args.as<winrt::Windows::ApplicationModel::
                                       Activation::LaunchActivatedEventArgs>()
                               .Arguments());
    }

    auto appView = winrt::Windows::UI::ViewManagement::ApplicationView::
        GetForCurrentView();
    appView.PreferredLaunchWindowingMode(
        winrt::Windows::UI::ViewManagement::ApplicationViewWindowingMode::
            FullScreen);

    CoreWindow window = CoreWindow::GetForCurrentThread();
    window.Activate();
    appView.TryEnterFullScreenMode();
  }

  void OnCharacterReceived(
      winrt::Windows::UI::Core::CoreWindow const& /* sender */,
      winrt::Windows::UI::Core::CharacterReceivedEventArgs const&
          e /* args */) {
    UWP::HandleCharacter(e.KeyCode());
  }

  void SetWindow(CoreWindow const& window) {
    // Game Vault: the whole screen, not the Xbox's TV-safe area (by default an app is drawn with a border
    // round it, which left the picture smaller than the screen; xemu and Game Vault ask the same)
    winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView().SetDesiredBoundsMode(
        winrt::Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
    window.CharacterReceived({this, &App::OnCharacterReceived});
    // Key events feed the caret keys of the ImGui text input.
    window.KeyDown([](CoreWindow const&,
                      winrt::Windows::UI::Core::KeyEventArgs const& e) {
      UWP::HandleKey(static_cast<uint32_t>(e.VirtualKey()), true);
    });
    window.KeyUp([](CoreWindow const&,
                    winrt::Windows::UI::Core::KeyEventArgs const& e) {
      UWP::HandleKey(static_cast<uint32_t>(e.VirtualKey()), false);
    });
    window.Closed([](auto const&, auto const&) { UWP::RequestQuit(); });
  }
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  winrt::init_apartment();
  CoreApplication::Run(make<App>());
  winrt::uninit_apartment();
}
