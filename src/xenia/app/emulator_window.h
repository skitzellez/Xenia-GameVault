/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_EMULATOR_WINDOW_H_
#define XENIA_APP_EMULATOR_WINDOW_H_

#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/app/gamerpic_browser.h"
#include "xenia/app/game_patch_dialog.h"
#include "xenia/app/game_title_update_dialog.h"
#include "xenia/app/frontend_video_player.h"
#include "xenia/app/netplay_settings_dialog.h"
#include "xenia/app/profile_dialogs.h"
#include "xenia/app/updater.h"
#include "xenia/app/updater_dialog.h"
#if XE_PLATFORM_WINRT
#include "xenia/app/guide_dialog.h"
#endif
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/kernel/xam/xam_loader_data.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/host_guide_navigation.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/menu_item.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/xbox.h"

namespace xe {
namespace app {

class ConsoleSettingsDialog;
class DsuIntegration;
class ScreenScraperFrontend;
namespace screenscraper { class Service; }
class ContentListDialog;

struct RecentTitleEntry {
  std::string title_name;
  std::filesystem::path path_to_file;
  std::time_t last_run_time;
};

class EmulatorWindow {
 public:
  using steady_clock = std::chrono::steady_clock;  // stdlib steady clock

  enum : size_t {
    // The UI is on top of the game and is open in special cases, so
    // lowest-priority.
    kZOrderHidInput,
    kZOrderImGui,
    kZOrderProfiler,
    // Emulator window controls are expected to be always accessible by the
    // user, so highest-priority.
    kZOrderEmulatorWindowInput,
  };

  virtual ~EmulatorWindow();

  void ShutdownUpdaterDialog();

  static std::unique_ptr<EmulatorWindow> Create(
      Emulator* emulator, ui::WindowedAppContext& app_context, uint32_t width,
      uint32_t height);

  std::unique_ptr<xe::threading::Thread> Gamepad_HotKeys_Listener;

  int32_t selected_title_index = -1;

  static constexpr int64_t diff_in_ms(
      const steady_clock::time_point t1,
      const steady_clock::time_point t2) noexcept {
    using ms = std::chrono::milliseconds;
    return std::chrono::duration_cast<ms>(t1 - t2).count();
  }

  steady_clock::time_point last_mouse_up = steady_clock::now();
  steady_clock::time_point last_mouse_down = steady_clock::now();

  Emulator* emulator() const { return emulator_; }
  ui::WindowedAppContext& app_context() const { return app_context_; }
  ui::Window* window() const { return window_.get(); }
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_.get(); }
  std::shared_ptr<ui::ImGuiDrawer> imgui_drawer_shared() const {
    return imgui_drawer_;
  }

  ui::Presenter* GetGraphicsSystemPresenter() const;
  void SetupGraphicsSystemPresenterPainting();
  void ShutdownGraphicsSystemPresenterPainting();

  void OnEmulatorInitialized();
#if XE_PLATFORM_WINRT
  void ShutdownDsu();
  void WaitForTitleClose();
  void SelectRemoteGame(const std::filesystem::path& path, const std::string& name);
  std::filesystem::path SelectedFrontendGame() const;
  screenscraper::Service& screen_scraper_service();
#endif

  xe::X_STATUS RunTitle(const std::filesystem::path& path_to_file);
  void UpdateTitle();
  void SetFullscreen(bool fullscreen);
  void ToggleFullscreen();
  void SetAutoCheckForUpdates(const bool state);
  void SetInitializingShaderStorage(bool initializing);

  void TakeScreenshot();
  void ExportScreenshot(const xe::ui::RawImage& image);
  void SaveImage(const std::filesystem::path& path,
                 const xe::ui::RawImage& image);

  void UpdateCompletionNotification();

  void ToggleProfilesConfigDialog();
  void ToggleGamerpicBrowserDialog();
  void ToggleXMPConfigDialog();
  void ToggleConsoleSettingsDialog();
  void ToggleContentListDialog();
  // Toggles the content list for one title ID, 0 lists all.
  void ToggleContentListDialogForTitle(uint32_t title_id);
  void ToggleFriendsDialog(bool guide_style = false);
  void ToggleUpdaterDialog();
  void ToggleCompletionDialog();
  void ToggleNetplaySettingsDialog();
  void ToggleNetplayStatusDialog();

  void SetHotkeysState(bool enabled) { disable_hotkeys_ = !enabled; }

  void ExtractContent(const std::filesystem::path file = "");
  // Extracts the packages into extract_dir with a progress dialog.
  void ExtractContentTo(const std::vector<std::filesystem::path>& package_files,
                        const std::filesystem::path& extract_dir);

