#define NOMINMAX

#include <windows.h>
#include <unknwn.h>

#include <iostream>
#include <fstream> 
#include <sstream> 
#include <string>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/windows.graphics.display.core.h>

#include <Gamingdeviceinformation.h>
#include <thread>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

#include "xenia/base/filesystem.h"
#include "XeniaUWP.h"
#include "UWPUtil.h"
#include "WinRTKeyboard.h"
#include "NetGame.h"

using namespace winrt;

using namespace Windows;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation::Numerics;
using namespace Windows::UI;
using namespace Windows::UI::Core;
using namespace Windows::UI::Composition;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;

// IStorageItemHandleAccess (WindowsStorageCOM.h, which does not build for apps here): a Win32 handle to a
// StorageFile. Declared as the SDK has it.
struct __declspec(uuid("5CA296B2-2C25-4D22-B785-B885C8201E6A")) GvStorageItemHandleAccess : ::IUnknown {
    virtual HRESULT __stdcall Create(DWORD access, DWORD sharing, DWORD options, void* oplock, HANDLE* handle) = 0;
};

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
    CompositionTarget m_target{ nullptr };
    VisualCollection m_visuals{ nullptr };
    Visual m_selected{ nullptr };
    float2 m_offset{};
    hstring m_launchOnExit;

    IFrameworkView CreateView()
    {
        return *this;
    }

    void Initialize(CoreApplicationView const& v)
    {
      v.Activated({this, &App::OnActivate});
    }

    void Load(hstring const&)
    {
    }

    void Uninitialize()
    {
    }

    void Run()
    {
        auto navigation = winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();

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

        while (true) {
          CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(
              CoreProcessEventsOption::ProcessAllIfPresent);

          UWP::ExecutePendingFunctionsFromUIThread();

          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        CoreDispatcher dispatcher =
            CoreWindow::GetForCurrentThread().Dispatcher();
        dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessUntilQuit);

        if (!m_launchOnExit.empty()) {
          winrt::Windows::Foundation::Uri m_uri{m_launchOnExit};
          auto asyncOperation =
              winrt::Windows::System::Launcher::LaunchUriAsync(m_uri);
          asyncOperation.Completed(
              [](winrt::Windows::Foundation::IAsyncOperation<bool> const&
                     sender,
                 winrt::Windows::Foundation::AsyncStatus const asyncStatus) {
                CoreApplication::Exit();
                return;
              });
        }
    }

    // Game Vault (Xenia for Game Vault): the launcher shares the game file through the console and sends the
    // token with the link. Opened here, Xenia reads the game through that handle (xe::filesystem shared file):
    // opening a USB drive's file by path can wait for good on a permission question the console never shows.
    // Runs on a thread of its own: blocking on WinRT calls is not allowed on this one.
    static void ShareFromGameVault(const std::string& path, const winrt::hstring& token) {
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

    void OnActivate(
        const winrt::Windows::ApplicationModel::Core::CoreApplicationView& v,
        const winrt::Windows::ApplicationModel::Activation::IActivatedEventArgs&
            args) {
        std::stringstream filePath;
        winrt::hstring disctoken, netcred;

        if (args.Kind() ==
            Windows::ApplicationModel::Activation::ActivationKind::Protocol) {
          auto protocolActivatedEventArgs{
              args.as<Windows::ApplicationModel::Activation::
                          ProtocolActivatedEventArgs>()};
          auto query = protocolActivatedEventArgs.Uri().QueryParsed();

          for (uint32_t i = 0; i < query.Size(); i++) {
            auto arg = query.GetAt(i);

            // parse command line string
            if (arg.Name() == winrt::hstring(L"cmd")) {
              std::string argVal = winrt::to_string(arg.Value());

              // Strip the executable from the cmd argument
              if (argVal.rfind("xeniacanary.exe", 0) == 0) {
                argVal = argVal.substr(15, argVal.length());
              }

              std::istringstream iss(argVal);
              std::string s;

              // Maintain slashes while reading the quotes
              while (iss >> std::quoted(s, '"', (char)0)) {
                filePath << s;
              }
            } else if (arg.Name() == winrt::hstring(L"launchOnExit")) {
              // For if we want to return to a frontend
              m_launchOnExit = arg.Value();
            } else if (arg.Name() == winrt::hstring(L"disctoken")) {
              // Game Vault: the game file, shared through the console
              disctoken = arg.Value();
            } else if (arg.Name() == winrt::hstring(L"netcred")) {
              // Game Vault: the sign-in for a game played from the network
              netcred = arg.Value();
            }
          }
        }

        std::string gamePath = filePath.str();
        if (!gamePath.empty() && !disctoken.empty()) {
          ShareFromGameVault(gamePath, disctoken);
        }
        if (gamePath.rfind("smb://", 0) == 0) {
          // Game Vault: a game on a PC's shared folder or a NAS, read over the network (NetGame.cpp)
          gamePath = NetGame::Open(gamePath, winrt::to_string(netcred));
        }
        if (!gamePath.empty() && gamePath != "") {
          UWP::SetAutomaticLaunch(gamePath);
        }

        auto appView = winrt::Windows::UI::ViewManagement::
            ApplicationView::GetForCurrentView();
        appView.PreferredLaunchWindowingMode(
            winrt::Windows::UI::ViewManagement::
                ApplicationViewWindowingMode::FullScreen);

        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();
        appView.TryEnterFullScreenMode();
    }

    void OnCharacterReceived(
        winrt::Windows::UI::Core::CoreWindow const& /* sender */,
        winrt::Windows::UI::Core::
            CharacterReceivedEventArgs const& e /* args */) {
        UWP::HandleCharacter(e.KeyCode());
    }

    void SetWindow(CoreWindow const & window)
    {
        // Game Vault: the whole screen, not the Xbox's TV-safe area (by default an app is drawn with a border
        // round it, which left the picture smaller than the screen; xemu and Game Vault ask the same)
        winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView().SetDesiredBoundsMode(
            winrt::Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
        window.CharacterReceived({this, &App::OnCharacterReceived});
    }
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    winrt::init_apartment();
    CoreApplication::Run(make<App>());
    winrt::uninit_apartment();
}