  // Types of button functions for hotkeys.
  enum class ButtonFunctions {
    ToggleFullscreen,
    RunTitle,
    CpuTimeScalarSetHalf,
    CpuTimeScalarSetDouble,
    CpuTimeScalarReset,
    ClearGPUCache,
    ToggleControllerVibration,
    ClearMemoryPageState,
    ReadbackResolve,
    ToggleLogging,
    IncTitleSelect,
    DecTitleSelect,
    Unknown
  };

  class ControllerHotKey {
   public:
    // If true the hotkey can be activated while a title is running, otherwise
    // false.
    bool title_passthru;

    // If true vibrate the controller after activating the hotkey, otherwise
    // false.
    bool rumble;
    std::string pretty;
    ButtonFunctions function;

    ControllerHotKey(ButtonFunctions fn = ButtonFunctions::Unknown,
                     std::string pretty = "", bool rumble = false,
                     bool active = true) {
      function = fn;
      this->pretty = pretty;
      title_passthru = active;
      this->rumble = rumble;
    }
  };

 private:
  class EmulatorWindowListener final : public ui::WindowListener,
                                       public ui::WindowInputListener {
   public:
    explicit EmulatorWindowListener(EmulatorWindow& emulator_window)
        : emulator_window_(emulator_window) {}

    void OnClosing(ui::UIEvent& e) override;
    void OnFileDrop(ui::FileDropEvent& e) override;

    void OnKeyDown(ui::KeyEvent& e) override;

    void OnMouseDown(ui::MouseEvent& e) override;
    void OnMouseUp(ui::MouseEvent& e) override;

    void OnUsbDeviceChanged(bool is_arrival) override;

   private:
    EmulatorWindow& emulator_window_;
  };

  class DisplayConfigGameConfigLoadCallback
      : public Emulator::GameConfigLoadCallback {
   public:
    DisplayConfigGameConfigLoadCallback(Emulator& emulator,
                                        EmulatorWindow& emulator_window)
        : Emulator::GameConfigLoadCallback(emulator),
          emulator_window_(emulator_window) {}

    void PostGameConfigLoad() override;

   private:
    EmulatorWindow& emulator_window_;
  };

  class ContentInstallDialog final : public ui::ImGuiDialog {
   public:
    ContentInstallDialog(
        ui::ImGuiDrawer* imgui_drawer, EmulatorWindow& emulator_window,
        std::shared_ptr<std::vector<Emulator::ContentInstallEntry>> entries)
        : ui::ImGuiDialog(imgui_drawer),
          emulator_window_(emulator_window),
          installation_entries_(entries) {
      window_id_ = GetWindowId();
    }

    ~ContentInstallDialog() {
      for (auto& entry : *installation_entries_) {
        entry.icon_.release();
      }
    }

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    uint64_t window_id_;

    EmulatorWindow& emulator_window_;
    std::shared_ptr<std::vector<Emulator::ContentInstallEntry>>
        installation_entries_;
#if XE_PLATFORM_WINRT
    GuideDialogState guide_state_;
    bool guide_popup_opened_ = false;
#endif
  };

  class DisplayConfigDialog final : public ui::ImGuiDialog {
   public:
    DisplayConfigDialog(ui::ImGuiDrawer* imgui_drawer,
                        EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };

  class XMPConfigDialog final : public ui::ImGuiDialog {
   public:
    XMPConfigDialog(ui::ImGuiDrawer* imgui_drawer,
                    EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {
      if (emulator_window_.emulator_->audio_media_player()) {
        volume_ = emulator_window_.emulator_->audio_media_player()
                      ->GetVolume()
                      ->load();
      }
    }

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
    float volume_ = 0.0f;
  };

#if XE_PLATFORM_WINRT
  enum class HostGuideAction {
    kContinue,
    kFriends = 1,
    kProfiles,
    kNetworkSettings,
    kNetworkStatus,
    kGamerpics,
    kRestartGame,
    kQuitGame,
    kExitXenia,
  };

  class WinRTHostGuideDialog final : public ui::ImGuiDialog {
    friend class EmulatorWindow;

   public:
    WinRTHostGuideDialog(ui::ImGuiDrawer* drawer, EmulatorWindow& window)
        : ui::ImGuiDialog(drawer),
          emulator_window_(window),
          background_dialog_count_(drawer->GetDialogCount() - 1) {}

   protected:
    void OnDraw(ImGuiIO& io) override;
    void OnClose() override;

   private:
    EmulatorWindow& emulator_window_;
    HostGuideNavigation navigation_;
    size_t background_dialog_count_ = 0;
    bool focus_requested_ = true;
    std::optional<HostGuideAction> close_action_;
    std::shared_ptr<ui::ImmediateTexture> background_;
    std::string background_texture_path_;
    std::shared_ptr<ui::ImmediateTexture> button_a_;
    std::shared_ptr<ui::ImmediateTexture> button_b_;
  };

  void BeginTitleClose(
      std::optional<kernel::xam::LoaderData> restart = std::nullopt);
  void CompleteTitleClose();
  // Every UI frame: the DSU link, and Game Vault's "another game while one runs" (see the .cc)
  void CompanionTick();
  void ShowHostGuide();
  void HostGuideClosed(WinRTHostGuideDialog* dialog);
  void HostGuideOpenPage(WinRTHostGuideDialog* dialog, HostGuideAction action);
  void ExecuteHostGuideAction(HostGuideAction action);

  class WinRTFrontendDialog final : public ui::ImGuiDialog {
   public:
    WinRTFrontendDialog(ui::ImGuiDrawer* imgui_drawer,
                        EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}
    ~WinRTFrontendDialog() override;
    void SelectRemoteGame(const std::string& path, const std::string& name) {
      selected_game_path_ = path;
      selected_game_name_ = name;
      show_search_panel_ = false;
      search_panel_list_focus_requested_ = false;
      search_selected_game_path_.clear();
      active_frontend_page_ = FrontendPage::kGameList;
      remote_focus_requested_ = true;
    }
    const std::string& selected_path() const { return selected_game_path_; }
    void StopVideoBackground();

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    enum class FrontendPage {
      kGameList = 0,
      kSettings,
      kPaths,
      kAbout,
      kCount,
    };

    enum class ActionPopupMode {
      kInfo,
      kManualPatchPrompt,
      kManualPluginPrompt,
      kManualConfigPrompt,
      kExistingDownloadPrompt,
      kPerGameConfigPrompt,
    };

    enum class ManualInstallKind { kNone, kPatch, kPlugin, kConfig };

    enum class PerGameConfigValueType {
      kUnknown,
      kBool,
      kInt,
      kUInt,
      kUInt64,
      kDouble,
      kString,
      kPath,
    };

    enum class PerGameConfigFocusSide {
      kNone,
      kLeft,
      kRight,
    };

    struct PerGameConfigOption {
      std::string name;
      std::string category;
      std::string description;
      std::string default_value;
      PerGameConfigValueType value_type = PerGameConfigValueType::kUnknown;
      std::vector<std::string> enum_values;
    };

    struct PerGameConfigEntry {
      std::string category;
      std::string value;
    };

    void OpenPatchEditor(const std::filesystem::path& directory,
                         const std::string& title_id);
    std::shared_ptr<int> async_lifetime_ = std::make_shared<int>(0);
    std::unique_ptr<GamePatchDialog> patch_editor_;
    std::unique_ptr<GameTitleUpdateDialog> title_update_dialog_;
    enum class StorageFolderTarget { kPrimaryGames, kAdditionalGames, kConfig, kMedia };
    struct StorageFolderResult {
      std::mutex mutex;
      StorageFolderTarget target;
      bool ready = false;
      std::string path;
      std::string error;
    };
    void ApplyStorageFolder(StorageFolderTarget target, const std::string& path);
    void PickStorageFolder(StorageFolderTarget target);
    void CompleteStorageFolderPicker();
    void DrawNoProfilePrompt(ImGuiIO& io);

    std::shared_ptr<ui::ImmediateTexture> GetOrCreateBackground();
    std::shared_ptr<ui::ImmediateTexture> GetOrCreateThemeTexture(
        const std::string& path, std::string& loaded_path,
        std::shared_ptr<ui::ImmediateTexture>& texture);
    std::shared_ptr<ui::ImmediateTexture> GetOrCreateBackgroundFallback();
    std::shared_ptr<ui::ImmediateTexture> GetOrCreateImageTexture(
        const std::string& image_path);
    std::shared_ptr<ui::ImmediateTexture> GetOrCreateXeniaLogo();
    std::shared_ptr<ui::ImmediateTexture> GetOrCreateButtonTexture(char button);

    EmulatorWindow& emulator_window_;
    std::shared_ptr<ui::ImmediateTexture> background_tex_;
    std::shared_ptr<ui::ImmediateTexture> background_fallback_tex_;
    std::shared_ptr<ui::ImmediateTexture> xenia_logo_tex_;
    std::string background_texture_path_;
    std::string background_fallback_texture_path_;
    std::string xenia_logo_texture_path_;
    std::shared_ptr<ui::ImmediateTexture> button_a_tex_;
    std::shared_ptr<ui::ImmediateTexture> button_b_tex_;
    std::shared_ptr<ui::ImmediateTexture> button_x_tex_;
    std::shared_ptr<ui::ImmediateTexture> button_y_tex_;
    std::unique_ptr<FrontendVideoPlayer> background_video_;
    std::shared_ptr<ui::ImmediateTexture> background_video_texture_;
    std::string background_video_path_;
    std::string background_video_error_;
    uint64_t artwork_revision_ = 0;
    std::shared_ptr<StorageFolderResult> storage_folder_result_;
    std::string storage_path_status_;
    bool storage_focus_requested_ = false;
    bool remote_focus_requested_ = false;
    std::string selected_game_path_;
    std::string selected_game_name_;
    std::string metadata_scroll_path_;
    double metadata_scroll_elapsed_ = 0;
    float metadata_scroll_range_ = 0;
    int metadata_scroll_frame_ = -1;
    FrontendPage active_frontend_page_ = FrontendPage::kGameList;
    bool show_action_status_ = false;
    std::string action_status_;
    ActionPopupMode action_popup_mode_ = ActionPopupMode::kInfo;
    ManualInstallKind pending_manual_kind_ = ManualInstallKind::kNone;
    std::filesystem::path pending_manual_destination_;
    std::string pending_manual_title_id_;
    ManualInstallKind pending_existing_download_kind_ =
        ManualInstallKind::kNone;
    std::filesystem::path pending_existing_download_destination_;
    std::string pending_existing_download_title_id_;
    bool action_popup_should_close_ = false;
    bool action_popup_focus_requested_ = false;
    bool show_per_game_config_editor_ = false;
    bool per_game_config_popup_focus_requested_ = false;
    std::filesystem::path pending_per_game_config_path_;
    std::string pending_per_game_config_title_id_;
    bool per_game_config_remove_mode_ = false;
    std::string per_game_config_add_candidate_;
    std::string per_game_config_add_preview_option_;
    std::string selected_per_game_config_option_;
    std::string pending_per_game_config_title_display_;
    PerGameConfigFocusSide per_game_config_focus_jump_request_ =
        PerGameConfigFocusSide::kNone;
    std::string per_game_config_last_focused_control_id_;
    std::string per_game_config_control_jump_request_;
    std::vector<PerGameConfigOption> per_game_config_options_;
    std::map<std::string, PerGameConfigEntry> per_game_config_entries_;
    char per_game_config_buffer_[65536] = {};
    char cl_buffer_[128] = {};
    std::unordered_map<std::string, std::string> cached_game_title_ids_;
    std::unordered_map<std::string, std::shared_ptr<ui::ImmediateTexture>>
        game_image_textures_;
    bool show_search_panel_ = false;
    bool search_panel_focus_input_requested_ = false;
    bool search_panel_list_focus_requested_ = false;
    bool show_game_context_menu_ = false;
    bool game_context_menu_focus_requested_ = false;
    bool search_gamepad_y_was_down_ = false;
    std::string search_selected_game_path_;
    char search_input_buffer_[128] = {};
    bool no_profile_prompt_dismissed_ = false;
  };

#endif  // XE_PLATFORM_WINRT

  explicit EmulatorWindow(Emulator* emulator,
                          ui::WindowedAppContext& app_context, uint32_t width,
                          uint32_t height);
  bool Initialize();

  // For comparisons, use GetSwapPostEffectForCvarValue instead as the default
  // fallback may be used for multiple values.
  static const char* GetCvarValueForSwapPostEffect(
      gpu::CommandProcessor::SwapPostEffect effect);
  static gpu::CommandProcessor::SwapPostEffect GetSwapPostEffectForCvarValue(
      const std::string& cvar_value);
  // For comparisons, use GetGuestOutputPaintEffectForCvarValue instead as the
  // default fallback may be used for multiple values.
  static const char* GetCvarValueForGuestOutputPaintEffect(
      ui::Presenter::GuestOutputPaintConfig::Effect effect);
  static ui::Presenter::GuestOutputPaintConfig::Effect
  GetGuestOutputPaintEffectForCvarValue(const std::string& cvar_value);
  static ui::Presenter::GuestOutputPaintConfig
  GetGuestOutputPaintConfigForCvars();
  void ApplyDisplayConfigForCvars();

  void OnKeyDown(ui::KeyEvent& e);
  void OnMouseDown(const ui::MouseEvent& e);
  void ToggleFullscreenOnDoubleClick();
  void FileDrop(const std::filesystem::path& filename);
  void OnMouseUp(const ui::MouseEvent& e);
  void FileOpen();
  void FileClose();
  struct DlcInstallContext {
    std::filesystem::path source_path;
    Emulator::ContentInstallEntry entry;
    std::string description;
  };

  enum class DlcInstallMode { kManual, kAutoDownload };

  void InstallContent();
  void InstallContentForTitle(const std::string& title_id,
                              const std::filesystem::path& storage_root,
                              DlcInstallMode mode);
  void ExtractZarchive();
  void CreateZarchive();
  void ShowContentDirectory();
  void DumpXLast();
  void CpuTimeScalarReset();
  void CpuTimeScalarSetHalf();
  void CpuTimeScalarSetDouble();
  void CpuBreakIntoDebugger();
  void CpuBreakIntoHostDebugger();
  void GpuTraceFrame();
  void GpuClearCaches();
  void ToggleDisplayConfigDialog();
  void ToggleControllerVibration();
  void ShowCompatibility();
  void ShowFAQ();
  void ShowBuildCommit();
  void ShowUpdateAvailableDialog(const std::string& commit,
                                 const std::string& date);
  EmulatorWindow::ControllerHotKey ProcessControllerHotkey(int buttons);
  void VibrateController(xe::hid::InputSystem* input_sys, uint32_t user_index,
                         bool vibrate = true);
  void GamepadHotKeys();
  void ToggleGPUSetting(gpu::GPUSetting setting);
  void CycleReadbackResolve();
  void DisplayHotKeysConfig();

  static std::string CanonicalizeFileExtension(
      const std::filesystem::path& path);

  void RunPreviouslyPlayedTitle();
  void FillRecentlyLaunchedTitlesMenu(xe::ui::MenuItem* recent_menu);
  void LoadRecentlyLaunchedTitles();
  void AddRecentlyLaunchedTitle(std::filesystem::path path_to_file,
                                std::string title_name);

  void ClearDialogs();

  friend class ProfileConfigDialog;

  Emulator* emulator_;
  ui::WindowedAppContext& app_context_;
  EmulatorWindowListener window_listener_;
  std::unique_ptr<ui::Window> window_;
  // shared_ptr needed for gamerpic browser
  std::shared_ptr<ui::ImGuiDrawer> imgui_drawer_;
  std::unique_ptr<DisplayConfigGameConfigLoadCallback>
      display_config_game_config_load_callback_;
  // Creation may fail, in this case immediate drawer UI must not be drawn.
  std::unique_ptr<ui::ImmediateDrawer> immediate_drawer_;

  bool emulator_initialized_ = false;
  std::atomic<bool> disable_hotkeys_ = false;

  std::string base_title_;
  bool initializing_shader_storage_ = false;

  std::shared_ptr<Updater> updater_;
  std::shared_future<CheckForUpdateInfo> update_info_;
  std::atomic<bool> cancel_request;
#if XE_PLATFORM_WINRT
  std::unique_ptr<EmulatorWindow::WinRTFrontendDialog> gamelist_;
  std::unique_ptr<DsuIntegration> dsu_;
  std::unique_ptr<ScreenScraperFrontend> screenscraper_;
  std::future<X_STATUS> title_close_result_;
  std::optional<kernel::xam::LoaderData> title_restart_data_;
  // Game Vault: the running game is closing because Game Vault sent another one
  bool game_switch_pending_ = false;
  std::unique_ptr<WinRTHostGuideDialog> host_guide_;
  std::shared_ptr<int> host_guide_lifetime_ = std::make_shared<int>(0);
#endif

  std::unique_ptr<DisplayConfigDialog> display_config_dialog_;
  std::unique_ptr<ConsoleSettingsDialog> console_settings_dialog_;
  std::unique_ptr<ContentListDialog> content_list_dialog_;
  // Storing pointers and toggling dialog state is useful for broadcasting
  // messages back to guest.
  std::unique_ptr<ProfileConfigDialog> profile_config_dialog_;

  std::unique_ptr<TitleGamerpicBrowser> gamerpic_browser_dialog_;

  std::unique_ptr<XMPConfigDialog> xmp_config_dialog_;

  std::unique_ptr<ManagerDialog> friends_manager_dialog_;

  std::unique_ptr<UpdaterDialog> updater_dialog_;

  std::unique_ptr<UpdaterCompletionDialog> updater_completion_dialog_;

  std::unique_ptr<NetplaySettingsDialog> netplay_settings_dialog_;

  std::unique_ptr<NetplayStatusDialog> netplay_status_dialog_;

  std::vector<RecentTitleEntry> recently_launched_titles_;

  void DetachProfileConfigDialog(ProfileConfigDialog* dialog);
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_EMULATOR_WINDOW_H_
