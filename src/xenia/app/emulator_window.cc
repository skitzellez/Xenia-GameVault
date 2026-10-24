/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/emulator_window.h"
#if XE_PLATFORM_WINRT
#include "xenia/app/dsu_integration.h"
#include "xenia/app/game_patch_dialog.h"
#include "xenia/app/game_title_update_dialog.h"
#include "xenia/patcher/patcher.h"
#include "xenia/app/screenscraper_frontend.h"
#endif
#include "xenia/app/guide_dialog.h"
#include "xenia/app/frontend_theme.h"
#include "xenia/ui/frontend_selection.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/imgui/imgui.h"
#include "third_party/stb/stb_image.h"
#include "third_party/stb/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wabsolute-value"
#endif
#include "third_party/tomlplusplus/toml.hpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "xenia/app/console_settings_dialog.h"
#include "xenia/app/content_list_dialog.h"
#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/system.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xam_title_restart.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/ui_event.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/virtual_file_system.h"

#include "version.h"
#if XE_PLATFORM_WINRT
#include "xenia-canary-uwp/UWPUtil.h"
#include "xenia-canary-uwp/WinRTKeyboard.h"
#include "xenia-canary-uwp/XeniaUWP.h"
#include "xenia-canary-uwp/game_storage.h"
#endif

#include "xenia/app/ui_text_effect_helpers.h"
#include "xenia/base/filesystem.h"
#include "xenia/config.h"
#include "xenia/kernel/xam/ui/create_profile_ui.h"
#include "xenia/kernel/xconfig.h"

DECLARE_bool(debug);

DECLARE_string(hid);
#if XE_PLATFORM_WINRT
DECLARE_path(target);
DECLARE_string(ui_dynamic_background_type);
#endif

DECLARE_bool(guide_button);

DECLARE_bool(clear_memory_page_state);

DECLARE_string(readback_resolve);

DECLARE_bool(readback_memexport);

DECLARE_string(api_address);

DECLARE_string(api_list);

DECLARE_bool(upnp);

DECLARE_string(network_guid);

DEFINE_bool(fullscreen, false, "Whether to launch the emulator in fullscreen.",
            "Display");

DEFINE_bool(controller_hotkeys, false, "Hotkeys for Xbox and PS controllers.",
            "General");

DECLARE_bool(skip_frontend);

DEFINE_bool(skip_frontend, false,
            "Skip the UWP frontend and launch with a file-picker.", "General");

DEFINE_bool(auto_check_updates, true,
            "Automatically check for updates on startup and notify if any are "
            "available.",
            "General");

DEFINE_bool(ui_show_genre_metadata, true,
            "Show genre metadata on the Games tab.", "UI");
DEFINE_bool(ui_show_developer_metadata, true,
            "Show developer metadata on the Games tab.", "UI");
DEFINE_bool(
    ui_show_title_metadata, true,
    "Show the selected game's title in the metadata panel on the Games tab.",
    "UI");
DEFINE_bool(ui_show_title_id_metadata, true,
            "Show Title IDs in the Games tab details panel.", "UI");
DEFINE_bool(ui_show_release_date_metadata, true,
            "Show release dates on the Games tab.", "UI");
DEFINE_bool(ui_show_user_rating_metadata, true,
            "Show user ratings on the Games tab.", "UI");

DEFINE_string(ui_color_theme, "green", "Base frontend color theme.", "UI");
DEFINE_string(ui_interface_overlay_color, "green",
              "Color of the interface overlay and Xenia logo.", "UI");
DEFINE_string(ui_background_color, "green",
              "Color of the static frontend background.", "UI");

DEFINE_bool(ui_hide_tabs_text, false,
            "Hide the text labels for tabs (games, settings, paths, about) in "
            "the frontend.",
            "UI");

DEFINE_string(ui_guide_color, "green", "Color of the guide panels.", "UI");

DEFINE_string(ui_text_effect, "stroke_fill",
              "Text effect to apply globally to all UI text. Options: shadow, "
              "lift, stroke_fill, glow, outline, double_shadow, bold, none.",
              "UI");

DEFINE_string(
    ui_text_color, "white",
    "Monochrome text color override for UI text. Options: white, black.", "UI");

DEFINE_string(ui_border_color, "green",
              "Accent color applied to UI borders, checkboxes, combo accents, "
              "and frontend headers. Options: green, purple, red, orange, "
              "blue, yellow, silver, black, white.",
              "UI");

DEFINE_double(ui_text_brightness, 1.0,
              "Brightness multiplier applied to UI text.", "UI");

DEFINE_double(ui_text_contrast, 1.0, "Contrast multiplier applied to UI text.",
              "UI");

DEFINE_string(
    postprocess_antialiasing, "",
    "Post-processing anti-aliasing effect to apply to the image output of the "
    "game.\n"
    "Using post-process anti-aliasing is heavily recommended when AMD "
    "FidelityFX Contrast Adaptive Sharpening or Super Resolution 1.0 is "
    "active.\n"
    "Use: [none, fxaa, fxaa_extreme]\n"
    " none (or any value not listed here):\n"
    "  Don't alter the original image.\n"
    " fxaa:\n"
    "  NVIDIA Fast Approximate Anti-Aliasing 3.11, normal quality preset (12)."
    "\n"
    " fxaa_extreme:\n"
    "  NVIDIA Fast Approximate Anti-Aliasing 3.11, extreme quality preset "
    "(39).",
    "Display");
DEFINE_string(
    postprocess_scaling_and_sharpening, "",
    "Post-processing effect to use for resampling and/or sharpening of the "
    "final display output.\n"
    "Use: [bilinear, cas, fsr]\n"
    " bilinear (or any value not listed here):\n"
    "  Original image at 1:1, simple bilinear stretching for resampling.\n"
    " cas:\n"
    "  Use AMD FidelityFX Contrast Adaptive Sharpening (CAS) for sharpening "
    "at scaling factors of up to 2x2, with additional bilinear stretching for "
    "larger factors.\n"
    " fsr:\n"
    "  Use AMD FidelityFX Super Resolution 1.0 (FSR) for highest-quality "
    "upscaling, or AMD FidelityFX Contrast Adaptive Sharpening for sharpening "
    "while not scaling or downsampling.\n"
    "  For scaling by factors of more than 2x2, multiple FSR passes are done.",
    "Display");
DEFINE_double(
    postprocess_ffx_cas_additional_sharpness,
    xe::ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessDefault,
    "Additional sharpness for AMD FidelityFX Contrast Adaptive Sharpening "
    "(CAS), from 0 to 1.\n"
    "Higher is sharper.",
    "Display");
DEFINE_uint32(
    postprocess_ffx_fsr_max_upsampling_passes,
    xe::ui::Presenter::GuestOutputPaintConfig::kFsrMaxUpscalingPassesMax,
    "Maximum number of upsampling passes performed in AMD FidelityFX Super "
    "Resolution 1.0 (FSR) before falling back to bilinear stretching after the "
    "final pass.\n"
    "Each pass upscales only to up to 2x2 the previous size. If the game "
    "outputs a 1280x720 image, 1 pass will upscale it to up to 2560x1440 "
    "(below 4K), after 2 passes it will be upscaled to a maximum of 5120x2880 "
    "(including 3840x2160 for 4K), and so on.\n"
    "This variable has no effect if the display resolution isn't very high, "
    "but may be reduced on resolutions like 4K or 8K in case the performance "
    "impact of multiple FSR upsampling passes is too high, or if softer edges "
    "are desired.\n"
    "The default value is the maximum internally supported by Xenia.",
    "Display");
DEFINE_double(
    postprocess_ffx_fsr_sharpness_reduction,
    xe::ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionDefault,
    "Sharpness reduction for AMD FidelityFX Super Resolution 1.0 (FSR), in "
    "stops.\n"
    "Lower is sharper.",
    "Display");
// Dithering to 8bpc is enabled by default since the effect is minor, only
// effects what can't be shown normally by host displays, and nothing is changed
// by it for 8bpc source without resampling.
DEFINE_bool(
    postprocess_dither, true,
    "Dither the final image output from the internal precision to 8 bits per "
    "channel so gradients are smoother.\n"
    "On a 10bpc display, the lower 2 bits will still be kept, but noise will "
    "be added to them - disabling may be recommended for 10bpc, but it "
    "depends on the 10bpc displaying capabilities of the actual display used.",
    "Display");

DEFINE_int32(recent_titles_entry_amount, 10,
             "Allows user to define how many titles is saved in list of "
             "recently played titles.",
             "General");
DEFINE_bool(disable_doubleclick_fullscreen, false,
            "Allows the user to disable the behavior where a fast double-click "
            "causes Xenia to enter fullscreen mode.",
            "General");

namespace xe {
namespace app {

using xe::ui::FileDropEvent;
using xe::ui::KeyEvent;
#if !XE_PLATFORM_WINRT
using xe::ui::MenuItem;
#endif
using xe::ui::UIEvent;

using namespace xe::hid;
using namespace xe::gpu;

constexpr std::string_view kRecentlyPlayedTitlesFilename = "recent.toml";
constexpr std::string_view kBaseTitle = "Xenia-canary-netplay";

namespace {
std::shared_ptr<ui::ImmediateTexture> LoadTextureFromFile(
    ui::ImGuiDrawer* imgui_drawer, const std::string& asset_path,
    bool transparent = false) {
  if (!imgui_drawer || asset_path.empty() ||
      !std::filesystem::exists(asset_path)) {
    return nullptr;
  }

  auto* drawer = imgui_drawer->GetImmediateDrawer();
  if (!drawer) {
    return nullptr;
  }

  int width = 0;
  int height = 0;
  int comp = 0;
  auto data = stbi_load(asset_path.c_str(), &width, &height, &comp, 4);
  if (!data || width <= 0 || height <= 0) {
    if (data) {
      stbi_image_free(data);
    }
    return nullptr;
  }

  auto tex = drawer->CreateTexture(
      static_cast<uint32_t>(width), static_cast<uint32_t>(height),
      xe::ui::ImmediateTextureFilter::kLinear, transparent, data);
  stbi_image_free(data);
  if (!tex) {
    return nullptr;
  }
  return std::shared_ptr<ui::ImmediateTexture>(tex.release());
}

}  // namespace

EmulatorWindow::EmulatorWindow(Emulator* emulator,
                               ui::WindowedAppContext& app_context,
                               uint32_t width, uint32_t height)
    : emulator_(emulator),
      app_context_(app_context),
      window_listener_(*this),
      window_(ui::Window::Create(app_context, kBaseTitle, width, height)),
      imgui_drawer_(
          std::make_shared<ui::ImGuiDrawer>(window_.get(), kZOrderImGui)),
      display_config_game_config_load_callback_(
          new DisplayConfigGameConfigLoadCallback(*emulator, *this)) {
  base_title_ = std::string(kBaseTitle) +
#ifdef DEBUG
#if _NO_DEBUG_HEAP == 1
                " DEBUG"
#else
                " CHECKED"
#endif
#endif
                " ("
#ifdef XE_BUILD_IS_PR
                "PR#" XE_BUILD_PR_NUMBER " - "
#endif
                XE_BUILD_BRANCH "@" XE_BUILD_COMMIT_SHORT " on " XE_BUILD_DATE
                ")";

  updater_ = std::make_shared<Updater>("AdrianCassar", "xenia-canary");

  LoadRecentlyLaunchedTitles();

#if XE_PLATFORM_WINRT
  if (!cvars::skip_frontend) {
    gamelist_ = std::unique_ptr<WinRTFrontendDialog>(
        new WinRTFrontendDialog(imgui_drawer_.get(), *this));
  }
#endif
}

std::unique_ptr<EmulatorWindow> EmulatorWindow::Create(
    Emulator* emulator, ui::WindowedAppContext& app_context, uint32_t width,
    uint32_t height) {
  assert_true(app_context.IsInUIThread());
  std::unique_ptr<EmulatorWindow> emulator_window(
      new EmulatorWindow(emulator, app_context, width, height));
  if (!emulator_window->Initialize()) {
    return nullptr;
  }
  return emulator_window;
}

EmulatorWindow::~EmulatorWindow() {
#if XE_PLATFORM_WINRT
  ShutdownDsu();
  if (gamelist_) gamelist_->StopVideoBackground();
  screenscraper_.reset();
  UWP::SetHostGuideAction({});
  host_guide_lifetime_.reset();
  host_guide_.reset();
#endif
  // Notify the ImGui drawer that the immediate drawer is being destroyed.
  ShutdownGraphicsSystemPresenterPainting();
}

void EmulatorWindow::ShutdownUpdaterDialog() {
  // Cancel checking for updates.
  cancel_request = true;
  updater_dialog_.reset();
}

#if XE_PLATFORM_WINRT
void EmulatorWindow::ShutdownDsu() {
  UWP::SetCompanionTick({});
  UWP::RegisterInputSystem(nullptr);
  dsu_.reset();
}

screenscraper::Service& EmulatorWindow::screen_scraper_service() {
  if (!screenscraper_) screenscraper_ = std::make_unique<ScreenScraperFrontend>();
  return screenscraper_->service();
}

void EmulatorWindow::SelectRemoteGame(const std::filesystem::path& path,
                                      const std::string& name) {
  if (gamelist_) gamelist_->SelectRemoteGame(xe::path_to_utf8(path), name);
}

std::filesystem::path EmulatorWindow::SelectedFrontendGame() const {
  return gamelist_ ? std::filesystem::u8path(gamelist_->selected_path())
                   : std::filesystem::path();
}
#endif

ui::Presenter* EmulatorWindow::GetGraphicsSystemPresenter() const {
  gpu::GraphicsSystem* graphics_system = emulator_->graphics_system();
  return graphics_system ? graphics_system->presenter() : nullptr;
}

void EmulatorWindow::SetupGraphicsSystemPresenterPainting() {
  ShutdownGraphicsSystemPresenterPainting();

  if (!window_) {
    return;
  }

  ui::Presenter* presenter = GetGraphicsSystemPresenter();
  if (!presenter) {
    return;
  }

  ApplyDisplayConfigForCvars();

  window_->SetPresenter(presenter);

  immediate_drawer_ =
      emulator_->graphics_system()->provider()->CreateImmediateDrawer();
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(presenter);
    imgui_drawer_->SetPresenterAndImmediateDrawer(presenter,
                                                  immediate_drawer_.get());
    Profiler::SetUserIO(kZOrderProfiler, window_.get(), presenter,
                        immediate_drawer_.get());
  }
}

void EmulatorWindow::ShutdownGraphicsSystemPresenterPainting() {
  Profiler::SetUserIO(kZOrderProfiler, window_.get(), nullptr, nullptr);
  imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
  immediate_drawer_.reset();
  if (window_) {
    window_->SetPresenter(nullptr);
  }
}

void EmulatorWindow::OnEmulatorInitialized() {
#if XE_PLATFORM_WINRT
  screen_scraper_service();
  UWP::RegisterInputSystem(emulator_->input_system());
  dsu_ = std::make_unique<DsuIntegration>(*this);
  UWP::SetCompanionTick([this]() { if (dsu_) dsu_->Tick(); });
  UWP::SetHostGuideAction([this]() { ShowHostGuide(); });
  emulator_->on_terminate.AddListener([this]() {
    app_context_.CallInUIThread([this]() {
      if (UWP::ShouldReturnToFrontend()) {
        UWP::RequestQuit();
        return;
      }
      if (gamelist_) {
        imgui_drawer_->AddDialog(gamelist_.get());
      }
    });
  });
  auto xam = emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
      "xam.xex");
  xam->LoadLoaderData();
  UWP::RestoreTitleRestart(!xam->loader_data().host_path.empty());
  if (!xam->loader_data().host_path.empty()) {
    // Consume restart data before a protocol launch or the frontend can run.
    UWP::ConsumeGamePath();
    XELOGI("UWP resuming saved title: {}; module: {}",
           xam->loader_data().host_path, xam->loader_data().launch_path);
    RunTitle(std::filesystem::u8path(xam->loader_data().host_path));
  } else if (!cvars::target.empty()) {
    UWP::ConsumeGamePath();
    RunTitle(cvars::target);
  } else if (cvars::skip_frontend) {
    if (UWP::HasGamePath()) {
      RunTitle(std::filesystem::u8path(UWP::ConsumeGamePath()));
    } else {
      FileOpen();
    }
  }
#endif
#if !XE_PLATFORM_WINRT
  if (!emulator_->kernel_state()
           ->xam_state()
           ->profile_manager()
           ->GetAccountCount()) {
    new NoProfileDialog(imgui_drawer_.get(), this);
    disable_hotkeys_ = true;
  }

#endif
  emulator_initialized_ = true;
  window_->SetMainMenuEnabled(true);
  // When the user can see that the emulator isn't initializing anymore (the
  // menu isn't disabled), enter fullscreen if requested.
  if (cvars::fullscreen) {
    SetFullscreen(true);
  }

  if (IsUseNexusForGameBarEnabled()) {
    XELOGE(
        "Xbox Gamebar Enabled, using BACK button instead of GUIDE for "
        "controller hotkeys!!!");
  }

  // Create a thread to listen for controller hotkeys.
  if (cvars::controller_hotkeys) {
    Gamepad_HotKeys_Listener =
        threading::Thread::Create({}, [&] { GamepadHotKeys(); });
    Gamepad_HotKeys_Listener->set_name("Gamepad HotKeys Listener");
  }

  // Check for updates
#if !XE_PLATFORM_WINRT && !defined(DEBUG) && !defined(XE_BUILD_IS_PR)
  bool should_check_update = cvars::auto_check_updates &&
                             !(cvar::updated_arg_present && cvar::updated);

  auto callback = [this](CheckForUpdateInfo update_info) {
    if (update_info.update_available) {
      app_context_.CallInUIThread([this, update_info]() {
        ShowUpdateAvailableDialog(update_info.metadata.commit_hash,
                                  update_info.metadata.commit_date);
      });
    }
  };

  if (should_check_update) {
    update_info_ = updater_->StartupUpdateCheckAsync(cancel_request, callback);
  }
#endif

  if (emulator_->kernel_state()
          ->xam_state()
          ->user_tracker()
          ->LoggedInToLive()) {
    emulator()->GetXboxLiveAPI()->StartWhoamiAsync();
  }
}

void EmulatorWindow::ShowUpdateAvailableDialog(const std::string& commit,
                                               const std::string& date) {
  std::string title_text = "Update Available";
  std::string short_commit = commit.substr(0, 9);
  std::string message = fmt::format(
      "Date: {} ({})\n\n"
      "You can update via the Netplay -> Update Checker menu",
      date, short_commit);

  new xe::ui::HostNotificationWindow(imgui_drawer_.get(), title_text, message,
                                     0, 9);
}

void EmulatorWindow::EmulatorWindowListener::OnClosing(ui::UIEvent& e) {
  emulator_window_.app_context_.QuitFromUIThread();
}

void EmulatorWindow::EmulatorWindowListener::OnFileDrop(ui::FileDropEvent& e) {
  emulator_window_.FileDrop(e.filename());
}

void EmulatorWindow::EmulatorWindowListener::OnKeyDown(ui::KeyEvent& e) {
  emulator_window_.OnKeyDown(e);
}

void EmulatorWindow::EmulatorWindowListener::OnMouseDown(ui::MouseEvent& e) {
  emulator_window_.OnMouseDown(e);
}

void EmulatorWindow::EmulatorWindowListener::OnMouseUp(ui::MouseEvent& e) {
  emulator_window_.OnMouseUp(e);
}

void EmulatorWindow::EmulatorWindowListener::OnUsbDeviceChanged(
    bool is_arrival) {
  if (!emulator_window_.emulator()) {
    return;
  }

  if (!emulator_window_.emulator()->input_system()) {
    return;
  }

  auto* portal = emulator_window_.emulator()->input_system()->GetPortal();
  if (!portal) {
    return;
  }

  if (is_arrival) {
    portal->OnDeviceArrival();
  } else {
    portal->OnDeviceRemoval();
  }
}

void EmulatorWindow::DisplayConfigGameConfigLoadCallback::PostGameConfigLoad() {
  emulator_window_.ApplyDisplayConfigForCvars();
}

void EmulatorWindow::DisplayConfigDialog::OnDraw(ImGuiIO& io) {
  gpu::GraphicsSystem* graphics_system =
      emulator_window_.emulator_->graphics_system();
  if (!graphics_system) {
    return;
  }

  // In the top-left corner so it's close to the menu bar from where it was
  // opened.
  // Origin Y coordinate 20 was taken from the Dear ImGui demo.
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  // Alpha from Dear ImGui tooltips (0.35 from the overlay provides too low
  // visibility). Translucent so some effect of the changes can still be seen
  // through it.
  ImGui::SetNextWindowBgAlpha(0.6f);
  bool dialog_open = true;
  if (!ImGui::Begin("Post-processing", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::End();
    Close();
    return;
  }

  // Even if the close button has been pressed, still paint everything not to
  // have one frame with an empty window.

  // Prevent user confusion which has been reported multiple times.
  ImGui::TextUnformatted("All effects can be used on GPUs of any brand.");
  ImGui::Spacing();

  gpu::CommandProcessor* command_processor =
      graphics_system->command_processor();
  if (command_processor) {
    if (ImGui::TreeNodeEx(
            "Anti-aliasing",
            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
      gpu::CommandProcessor::SwapPostEffect current_swap_post_effect =
          command_processor->GetDesiredSwapPostEffect();
      int new_swap_post_effect_index = int(current_swap_post_effect);
      ImGui::RadioButton("None", &new_swap_post_effect_index,
                         int(gpu::CommandProcessor::SwapPostEffect::kNone));
      ImGui::RadioButton(
          "NVIDIA Fast Approximate Anti-Aliasing (FXAA) [Normal Quality]",
          &new_swap_post_effect_index,
          int(gpu::CommandProcessor::SwapPostEffect::kFxaa));
      ImGui::RadioButton(
          "NVIDIA Fast Approximate Anti-Aliasing (FXAA) [Extreme Quality]",
          &new_swap_post_effect_index,
          int(gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme));
      gpu::CommandProcessor::SwapPostEffect new_swap_post_effect =
          gpu::CommandProcessor::SwapPostEffect(new_swap_post_effect_index);
      if (current_swap_post_effect != new_swap_post_effect) {
        command_processor->SetDesiredSwapPostEffect(new_swap_post_effect);
      }

      // Override the values in the cvars to save them to the config at exit if
      // the user has set them to anything new.
      if (GetSwapPostEffectForCvarValue(cvars::postprocess_antialiasing) !=
          new_swap_post_effect) {
        OVERRIDE_string(postprocess_antialiasing,
                        GetCvarValueForSwapPostEffect(new_swap_post_effect));
      }

      ImGui::TreePop();
    }
  }

  ui::Presenter* presenter = graphics_system->presenter();
  if (presenter) {
    const ui::Presenter::GuestOutputPaintConfig& current_presenter_config =
        presenter->GetGuestOutputPaintConfigFromUIThread();
    ui::Presenter::GuestOutputPaintConfig new_presenter_config =
        current_presenter_config;

    if (ImGui::TreeNodeEx(
            "Resampling and sharpening",
            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
      // Filtering effect.
      int new_effect_index = int(new_presenter_config.GetEffect());
      ImGui::RadioButton(
          "None / Bilinear", &new_effect_index,
          int(ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear));
      ImGui::RadioButton(
          "AMD FidelityFX Contrast Adaptive Sharpening (CAS)",
          &new_effect_index,
          int(ui::Presenter::GuestOutputPaintConfig::Effect::kCas));
      ImGui::RadioButton(
          "AMD FidelityFX Super Resolution 1.0 (FSR)", &new_effect_index,
          int(ui::Presenter::GuestOutputPaintConfig::Effect::kFsr));
      new_presenter_config.SetEffect(
          ui::Presenter::GuestOutputPaintConfig::Effect(new_effect_index));

      // effect_description must be one complete, but short enough, sentence per
      // line, as TextWrapped doesn't work correctly in auto-resizing windows
      // (in the initial frames, the window becomes extremely tall, and widgets
      // added after the wrapped text have no effect on the width of the text).
      const char* effect_description = nullptr;
      switch (new_presenter_config.GetEffect()) {
        case ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear:
          effect_description =
              "Simple bilinear filtering is done if resampling is needed.\n"
              "Otherwise, only anti-aliasing is done if enabled, or displaying "
              "as is.";
          break;
        case ui::Presenter::GuestOutputPaintConfig::Effect::kCas:
          effect_description =
              "Sharpening and resampling to up to 2x2 to improve the fidelity "
              "of details.\n"
              "For scaling by more than 2x2, bilinear stretching is done "
              "afterwards.";
          break;
        case ui::Presenter::GuestOutputPaintConfig::Effect::kFsr:
          effect_description =
              "High-quality edge-preserving upscaling to arbitrary target "
              "resolutions.\n"
              "For scaling by more than 2x2, multiple upsampling passes are "
              "done.\n"
              "If not upscaling, Contrast Adaptive Sharpening (CAS) is used "
              "instead.";
          break;
      }
      if (effect_description) {
        ImGui::TextUnformatted(effect_description);
      }

      if (new_presenter_config.GetEffect() ==
              ui::Presenter::GuestOutputPaintConfig::Effect::kCas ||
          new_presenter_config.GetEffect() ==
              ui::Presenter::GuestOutputPaintConfig::Effect::kFsr) {
        if (effect_description) {
          ImGui::Spacing();
        }

        ImGui::TextUnformatted(
            "FXAA is highly recommended when using CAS or FSR.");

        ImGui::Spacing();

        // 2 decimal places is more or less enough precision for the sharpness
        // given the minor visual effect of small changes, the width of the
        // slider, and readability convenience (2 decimal places is like an
        // integer percentage). However, because Dear ImGui parses the string
        // representation of the number and snaps the value to it internally,
        // 2 decimal places actually offer less precision than the slider itself
        // does. This is especially prominent in the low range of the non-linear
        // FSR sharpness reduction slider. 3 decimal places are optimal in this
        // case.

        if (new_presenter_config.GetEffect() ==
            ui::Presenter::GuestOutputPaintConfig::Effect::kFsr) {
          float fsr_sharpness_reduction =
              new_presenter_config.GetFsrSharpnessReduction();
          ImGui::TextUnformatted(
              "FSR sharpness reduction when upscaling (lower is sharper):");
          const auto label = fmt::format(
              "{} %%", static_cast<int>(fsr_sharpness_reduction * 100));
          // Power 2.0 scaling as the reduction is in stops, used in exp2.
          fsr_sharpness_reduction = sqrt(2.f * fsr_sharpness_reduction);
          ImGui::SliderFloat(
              "##FSRSharpnessReduction", &fsr_sharpness_reduction,
              ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionMin,
              ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionMax,
              label.c_str(), ImGuiSliderFlags_NoInput);
          fsr_sharpness_reduction =
              .5f * fsr_sharpness_reduction * fsr_sharpness_reduction;
          ImGui::SameLine();
          if (ImGui::Button("Reset##ResetFSRSharpnessReduction")) {
            fsr_sharpness_reduction = ui::Presenter::GuestOutputPaintConfig ::
                kFsrSharpnessReductionDefault;
          }
          new_presenter_config.SetFsrSharpnessReduction(
              fsr_sharpness_reduction);
        }

        float cas_additional_sharpness =
            new_presenter_config.GetCasAdditionalSharpness();
        ImGui::TextUnformatted(
            new_presenter_config.GetEffect() ==
                    ui::Presenter::GuestOutputPaintConfig::Effect::kFsr
                ? "CAS additional sharpness when not upscaling (higher is "
                  "sharper):"
                : "CAS additional sharpness (higher is sharper):");
        const auto label = fmt::format(
            "{} %%", static_cast<int>(cas_additional_sharpness * 100));
        ImGui::SliderFloat(
            "##CASAdditionalSharpness", &cas_additional_sharpness,
            ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessMin,
            ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessMax,
            label.c_str(), ImGuiSliderFlags_NoInput);
        ImGui::SameLine();
        if (ImGui::Button("Reset##ResetCASAdditionalSharpness")) {
          cas_additional_sharpness = ui::Presenter::GuestOutputPaintConfig ::
              kCasAdditionalSharpnessDefault;
        }
        new_presenter_config.SetCasAdditionalSharpness(
            cas_additional_sharpness);

        // There's no need to expose the setting for the maximum number of FSR
        // EASU passes as it's largely meaningless if the user doesn't have a
        // very high-resolution monitor compared to the original image size as
        // most of the values of the slider will have no effect, and that's just
        // very fine-grained performance control for a fixed-overhead pass only
        // for huge screen resolutions.
      }

      ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Dithering", ImGuiTreeNodeFlags_Framed |
                                           ImGuiTreeNodeFlags_DefaultOpen)) {
      bool dither = current_presenter_config.GetDither();
      ImGui::Checkbox(
          "Dither the final output to 8bpc to make gradients smoother",
          &dither);
      new_presenter_config.SetDither(dither);

      ImGui::TreePop();
    }

    presenter->SetGuestOutputPaintConfigFromUIThread(new_presenter_config);

    // Override the values in the cvars to save them to the config at exit if
    // the user has set them to anything new.
    ui::Presenter::GuestOutputPaintConfig cvars_presenter_config =
        GetGuestOutputPaintConfigForCvars();
    if (cvars_presenter_config.GetEffect() !=
        new_presenter_config.GetEffect()) {
      OVERRIDE_string(postprocess_scaling_and_sharpening,
                      GetCvarValueForGuestOutputPaintEffect(
                          new_presenter_config.GetEffect()));
    }
    if (cvars_presenter_config.GetCasAdditionalSharpness() !=
        new_presenter_config.GetCasAdditionalSharpness()) {
      OVERRIDE_double(postprocess_ffx_cas_additional_sharpness,
                      new_presenter_config.GetCasAdditionalSharpness());
    }
    if (cvars_presenter_config.GetFsrSharpnessReduction() !=
        new_presenter_config.GetFsrSharpnessReduction()) {
      OVERRIDE_double(postprocess_ffx_fsr_sharpness_reduction,
                      new_presenter_config.GetFsrSharpnessReduction());
    }
    if (cvars_presenter_config.GetDither() !=
        new_presenter_config.GetDither()) {
      OVERRIDE_bool(postprocess_dither, new_presenter_config.GetDither());
    }
  }

  ImGui::End();

  if (!dialog_open) {
    Close();
    emulator_window_.ToggleDisplayConfigDialog();
    // `this` might have been destroyed by ToggleDisplayConfigDialog.
    return;
  }
}

void EmulatorWindow::ContentInstallDialog::OnDraw(ImGuiIO& io) {
#if XE_PLATFORM_WINRT
  const std::string popup_name =
      fmt::format("Installation Progress###install_{}", window_id_);
  if (!guide_popup_opened_) {
    guide_popup_opened_ = true;
    ImGui::OpenPopup(popup_name.c_str());
  }

  bool dialog_open = true;
  if (!BeginGuidePopup(popup_name.c_str(), &dialog_open, guide_state_,
                       imgui_drawer())) {
    Close();
    return;
  }

  const float uy = io.DisplaySize.y / 576.0f;
  const ImVec2 icon_size(32.0f * uy, 32.0f * uy);
  bool is_everything_installed = true;
  for (const auto& entry : *installation_entries_) {
    if (entry.icon_) {
      ImGui::Image(reinterpret_cast<ImTextureID>(entry.icon_.get()), icon_size);
      ImGui::SameLine();
    }
    ImGui::TextWrapped("%s", entry.name_.c_str());
    if (entry.content_type_ != xe::XContentType::kInvalid) {
      ImGui::TextWrapped("Type: %s",
                         XContentTypeMap.at(entry.content_type_).c_str());
    }
    ImGui::TextWrapped("Path: %s",
                       xe::path_to_utf8(entry.data_installation_path_).c_str());

    std::string result = fmt::format(
        "Status: {}", xe::Emulator::installStateStringName[static_cast<uint8_t>(
                          entry.installation_state_)]);
    if (entry.installation_state_ == xe::Emulator::InstallState::failed) {
      result += fmt::format(" - {} ({:08X})",
                            entry.installation_error_message_.c_str(),
                            entry.installation_result_);
    }
    ImGui::TextWrapped("%s", result.c_str());

    float fraction = 0.0f;
    if (entry.content_size_ > 0) {
      fraction =
          std::min(1.0f, static_cast<float>(entry.currently_installed_size_) /
                             static_cast<float>(entry.content_size_));
    }
    if (entry.installation_state_ == Emulator::InstallState::installing ||
        entry.installation_state_ == Emulator::InstallState::pending ||
        entry.installation_state_ == Emulator::InstallState::preparing) {
      is_everything_installed = false;
    } else if (entry.installation_state_ == Emulator::InstallState::installed) {
      fraction = 1.0f;
    }
    const std::string overlay =
        fmt::format("{}%", static_cast<int>(fraction * 100.0f));
    ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay.c_str());

    if (installation_entries_->size() > 1) {
      ImGui::Separator();
    }
  }
  ImGui::Spacing();

  ImGui::BeginDisabled(!is_everything_installed);
  if (GuideButton("Close")) {
    dialog_open = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  if (is_everything_installed && ConsumeGuideBackPress()) {
    dialog_open = false;
    ImGui::CloseCurrentPopup();
  }
  EndGuidePopup();

  if (!dialog_open) {
    Close();
  }
#else
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(20, 20), ImGuiCond_FirstUseEver);

  bool dialog_open = true;
  if (!ImGui::Begin(
          fmt::format("Installation Progress###{}", window_id_).c_str(),
          &dialog_open,
          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
              ImGuiWindowFlags_HorizontalScrollbar)) {
    Close();
    ImGui::End();
    return;
  }

  bool is_everything_installed = true;
  for (const auto& entry : *installation_entries_) {
    ImGui::BeginTable(fmt::format("table_{}", entry.name_).c_str(), 2);
    ImGui::TableNextRow(0);
    ImGui::TableSetColumnIndex(0);
    if (entry.icon_) {
      ImGui::Image(reinterpret_cast<ImTextureID>(entry.icon_.get()),
                   ui::default_image_icon_size);
    } else {
      ImGui::Dummy(ui::default_image_icon_size);
    }
    ImGui::TableNextColumn();

    ImGui::Text("Name: %s", entry.name_.c_str());
    ImGui::Text("Installation Path:");
    ImGui::SameLine();
    if (ImGui::TextLink(
            xe::path_to_utf8(entry.data_installation_path_).c_str())) {
      LaunchFileExplorer(emulator_window_.emulator_->content_root() /
                         entry.data_installation_path_);
    }

    if (entry.content_type_ != xe::XContentType::kInvalid) {
      ImGui::Text("Content Type: %s",
                  XContentTypeMap.at(entry.content_type_).c_str());
    }

    std::string result = fmt::format(
        "Status: {}", xe::Emulator::installStateStringName[static_cast<uint8_t>(
                          entry.installation_state_)]);

    if (entry.installation_state_ == xe::Emulator::InstallState::failed) {
      result += fmt::format(" - {} ({:08X})",
                            entry.installation_error_message_.c_str(),
                            entry.installation_result_);
    }

    ImGui::Text("%s", result.c_str());
    ImGui::EndTable();

    if (entry.content_size_ > 0) {
      ImGui::ProgressBar(static_cast<float>(entry.currently_installed_size_) /
                         entry.content_size_);

      if (entry.installation_state_ == Emulator::InstallState::installing ||
          entry.installation_state_ == Emulator::InstallState::pending ||
          entry.installation_state_ == Emulator::InstallState::preparing) {
        is_everything_installed = false;
      }
    } else {
      ImGui::ProgressBar(0.0f);
    }

    if (installation_entries_->size() > 1) {
      ImGui::Separator();
    }
  }
  ImGui::Spacing();

  ImGui::BeginDisabled(!is_everything_installed);
  if (ImGui::Button("Close")) {
    ImGui::EndDisabled();
    Close();
    ImGui::End();
    return;
  }
  ImGui::EndDisabled();

  if (!dialog_open && is_everything_installed) {
    Close();
    ImGui::End();
    return;
  }
  ImGui::End();
#endif  // XE_PLATFORM_WINRT
}

void EmulatorWindow::XMPConfigDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(20, 20), ImGuiCond_FirstUseEver);

  bool dialog_open = true;
  if (!ImGui::Begin("Audio Player Menu", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    Close();
    ImGui::End();
    return;
  }

  auto audio_player = emulator_window_.emulator_->audio_media_player();
  using xmp_state = kernel::xam::apps::XmpApp::State;
  if (audio_player) {
    ImGui::Text("Audio player status:");
    ImGui::SameLine();
    switch (audio_player->GetState()) {
      case xmp_state::kIdle:
        ImGui::Text("Idle");
        break;
      case xmp_state::kPaused:
        ImGui::Text("Paused");
        break;
      case xmp_state::kPlaying:
        ImGui::Text("Playing");
        break;
      default:
        break;
    }

    if (audio_player->IsPlaying()) {
      if (ImGui::Button("Pause")) {
        audio_player->Pause();
      }
    } else if (audio_player->IsPaused()) {
      if (ImGui::Button("Resume")) {
        audio_player->Continue();
      }
    }

    volume_ =
        emulator_window_.emulator_->audio_media_player()->GetVolume()->load();

    if (ImGui::SliderFloat("Audio player volume", &volume_, 0.0f, 1.0f,
                           "%.2f")) {
      audio_player->SetVolume(volume_);
    }
  }

  ImGui::End();

  if (!dialog_open) {
    Close();
    emulator_window_.xmp_config_dialog_.release();
    return;
  }
}

bool EmulatorWindow::Initialize() {
  window_->AddListener(&window_listener_);
  window_->AddInputListener(&window_listener_, kZOrderEmulatorWindowInput);

  // Main menu.
  // FIXME: This code is really messy.
#if !XE_PLATFORM_WINRT
  auto main_menu = MenuItem::Create(MenuItem::Type::kNormal);
  auto file_menu = MenuItem::Create(MenuItem::Type::kPopup, "&File");
  auto recent_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Open Recent");
  auto zar_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Zar Package");
  FillRecentlyLaunchedTitlesMenu(recent_menu.get());
  {
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Open...", "Ctrl+O",
                         std::bind(&EmulatorWindow::FileOpen, this)));
    file_menu->AddChild(std::move(recent_menu));
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    zar_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Create",
                         std::bind(&EmulatorWindow::CreateZarchive, this)));
    zar_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Extract",
                         std::bind(&EmulatorWindow::ExtractZarchive, this)));
    file_menu->AddChild(std::move(zar_menu));
#ifdef DEBUG
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Close",
                         std::bind(&EmulatorWindow::FileClose, this)));
#endif  // #ifdef DEBUG
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Show content directory...",
        std::bind(&EmulatorWindow::ShowContentDirectory, this)));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Dump XLast",
                         std::bind(&EmulatorWindow::DumpXLast, this)));
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "E&xit", "Alt+F4",
                         [this]() { window_->RequestClose(); }));
  }
  main_menu->AddChild(std::move(file_menu));

  // Profile Menu
  auto profile_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Profile");
  {
    profile_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Show Profile Menu", "",
        std::bind(&EmulatorWindow::ToggleProfilesConfigDialog, this)));
    profile_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Gamerpic Browser", "",
        std::bind(&EmulatorWindow::ToggleGamerpicBrowserDialog, this)));
  }
  main_menu->AddChild(std::move(profile_menu));

  // Content Menu
  auto content_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Content");
  {
    content_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Install Content",
                         std::bind(&EmulatorWindow::InstallContent, this)));
    content_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Extract Content",
                         std::bind(&EmulatorWindow::ExtractContent, this, "")));
    content_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Show Installed Content",
        std::bind(&EmulatorWindow::ToggleContentListDialog, this)));
  }
  main_menu->AddChild(std::move(content_menu));

  // CPU menu.
  auto cpu_menu = MenuItem::Create(MenuItem::Type::kPopup, "&CPU");
  {
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Reset Time Scalar", "Numpad *",
        std::bind(&EmulatorWindow::CpuTimeScalarReset, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Time Scalar /= 2", "Numpad -",
        std::bind(&EmulatorWindow::CpuTimeScalarSetHalf, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Time Scalar *= 2", "Numpad +",
        std::bind(&EmulatorWindow::CpuTimeScalarSetDouble, this)));
  }
#if XE_OPTION_PROFILING
  cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kString,
                                        "Toggle Profiler &Display", "F3",
                                        []() { Profiler::ToggleDisplay(); }));
    cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kString,
                                        "&Pause/Resume Profiler", "`",
                                        []() { Profiler::TogglePause(); }));
  }
#endif
  cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Break and Show Guest Debugger",
        "Pause/Break", std::bind(&EmulatorWindow::CpuBreakIntoDebugger, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Break into Host Debugger",
        "Ctrl+Pause/Break",
        std::bind(&EmulatorWindow::CpuBreakIntoHostDebugger, this)));
  }
  main_menu->AddChild(std::move(cpu_menu));

  // GPU menu.
  auto gpu_menu = MenuItem::Create(MenuItem::Type::kPopup, "&GPU");
  {
    gpu_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Trace Frame", "F4",
                         std::bind(&EmulatorWindow::GpuTraceFrame, this)));
  }
  gpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    gpu_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Clear Runtime Caches", "F5",
                         std::bind(&EmulatorWindow::GpuClearCaches, this)));
  }
  main_menu->AddChild(std::move(gpu_menu));

  // Display menu.
  auto display_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Display");
  {
    display_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Post-processing settings", "F6",
        std::bind(&EmulatorWindow::ToggleDisplayConfigDialog, this)));
  }
  display_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    display_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Fullscreen", "F11",
                         std::bind(&EmulatorWindow::ToggleFullscreen, this)));
    display_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Take Screenshot", "F12",
                         std::bind(&EmulatorWindow::TakeScreenshot, this)));
  }
  main_menu->AddChild(std::move(display_menu));

  // HID menu.
  auto hid_menu = MenuItem::Create(MenuItem::Type::kPopup, "&HID");
  {
    hid_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Toggle controller vibration", "",
        std::bind(&EmulatorWindow::ToggleControllerVibration, this)));
    hid_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Display controller hotkeys", "",
        std::bind(&EmulatorWindow::DisplayHotKeysConfig, this)));
  }
  main_menu->AddChild(std::move(hid_menu));

  // XMP menu
  auto xmp_menu = MenuItem::Create(MenuItem::Type::kPopup, "&XMP");
  {
    xmp_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Show XMP Menu", "",
        std::bind(&EmulatorWindow::ToggleXMPConfigDialog, this)));
  }
  main_menu->AddChild(std::move(xmp_menu));

  // Console menu
  auto console_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Console");
  {
    console_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Open console settings", "",
        std::bind(&EmulatorWindow::ToggleConsoleSettingsDialog, this)));
  }
  main_menu->AddChild(std::move(console_menu));

  // Netplay menu.
  auto Netplay_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Netplay");
  {
    Netplay_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Status", "",
        std::bind(&EmulatorWindow::ToggleNetplayStatusDialog, this)));

    Netplay_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Settings", "",
        std::bind(&EmulatorWindow::ToggleNetplaySettingsDialog, this)));

    Netplay_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Manager", "",
        std::bind(&EmulatorWindow::ToggleFriendsDialog, this, false)));

    Netplay_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));

    Netplay_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Update Checker",
        std::bind(&EmulatorWindow::ToggleUpdaterDialog, this)));
  }
  main_menu->AddChild(std::move(Netplay_menu));

  // Help menu.
  auto help_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Help");
  {
    help_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "FA&Q...", "F1",
                         std::bind(&EmulatorWindow::ShowFAQ, this)));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Game &compatibility...",
                         std::bind(&EmulatorWindow::ShowCompatibility, this)));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Build commit on GitHub...", "F2",
        std::bind(&EmulatorWindow::ShowBuildCommit, this)));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Recent changes on GitHub...", []() {
          LaunchWebBrowser(
              "https://github.com/AdrianCassar/xenia-canary/"
              "compare/" XE_BUILD_COMMIT "..." XE_BUILD_BRANCH);
        }));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&About...",
        []() { LaunchWebBrowser("https://xenia.jp/about/"); }));
  }
  main_menu->AddChild(std::move(help_menu));

  window_->SetMainMenu(std::move(main_menu));
#endif

  window_->SetMainMenuEnabled(false);

  UpdateTitle();

  if (!window_->Open()) {
    XELOGE("Failed to open the platform window");
    return false;
  }

  Profiler::SetUserIO(kZOrderProfiler, window_.get(), nullptr, nullptr);

  return true;
}

const char* EmulatorWindow::GetCvarValueForSwapPostEffect(
    gpu::CommandProcessor::SwapPostEffect effect) {
  switch (effect) {
    case gpu::CommandProcessor::SwapPostEffect::kFxaa:
      return "fxaa";
    case gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme:
      return "fxaa_extreme";
    default:
      return "";
  }
}

gpu::CommandProcessor::SwapPostEffect
EmulatorWindow::GetSwapPostEffectForCvarValue(const std::string& cvar_value) {
  if (cvar_value == GetCvarValueForSwapPostEffect(
                        gpu::CommandProcessor::SwapPostEffect::kFxaa)) {
    return gpu::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (cvar_value == GetCvarValueForSwapPostEffect(
                        gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme)) {
    return gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return gpu::CommandProcessor::SwapPostEffect::kNone;
}

const char* EmulatorWindow::GetCvarValueForGuestOutputPaintEffect(
    ui::Presenter::GuestOutputPaintConfig::Effect effect) {
  switch (effect) {
    case ui::Presenter::GuestOutputPaintConfig::Effect::kCas:
      return "cas";
    case ui::Presenter::GuestOutputPaintConfig::Effect::kFsr:
      return "fsr";
    default:
      return "";
  }
}

ui::Presenter::GuestOutputPaintConfig::Effect
EmulatorWindow::GetGuestOutputPaintEffectForCvarValue(
    const std::string& cvar_value) {
  if (cvar_value == GetCvarValueForGuestOutputPaintEffect(
                        ui::Presenter::GuestOutputPaintConfig::Effect::kCas)) {
    return ui::Presenter::GuestOutputPaintConfig::Effect::kCas;
  }
  if (cvar_value == GetCvarValueForGuestOutputPaintEffect(
                        ui::Presenter::GuestOutputPaintConfig::Effect::kFsr)) {
    return ui::Presenter::GuestOutputPaintConfig::Effect::kFsr;
  }
  return ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear;
}

ui::Presenter::GuestOutputPaintConfig
EmulatorWindow::GetGuestOutputPaintConfigForCvars() {
  ui::Presenter::GuestOutputPaintConfig paint_config;
  paint_config.SetAllowOverscanCutoff(true);
  paint_config.SetEffect(GetGuestOutputPaintEffectForCvarValue(
      cvars::postprocess_scaling_and_sharpening));
  paint_config.SetCasAdditionalSharpness(
      float(cvars::postprocess_ffx_cas_additional_sharpness));
  paint_config.SetFsrMaxUpsamplingPasses(
      cvars::postprocess_ffx_fsr_max_upsampling_passes);
  paint_config.SetFsrSharpnessReduction(
      float(cvars::postprocess_ffx_fsr_sharpness_reduction));
  paint_config.SetDither(cvars::postprocess_dither);
  return paint_config;
}

void EmulatorWindow::ApplyDisplayConfigForCvars() {
  gpu::GraphicsSystem* graphics_system = emulator_->graphics_system();
  if (!graphics_system) {
    return;
  }

  gpu::CommandProcessor* command_processor =
      graphics_system->command_processor();
  if (command_processor) {
    command_processor->SetDesiredSwapPostEffect(
        GetSwapPostEffectForCvarValue(cvars::postprocess_antialiasing));
  }

  ui::Presenter* presenter = graphics_system->presenter();
  if (presenter) {
    presenter->SetGuestOutputPaintConfigFromUIThread(
        GetGuestOutputPaintConfigForCvars());
  }
}

void EmulatorWindow::OnKeyDown(ui::KeyEvent& e) {
  if (!emulator_initialized_) {
    return;
  }

  switch (e.virtual_key()) {
    case ui::VirtualKey::kO: {
      if (!e.is_ctrl_pressed()) {
        return;
      }
      FileOpen();
    } break;
    case ui::VirtualKey::kMultiply: {
      CpuTimeScalarReset();
    } break;
    case ui::VirtualKey::kSubtract: {
      CpuTimeScalarSetHalf();
    } break;
    case ui::VirtualKey::kAdd: {
      CpuTimeScalarSetDouble();
    } break;

    case ui::VirtualKey::kF3: {
      Profiler::ToggleDisplay();
    } break;

    case ui::VirtualKey::kF4: {
      GpuTraceFrame();
    } break;
    case ui::VirtualKey::kF5: {
      GpuClearCaches();
    } break;

    case ui::VirtualKey::kF6: {
      ToggleDisplayConfigDialog();
    } break;
    case ui::VirtualKey::kF11: {
      ToggleFullscreen();
    } break;
    case ui::VirtualKey::kF12: {
      TakeScreenshot();
    } break;

    case ui::VirtualKey::kEscape: {
      // Allow users to escape fullscreen (but not enter it).
      if (!window_->IsFullscreen()) {
        return;
      }
      SetFullscreen(false);
    } break;

#ifdef DEBUG
    case ui::VirtualKey::kF7: {
      // Save to file
      // TODO: Choose path based on user input, or from options
      // TODO: Spawn a new thread to do this.
      emulator()->SaveToFile("test.sav");
    } break;
    case ui::VirtualKey::kF8: {
      // Restore from file
      // TODO: Choose path from user
      // TODO: Spawn a new thread to do this.
      emulator()->RestoreFromFile("test.sav");
    } break;
#endif  // #ifdef DEBUG

    case ui::VirtualKey::kPause: {
      CpuBreakIntoDebugger();
    } break;
    case ui::VirtualKey::kCancel: {
      CpuBreakIntoHostDebugger();
    } break;

    case ui::VirtualKey::kF1: {
      ShowFAQ();
    } break;

    case ui::VirtualKey::kF2: {
      ShowBuildCommit();
    } break;

    case ui::VirtualKey::kF9: {
      RunPreviouslyPlayedTitle();
    } break;

    default:
      return;
  }

  e.set_handled(true);
}

void EmulatorWindow::OnMouseDown(const ui::MouseEvent& e) {
  if (imgui_drawer_->IsAnyDialogOpen()) {
    return;
  }

  if (e.button() == ui::MouseEvent::Button::kLeft) {
    ToggleFullscreenOnDoubleClick();
  }
}

void EmulatorWindow::OnMouseUp(const ui::MouseEvent& e) {
  last_mouse_up = steady_clock::now();
}

void EmulatorWindow::TakeScreenshot() {
  xe::ui::RawImage image;

  imgui_drawer_->EnableNotifications(false);

  if (!GetGraphicsSystemPresenter()->CaptureGuestOutput(image) ||
      GetGraphicsSystemPresenter() == nullptr) {
    XELOGE("Failed to capture guest output for screenshot");
    return;
  }

  imgui_drawer_->EnableNotifications(true);
  ExportScreenshot(image);
}

void EmulatorWindow::ExportScreenshot(const xe::ui::RawImage& image) {
  auto t = std::time(nullptr);

  // The format is: Year-Month-DayTHours-Minutes-Seconds based off ISO 8601
  std::string datetime =
      fmt::format("{:%Y-%m-%dT%H-%M-%S}", *std::localtime(&t));

  // Get the title id of the game because some titles contain characters that
  // cannot be used as a directory
  std::string title_id;
  if (emulator()->title_id()) {
    title_id = fmt::format("{:08X}", emulator()->title_id());
  } else {
    XELOGE("Failed to get the current title id");
    return;
  }

  // Find where xenia.exe or xenia_canary.exe is located and create a
  // screenshots folder
  auto screenshot_path =
      (xe::filesystem::GetExecutableFolder() / "screenshots" / title_id);

  if (!std::filesystem::exists(screenshot_path)) {
    std::filesystem::create_directories(screenshot_path);
  }

  std::string filename = fmt::format("{} - {}.png", title_id, datetime);
  SaveImage(screenshot_path / filename, image);

  const std::string notification_text =
      fmt::format("Screenshot saved: {}", filename);

  app_context_.CallInUIThread([&, notification_text]() {
    new xe::ui::HostNotificationWindow(imgui_drawer(), "Screenshot Created!",
                                       notification_text, 0);
  });
}

// Converts a RawImage into a PNG file
void EmulatorWindow::SaveImage(const std::filesystem::path& filepath,
                               const xe::ui::RawImage& image) {
  auto file = std::ofstream(filepath, std::ios::binary);
  if (!file.is_open()) {
    XELOGE("Failed to open file for writing: {}", filepath);
    return;
  }

  auto result = stbi_write_png_to_func(
      [](void* context, void* data, int size) {
        auto file = reinterpret_cast<std::ofstream*>(context);
        file->write(reinterpret_cast<const char*>(data), size);
      },
      &file, image.width, image.height, 4, image.data.data(),
      (int)image.stride);
  if (result == 0) {
    XELOGE("Failed to write PNG to file: {}", filepath);
    return;
  }
}

void EmulatorWindow::ToggleFullscreenOnDoubleClick() {
  if (cvars::disable_doubleclick_fullscreen) {
    return;
  }

  // this function tests if user has double clicked.
  // if double click was achieved the fullscreen gets toggled
  const auto now = steady_clock::now();  // current mouse event time
  constexpr int16_t mouse_down_max_threshold = 250;
  constexpr int16_t mouse_up_max_threshold = 250;
  constexpr int16_t mouse_up_down_max_delta = 100;
  // max delta to prevent 'chaining' of double clicks with next mouse events

  const auto last_mouse_down_delta = diff_in_ms(now, last_mouse_down);
  if (last_mouse_down_delta >= mouse_down_max_threshold) {
    last_mouse_down = now;
    return;
  }

  const auto last_mouse_up_delta = diff_in_ms(now, last_mouse_up);
  const auto mouse_event_deltas = diff_in_ms(last_mouse_up, last_mouse_down);
  if (last_mouse_up_delta >= mouse_up_max_threshold) {
    return;
  }

  if (mouse_event_deltas < mouse_up_down_max_delta) {
    ToggleFullscreen();
  }
}

void EmulatorWindow::FileDrop(const std::filesystem::path& path) {
  if (!emulator_initialized_) {
    return;
  }

  RunTitle(path);
}

void EmulatorWindow::FileOpen() {
#if XE_PLATFORM_WINRT
  imgui_drawer()->SetIgnoreInput(true);
  UWP::SelectFile([this](std::string file) {
    imgui_drawer()->SetIgnoreInput(false);
    if (!file.empty()) {
      RunTitle(std::filesystem::absolute(file));
    }
  });
#else
  std::filesystem::path path;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(false);
  file_picker->set_title("Select Content Package");
  file_picker->set_extensions({
      {"Supported Files", "*.iso;*.xex;*.zar;*.*"},
      {"Disc Image (*.iso)", "*.iso"},
      {"Disc Archive (*.zar)", "*.zar"},
      {"Xbox Executable (*.xex)", "*.xex"},
      //{"Content Package (*.xcp)", "*.xcp" },
      {"All Files (*.*)", "*.*"},
  });
  if (file_picker->Show(window_.get())) {
    auto selected_files = file_picker->selected_files();
    if (!selected_files.empty()) {
      path = selected_files[0];
    }
    // Only run the title if a file is selected
    RunTitle(path);
  }
#endif
}

void EmulatorWindow::FileClose() {
#if XE_PLATFORM_WINRT
  BeginTitleClose();
#else
  emulator_->TerminateTitle();
#endif
}

void EmulatorWindow::InstallContent() {
  auto install_paths = [this](std::vector<std::filesystem::path> paths) {
    if (paths.empty()) {
      return;
    }

    std::shared_ptr<std::vector<Emulator::ContentInstallEntry>>
        content_installation_status =
            std::make_shared<std::vector<Emulator::ContentInstallEntry>>();

    for (const auto& path : paths) {
      content_installation_status->push_back({path});
    }

    for (auto& entry : *content_installation_status) {
      emulator_->ProcessContentPackageHeader(entry.path_, entry);
    }

    auto installationThread = std::thread([this, content_installation_status] {
      for (auto& entry : *content_installation_status) {
        emulator_->InstallContentPackage(entry.path_, entry);
      }
    });
    installationThread.detach();

    new ContentInstallDialog(imgui_drawer_.get(), *this,
                             content_installation_status);
  };
#if XE_PLATFORM_WINRT
  imgui_drawer()->SetIgnoreInput(true);
  UWP::SelectFiles([this, install_paths](std::vector<std::string> files) {
    imgui_drawer()->SetIgnoreInput(false);
    std::vector<std::filesystem::path> paths;
    for (const auto& file : files) {
      paths.emplace_back(file);
    }
    install_paths(std::move(paths));
  });
#else
  std::vector<std::filesystem::path> paths;
  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(true);
  file_picker->set_title("Select Content Package");
  file_picker->set_extensions({
      {"All Files (*.*)", "*.*"},
  });
  if (file_picker->Show(window_.get())) {
    paths = file_picker->selected_files();
  }

  install_paths(std::move(paths));
#endif
}

void EmulatorWindow::ExtractContent(const std::filesystem::path file) {
  std::vector<std::filesystem::path> package_files;
  std::filesystem::path extract_dir;

  if (!file.empty()) {
    package_files.push_back(file);
  } else {
    auto file_picker = xe::ui::FilePicker::Create();
    file_picker->set_mode(ui::FilePicker::Mode::kOpen);
    file_picker->set_type(ui::FilePicker::Type::kFile);
    file_picker->set_multi_selection(true);
    file_picker->set_title("Select Content Package");
    file_picker->set_extensions({
        {"All Files (*.*)", "*.*"},
    });

    if (file_picker->Show(window_.get())) {
      package_files = file_picker->selected_files();
    }

    if (package_files.empty()) {
      return;
    }
  }
  auto save_file_picker = xe::ui::FilePicker::Create();
  save_file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  save_file_picker->set_type(ui::FilePicker::Type::kDirectory);
  save_file_picker->set_title("Select Directory to Extract");

  if (save_file_picker->Show(window_.get())) {
    extract_dir = save_file_picker->selected_files().front();
  }

  ExtractContentTo(package_files, extract_dir);
}

void EmulatorWindow::ExtractContentTo(
    const std::vector<std::filesystem::path>& package_files,
    const std::filesystem::path& extract_dir) {
  if (package_files.empty() || extract_dir.empty()) {
    return;
  }

  std::shared_ptr<std::vector<Emulator::ContentInstallEntry>>
      content_installation_status =
          std::make_shared<std::vector<Emulator::ContentInstallEntry>>();

  for (const auto& path : package_files) {
    content_installation_status->push_back({path});
  }

  for (auto& entry : *content_installation_status) {
    emulator_->ProcessContentPackageHeader(entry.path_, entry);
    entry.data_installation_path_ = extract_dir;
    entry.header_installation_path_ = "";
  }

  auto installationThread = std::thread([this, content_installation_status] {
    for (auto& entry : *content_installation_status) {
      emulator_->ExtractContentPackage(entry.path_, entry);
    }
  });
  installationThread.detach();

  new ContentInstallDialog(imgui_drawer_.get(), *this,
                           content_installation_status);
}

void EmulatorWindow::ExtractZarchive() {
  std::vector<std::filesystem::path> zarchive_files;
  std::filesystem::path extract_dir;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(true);
  file_picker->set_title("Select Zar Package");
  file_picker->set_extensions({
      {"Zarchive Files (*.zar)", "*.zar"},
  });

  if (file_picker->Show(window_.get())) {
    zarchive_files = file_picker->selected_files();
  }

  if (zarchive_files.empty()) {
    return;
  }

  file_picker->set_type(ui::FilePicker::Type::kDirectory);
  file_picker->set_title("Select Directory to Extract");

  if (file_picker->Show(window_.get())) {
    extract_dir = file_picker->selected_files().front();
  }

  if (extract_dir.empty()) {
    return;
  }

  std::string extract_overview = "";

  for (auto& zarchive_file_path : zarchive_files) {
    extract_overview += "\n" + path_to_utf8(zarchive_file_path);
  }

  app_context_.CallInUIThread([&]() {
    new xe::ui::HostNotificationWindow(imgui_drawer(), "Extracting...",
                                       string_util::trim(extract_overview), 0);
  });

  auto run = [this, extract_dir, zarchive_files]() -> void {
    std::string summary = "";

    for (auto& zarchive_file_path : zarchive_files) {
      // Normalize the path and make absolute.
      auto abs_path = std::filesystem::absolute(zarchive_file_path);
      std::filesystem::path abs_extract_dir;

      if (zarchive_files.size() > 1) {
        abs_extract_dir =
            std::filesystem::absolute((extract_dir / abs_path.stem()));
      } else {
        abs_extract_dir = std::filesystem::absolute(extract_dir);
      }

      XELOGI("Extracting zar package: {}\n",
             zarchive_file_path.filename().string());

      auto result =
          emulator_->ExtractZarchivePackage(abs_path, abs_extract_dir);

      if (result != X_STATUS_SUCCESS) {
        std::error_code ec;

        if (!std::filesystem::is_empty(abs_extract_dir)) {
          std::filesystem::remove(abs_extract_dir, ec);
        }

        summary += fmt::format("\nFailed: {}", zarchive_file_path);

        XELOGE("Failed to extract Zarchive package.", result);
      } else {
        summary += fmt::format("\nSuccess: {}", abs_extract_dir);
      }
    }

    new xe::ui::HostNotificationWindow(imgui_drawer(), "Zar Extraction Summary",
                                       string_util::trim(summary), 0);
  };

  auto zarThread = std::thread(run);
  zarThread.detach();
}

void EmulatorWindow::CreateZarchive() {
  std::vector<std::filesystem::path> content_dirs;
  std::filesystem::path zarchive_dir;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kDirectory);
  file_picker->set_multi_selection(true);
  file_picker->set_title("Select Contents");

  if (file_picker->Show(window_.get())) {
    content_dirs = file_picker->selected_files();
  }

  if (content_dirs.empty()) {
    return;
  }

  if (content_dirs.size() == 1) {
    file_picker->set_mode(ui::FilePicker::Mode::kSave);
    file_picker->set_type(ui::FilePicker::Type::kFile);
    file_picker->set_multi_selection(false);
    file_picker->set_file_name(content_dirs.front().filename().string());
    file_picker->set_default_extension("zar");
    file_picker->set_title("Zarchive File");
    file_picker->set_extensions({
        {"Zarchive File (*.zar)", "*.zar"},
    });
  } else {
    file_picker->set_title("Output Directory");
  }

  if (file_picker->Show(window_.get())) {
    zarchive_dir = file_picker->selected_files().front();
  }

  if (zarchive_dir.empty()) {
    return;
  }

  std::string create_overview = "";

  std::map<std::filesystem::path, std::filesystem::path> zarchive_files{};

  for (auto& content_path : content_dirs) {
    // Normalize the path and make absolute.
    auto abs_content_dir = std::filesystem::absolute(content_path);
    std::filesystem::path abs_zarchive_file;

    if (content_dirs.size() > 1) {
      abs_zarchive_file = std::filesystem::absolute(
          (zarchive_dir / abs_content_dir.filename().concat(".zar")));
    } else {
      abs_zarchive_file = std::filesystem::absolute(zarchive_dir);
    }

    zarchive_files[content_path] = abs_zarchive_file;

    create_overview += "\n" + path_to_utf8(abs_zarchive_file);
  }

  app_context_.CallInUIThread([&]() {
    new xe::ui::HostNotificationWindow(imgui_drawer(), "Creating...",
                                       string_util::trim(create_overview), 0);
  });

  auto run = [this, zarchive_files]() -> void {
    std::string summary = "";

    for (auto const& [content_path, zarchive_file] : zarchive_files) {
      // Normalize the path and make absolute.
      auto abs_content_dir = std::filesystem::absolute(content_path);

      XELOGI("Creating zar package: {}\n", zarchive_file.filename().string());

      auto result =
          emulator_->CreateZarchivePackage(abs_content_dir, zarchive_file);

      if (result != X_ERROR_SUCCESS) {
        std::error_code ec;

        // delete incomplete output file
        std::filesystem::remove(zarchive_file, ec);

        summary += fmt::format("\nFailed: {}", abs_content_dir);

        XELOGE("Failed to create Zarchive package.", result);
      } else {
        summary += fmt::format("\nSuccess: {}", zarchive_file);
      }
    }

    new xe::ui::HostNotificationWindow(imgui_drawer(), "Zar Creation Summary",
                                       string_util::trim(summary), 0);
  };

  auto zarThread = std::thread(run);
  zarThread.detach();
}

void EmulatorWindow::InstallContentForTitle(
    const std::string& title_id, const std::filesystem::path& storage_root,
    DlcInstallMode mode) {
#if XE_PLATFORM_WINRT
  try {
    InstallContent();
  } catch (const std::exception& e) {
    XELOGE("InstallContentForTitle: exception: {}", e.what());
    throw;
  } catch (...) {
    XELOGE("InstallContentForTitle: unknown exception");
    throw;
  }
#else
  (void)title_id;
  (void)storage_root;
  (void)mode;
  InstallContent();
#endif
}

void EmulatorWindow::DetachProfileConfigDialog(ProfileConfigDialog* dialog) {
  if (profile_config_dialog_.get() == dialog) {
    profile_config_dialog_.release();
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     false);
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
  }
}

void EmulatorWindow::ShowContentDirectory() {
  auto content_root = emulator_->content_root();

  if (!std::filesystem::exists(content_root)) {
    std::filesystem::create_directories(content_root);
  }

  LaunchFileExplorer(content_root);
}

void EmulatorWindow::DumpXLast() { emulator()->DumpXLast(); }

void EmulatorWindow::CpuTimeScalarReset() {
  Clock::set_guest_time_scalar(1.0);
  UpdateTitle();
}

void EmulatorWindow::CpuTimeScalarSetHalf() {
  Clock::set_guest_time_scalar(Clock::guest_time_scalar() / 2.0);
  UpdateTitle();
}

void EmulatorWindow::CpuTimeScalarSetDouble() {
  Clock::set_guest_time_scalar(Clock::guest_time_scalar() * 2.0);
  UpdateTitle();
}

void EmulatorWindow::CpuBreakIntoDebugger() {
  if (!cvars::debug) {
    xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_.get(), "Xenia Debugger",
                                        "Xenia must be launched with the "
                                        "--debug flag in order to enable "
                                        "debugging.");
    return;
  }
  auto processor = emulator()->processor();
  if (processor->execution_state() == cpu::ExecutionState::kRunning) {
    // Currently running, so interrupt (and show the debugger).
    processor->Pause();
  } else {
    // Not running, so just bring the debugger into focus.
    processor->ShowDebugger();
  }
}

void EmulatorWindow::CpuBreakIntoHostDebugger() { xe::debugging::Break(); }

void EmulatorWindow::GpuTraceFrame() {
  emulator()->graphics_system()->RequestFrameTrace();
}

void EmulatorWindow::GpuClearCaches() {
  emulator()->graphics_system()->ClearCaches();
}

void EmulatorWindow::SetFullscreen(bool fullscreen_) {
  if (window_->IsFullscreen() == fullscreen_) {
    return;
  }

  OVERRIDE_bool(fullscreen, fullscreen_);

  window_->SetFullscreen(fullscreen_);
  window_->SetCursorVisibility(fullscreen_
                                   ? ui::Window::CursorVisibility::kAutoHidden
                                   : ui::Window::CursorVisibility::kVisible);
}

void EmulatorWindow::ToggleFullscreen() {
  SetFullscreen(!window_->IsFullscreen());
}

void EmulatorWindow::SetAutoCheckForUpdates(bool state) {
  OVERRIDE_bool(auto_check_updates, state);
}

void EmulatorWindow::UpdateCompletionNotification() {
  app_context_.CallInUIThread([&]() {
    std::string message = fmt::format("Build Date: {} ({})", XE_BUILD_DATE,
                                      XE_BUILD_COMMIT_SHORT);

    new xe::ui::HostNotificationWindow(imgui_drawer(), "Update Completed",
                                       message.c_str(), 0, 9);
  });
}

void EmulatorWindow::ToggleDisplayConfigDialog() {
  if (!display_config_dialog_) {
    display_config_dialog_ =
        std::make_unique<DisplayConfigDialog>(imgui_drawer_.get(), *this);
  } else {
    if (display_config_dialog_->IsClosing()) {
      display_config_dialog_.release();
    } else {
      display_config_dialog_.reset();
    }
  }
}

void EmulatorWindow::ToggleProfilesConfigDialog() {
  if (!profile_config_dialog_) {
    disable_hotkeys_ = true;

    if (emulator_->kernel_state()->xam_state()->IsUIActive()) {
      return;
    }

    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     true);
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

    profile_config_dialog_ =
        std::make_unique<ProfileConfigDialog>(imgui_drawer_.get(), this);
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     false);
    if (profile_config_dialog_->IsClosing()) {
      profile_config_dialog_.release();
    } else {
      profile_config_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
  }
}

void EmulatorWindow::ToggleGamerpicBrowserDialog() {
  if (!gamerpic_browser_dialog_) {
    disable_hotkeys_ = true;

    if (emulator_->kernel_state()->xam_state()->IsUIActive()) {
      return;
    }

    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     true);
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

    gamerpic_browser_dialog_ =
        TitleGamerpicBrowser::Create(imgui_drawer_.get(), this);
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     false);
    if (gamerpic_browser_dialog_->IsClosing()) {
      gamerpic_browser_dialog_.release();
    } else {
      gamerpic_browser_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
  }
}

void EmulatorWindow::ToggleXMPConfigDialog() {
  if (!xmp_config_dialog_) {
    xmp_config_dialog_ = std::unique_ptr<XMPConfigDialog>(
        new XMPConfigDialog(imgui_drawer_.get(), *this));
  } else {
    xmp_config_dialog_.reset();
  }
}

void EmulatorWindow::ToggleConsoleSettingsDialog() {
  if (!console_settings_dialog_) {
    console_settings_dialog_ =
        std::unique_ptr<ConsoleSettingsDialog>(new ConsoleSettingsDialog(
            imgui_drawer_.get(), *this, emulator_->kernel_state()->xconfig()));
  } else {
    if (console_settings_dialog_->IsClosing()) {
      console_settings_dialog_.release();
    } else {
      console_settings_dialog_.reset();
    }
  }
}

void EmulatorWindow::ToggleContentListDialog() {
  ToggleContentListDialogForTitle(0);
}

void EmulatorWindow::ToggleContentListDialogForTitle(uint32_t title_id) {
  if (!content_list_dialog_) {
#if XE_PLATFORM_WINRT
    if (emulator_->kernel_state()->xam_state()->IsUIActive()) {
      return;
    }
    disable_hotkeys_ = true;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     true);
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(true);
#endif
    content_list_dialog_ = std::make_unique<ContentListDialog>(
        imgui_drawer_.get(), *this,
        emulator_->kernel_state()->content_manager(), title_id);
  } else {
#if XE_PLATFORM_WINRT
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     false);
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
#endif
    if (content_list_dialog_->IsClosing()) {
      content_list_dialog_.release();
    } else {
      content_list_dialog_.reset();
    }
  }
}

void EmulatorWindow::ToggleFriendsDialog(bool guide_style) {
  if (!friends_manager_dialog_) {
    disable_hotkeys_ = true;

    if (emulator_->kernel_state()->xam_state()->IsUIActive()) {
      return;
    }

    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     true);
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

    friends_manager_dialog_ =
        std::make_unique<ManagerDialog>(imgui_drawer_.get(), this, guide_style);
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     false);
    if (friends_manager_dialog_->IsClosing()) {
      friends_manager_dialog_.release();
    } else {
      friends_manager_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
  }
}

void EmulatorWindow::ToggleUpdaterDialog() {
  if (!updater_dialog_) {
    const bool auto_check_update =
        update_info_.valid() ? update_info_.get().update_available : false;

    disable_hotkeys_ = true;

    if (emulator_->kernel_state()->xam_state()->IsUIActive()) {
      return;
    }

    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     true);
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

    updater_dialog_ = std::make_unique<UpdaterDialog>(
        updater_, auto_check_update, imgui_drawer_.get(), this);
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     false);
    if (updater_dialog_->IsClosing()) {
      updater_dialog_.release();
    } else {
      updater_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
  }
}

void EmulatorWindow::ToggleCompletionDialog() {
  if (!updater_completion_dialog_) {
    disable_hotkeys_ = true;

    if (emulator_->kernel_state()->xam_state()->IsUIActive()) {
      return;
    }

    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     true);
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

    updater_completion_dialog_ = std::make_unique<UpdaterCompletionDialog>(
        imgui_drawer_.get(), this, cvar::updated);
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     false);
    if (updater_completion_dialog_->IsClosing()) {
      updater_completion_dialog_.release();
    } else {
      updater_completion_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
  }
}

void EmulatorWindow::ToggleNetplaySettingsDialog() {
  if (!netplay_settings_dialog_) {
    disable_hotkeys_ = true;

    if (emulator_->kernel_state()->xam_state()->IsUIActive()) {
      return;
    }

    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     true);
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

    netplay_settings_dialog_ = std::make_unique<NetplaySettingsDialog>(
        imgui_drawer_.get(), this, emulator_->GetNetworkAdapterManager());
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     false);
    if (netplay_settings_dialog_->IsClosing()) {
      netplay_settings_dialog_.release();
    } else {
      netplay_settings_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
  }
}

void EmulatorWindow::ToggleNetplayStatusDialog() {
  if (!netplay_status_dialog_) {
    disable_hotkeys_ = true;

    if (emulator_->kernel_state()->xam_state()->IsUIActive()) {
      return;
    }

    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     true);
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

    netplay_status_dialog_ = std::make_unique<NetplayStatusDialog>(
        imgui_drawer_.get(), this, emulator_->GetNetworkAdapterManager());
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     false);
    if (netplay_status_dialog_->IsClosing()) {
      netplay_status_dialog_.release();
    } else {
      netplay_status_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
  }
}

void EmulatorWindow::ToggleControllerVibration() {
  auto input_sys = emulator()->input_system();
  if (input_sys) {
    auto input_lock = input_sys->lock();

    input_sys->ToggleVibration();

    if (emulator_->kernel_state()) {
      emulator_->kernel_state()->BroadcastNotification(
          kXNotificationSystemProfileSettingChanged,
          static_cast<uint32_t>(input_sys->GetConnectedSlots().count()));
    }
  }
}

void EmulatorWindow::ShowCompatibility() {
  const std::string_view base_url =
      "https://github.com/xenia-canary/game-compatibility/issues";
  std::string url;
  // Avoid searching for a title ID of "00000000".
  uint32_t title_id = emulator_->title_id();
  if (!title_id) {
    url = base_url;
  } else {
    url = fmt::format("{}?q=is%3Aissue+is%3Aopen+{:08X}", base_url, title_id);
  }
  LaunchWebBrowser(url);
}

void EmulatorWindow::ShowFAQ() {
  LaunchWebBrowser("https://github.com/xenia-canary/xenia-canary/wiki/FAQ");
}

void EmulatorWindow::ShowBuildCommit() {
#ifdef XE_BUILD_IS_PR
  LaunchWebBrowser(
      "https://github.com/AdrianCassar/xenia-canary/pull/" XE_BUILD_PR_NUMBER);
#else
  LaunchWebBrowser(
      "https://github.com/AdrianCassar/xenia-canary/commit/" XE_BUILD_COMMIT);
#endif
}

void EmulatorWindow::UpdateTitle() {
  xe::StringBuffer sb;
  sb.Append(base_title_);

  // Title information, if available
  if (emulator()->is_title_open()) {
    sb.AppendFormat(" | [{:08X}", emulator()->title_id());
    auto title_version = emulator()->title_version();
    if (!title_version.empty()) {
      sb.Append(" v");
      sb.Append(title_version);
    }
    sb.Append("]");

    auto title_name = emulator()->title_name();
    if (!title_name.empty()) {
      sb.Append(" ");
      sb.Append(title_name);
    }
  }

  // Graphics system name, if available
  auto graphics_system = emulator()->graphics_system();
  if (graphics_system) {
    auto graphics_name = graphics_system->name();
    if (!graphics_name.empty()) {
      sb.Append(" <");
      sb.Append(graphics_name);
      sb.Append(">");
    }
  }

  if (Clock::guest_time_scalar() != 1.0) {
    sb.AppendFormat(" (@{:.2f}x)", Clock::guest_time_scalar());
  }

  if (initializing_shader_storage_) {
    sb.Append(" (Preloading shaders\u2026)");
  }

  patcher::Patcher* patcher = emulator()->patcher();
  if (patcher && patcher->IsAnyPatchApplied()) {
    sb.Append(" [Patches Applied]");
  }

  patcher::PluginLoader* pluginloader = emulator()->plugin_loader();
  if (pluginloader && pluginloader->IsAnyPluginLoaded()) {
    sb.Append(" [Plugins Loaded]");
  }

  window_->SetTitle(sb.to_string_view());
}

void EmulatorWindow::SetInitializingShaderStorage(bool initializing) {
  if (initializing_shader_storage_ == initializing) {
    return;
  }
  initializing_shader_storage_ = initializing;
  UpdateTitle();
}

// Notes:
// SDL and XInput both support the guide button
//
// Assumes titles do not use the guide button.
// For titles that do such as dashboards these titles could be excluded based on
// their title ID.
//
// Xbox Gamebar:
// If the Xbox Gamebar overlay is enabled Windows will consume the guide
// button's input, this can be seen using hid-demo.
//
// Workaround: Detect if the Xbox Gamebar overlay is enabled then use the BACK
// button instead of the GUIDE button. Therefore BACK and GUIDE are reserved
// buttons for hotkeys.
//
// This is not an issue with DualShock controllers because Windows will not
// open the gamebar overlay using the PlayStation menu button.
//
// Xbox One S Controller:
// The guide button on this controller is very buggy no idea why.
// Using xinput usually registers after a double tap.
// Doesn't work at all using SDL.
// Needs more testing.
//
// Steam:
// If guide button focus is enabled steam will open.
// Steam uses BACK + GUIDE to open an On-Screen keyboard, however this is not a
// problem since both these buttons are reserved.
const std::map<int, EmulatorWindow::ControllerHotKey> controller_hotkey_map = {
    // Must use the Guide Button for all pass through hotkeys
    {X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ReadbackResolve,
         "A + Guide = Toggle Readback Resolve", true)},
    {X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ToggleLogging,
         "B + Guide = Toggle between loglevel set in config and the 'Disabled' "
         "loglevel.",
         true, true)},
    {X_INPUT_GAMEPAD_Y | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ToggleFullscreen,
         "Y + Guide = Toggle Fullscreen", true)},
    {X_INPUT_GAMEPAD_X | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ClearMemoryPageState,
         "X + Guide = Toggle Clear Memory Page State", true)},

    {X_INPUT_GAMEPAD_RIGHT_SHOULDER | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ClearGPUCache,
         "Right Shoulder + Guide = Clear GPU Cache", true)},
    {X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ToggleControllerVibration,
         "Left Shoulder + Guide = Toggle Controller Vibration", true)},

    // CPU Time Scalar with no rumble feedback
    {X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::CpuTimeScalarSetHalf,
         "D-PAD Down + Guide = Half CPU Scalar")},
    {X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::CpuTimeScalarSetDouble,
         "D-PAD Up + Guide = Double CPU Scalar")},
    {X_INPUT_GAMEPAD_DPAD_RIGHT | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::CpuTimeScalarReset,
         "D-PAD Right + Guide = Reset CPU Scalar")},

    // non-pass through hotkeys
    {X_INPUT_GAMEPAD_Y, EmulatorWindow::ControllerHotKey(
                            EmulatorWindow::ButtonFunctions::ToggleFullscreen,
                            "Y = Toggle Fullscreen", true, false)},
    {X_INPUT_GAMEPAD_START, EmulatorWindow::ControllerHotKey(
                                EmulatorWindow::ButtonFunctions::RunTitle,
                                "Start = Run Selected Title", false, false)},
    {X_INPUT_GAMEPAD_BACK | X_INPUT_GAMEPAD_START,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ToggleLogging,
         "Back + Start = Toggle between loglevel set in config and the "
         "'Disabled' loglevel.",
         false, false)},
    {X_INPUT_GAMEPAD_DPAD_DOWN,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::IncTitleSelect,
         "D-PAD Down = Title Selection +1", true, false)},
    {X_INPUT_GAMEPAD_DPAD_UP,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::DecTitleSelect,
         "D-PAD Up = Title Selection -1", true, false)}};

EmulatorWindow::ControllerHotKey EmulatorWindow::ProcessControllerHotkey(
    int buttons) {
#if XE_PLATFORM_WINRT
  // Just crashes and gets in the way of WinRT's frontend
  return {};
#endif

  // Default return value
  EmulatorWindow::ControllerHotKey Unknown_hotkey = {};

  if (buttons == 0) {
    return Unknown_hotkey;
  }

  if (disable_hotkeys_.load()) {
    return Unknown_hotkey;
  }

  // Hotkey cool-down to prevent toggling too fast
  constexpr std::chrono::milliseconds delay(75);

  // If the Xbox Gamebar is enabled or the Guide button is disabled then
  // replace the Guide button with the Back button without redeclaring the key
  // mappings
  if (IsUseNexusForGameBarEnabled() || !cvars::guide_button) {
    if ((buttons & X_INPUT_GAMEPAD_BACK) == X_INPUT_GAMEPAD_BACK) {
      buttons &= ~X_INPUT_GAMEPAD_BACK;
      buttons |= X_INPUT_GAMEPAD_GUIDE;
    }
  }

  auto it = controller_hotkey_map.find(buttons);
  if (it == controller_hotkey_map.end()) {
    return Unknown_hotkey;
  }

  // Do not activate hotkeys that are not intended for activation during
  // gameplay
  if (emulator_->is_title_open()) {
    // If non-pass through (menu hoykeys) or hotkeys disabled then return
    if (!it->second.title_passthru || !cvars::controller_hotkeys) {
      return Unknown_hotkey;
    }
  }

  std::string notificationTitle = "";
  std::string notificationDesc = "";

  EmulatorWindow::ControllerHotKey button_combination = it->second;

  switch (button_combination.function) {
    case ButtonFunctions::ToggleFullscreen:
      app_context().CallInUIThread([this]() { ToggleFullscreen(); });

      // Extra Sleep
      xe::threading::Sleep(delay);
      break;
    case ButtonFunctions::RunTitle: {
      if (selected_title_index == -1) {
        selected_title_index++;
      }

      if (selected_title_index < recently_launched_titles_.size()) {
        app_context().CallInUIThread([this]() {
          RunTitle(
              recently_launched_titles_[selected_title_index].path_to_file);
        });
      }
    } break;
    case ButtonFunctions::ClearMemoryPageState:
      ToggleGPUSetting(GPUSetting::ClearMemoryPageState);

      // Assume the user wants ClearCaches as well
      if (cvars::clear_memory_page_state) {
        GpuClearCaches();
      }

      notificationTitle = "Toggle Clear Memory Page State";
      notificationDesc =
          cvars::clear_memory_page_state ? "Enabled" : "Disabled";

      // Extra Sleep
      xe::threading::Sleep(delay);
      break;
    case ButtonFunctions::ReadbackResolve:
      CycleReadbackResolve();

      notificationTitle = "Readback Resolve Mode";
      notificationDesc = cvars::readback_resolve;

      // Extra Sleep
      xe::threading::Sleep(delay);
      break;
    case ButtonFunctions::CpuTimeScalarSetHalf:
      CpuTimeScalarSetHalf();

      notificationTitle = "Time Scalar";
      notificationDesc =
          fmt::format("Decreased to {}", Clock::guest_time_scalar());
      break;
    case ButtonFunctions::CpuTimeScalarSetDouble:
      CpuTimeScalarSetDouble();

      notificationTitle = "Time Scalar";
      notificationDesc =
          fmt::format("Increased to {}", Clock::guest_time_scalar());
      break;
    case ButtonFunctions::CpuTimeScalarReset:
      CpuTimeScalarReset();

      notificationTitle = "Time Scalar";
      notificationDesc = fmt::format("Reset to {}", Clock::guest_time_scalar());
      break;
    case ButtonFunctions::ClearGPUCache:
      GpuClearCaches();

      notificationTitle = "Clear GPU Cache";
      notificationDesc = "Complete";

      // Extra Sleep
      xe::threading::Sleep(delay);
      break;
    case ButtonFunctions::ToggleControllerVibration: {
      ToggleControllerVibration();

      bool vibration = false;

      auto input_sys = emulator()->input_system();
      if (input_sys) {
        vibration = input_sys->GetVibrationCvar();
      }

      notificationTitle = "Toggle Controller Vibration";
      notificationDesc = vibration ? "Enabled" : "Disabled";

      // Extra Sleep
      xe::threading::Sleep(delay);
    } break;
    case ButtonFunctions::IncTitleSelect:
      selected_title_index++;
      break;
    case ButtonFunctions::DecTitleSelect:
      selected_title_index--;
      break;
    case ButtonFunctions::ToggleLogging: {
      logging::ToggleLogLevel();

      notificationTitle = "Toggle Logging";

      LogLevel level = static_cast<LogLevel>(logging::internal::GetLogLevel());
      notificationDesc = level == LogLevel::Disabled ? "Disabled" : "Enabled";
    } break;
    case ButtonFunctions::Unknown:
    default:
      break;
  }

  if ((button_combination.function == ButtonFunctions::IncTitleSelect ||
       button_combination.function == ButtonFunctions::DecTitleSelect) &&
      recently_launched_titles_.size() > 0) {
    selected_title_index =
        std::clamp(selected_title_index, 0,
                   static_cast<int32_t>(recently_launched_titles_.size() - 1));

    // Must clear dialogs to prevent stacking
    ClearDialogs();

    // Titles may contain Unicode characters such as At World’s End
    // Must use ImGUI font that can render these Unicode characters
    std::string title_name;

    // Use filename if title name is empty
    if (recently_launched_titles_[selected_title_index].title_name.empty()) {
      title_name = recently_launched_titles_[selected_title_index]
                       .path_to_file.filename()
                       .string();
    } else {
      title_name = recently_launched_titles_[selected_title_index].title_name;
    }

    std::string title = fmt::format(
        "{}: {}\n\n{}", selected_title_index + 1, title_name,
        controller_hotkey_map.find(X_INPUT_GAMEPAD_START)->second.pretty);

    xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_.get(), "Title Selection",
                                        title);
  }

  if (!notificationTitle.empty()) {
    app_context_.CallInUIThread(
        [imgui_drawer = imgui_drawer(), notificationTitle, notificationDesc]() {
          new xe::ui::HostNotificationWindow(imgui_drawer, notificationTitle,
                                             notificationDesc, 0);
        });
  }

  xe::threading::Sleep(delay);

  return it->second;
}

void EmulatorWindow::VibrateController(xe::hid::InputSystem* input_sys,
                                       uint32_t user_index,
                                       bool toggle_rumble) {
  constexpr std::chrono::milliseconds rumble_duration(100);

  // Hold lock while sleeping this thread for the duration of the rumble,
  // otherwise the rumble may fail.
  auto input_lock = input_sys->lock();

  X_INPUT_VIBRATION vibration = {};

  vibration.left_motor_speed = toggle_rumble ? UINT16_MAX : 0;
  vibration.right_motor_speed = toggle_rumble ? UINT16_MAX : 0;

  input_sys->SetState(user_index, &vibration);

  // Vibration duration
  if (toggle_rumble) {
    xe::threading::Sleep(rumble_duration);
  }
}

void EmulatorWindow::GamepadHotKeys() {
  X_INPUT_STATE state;

  constexpr std::chrono::milliseconds thread_delay(75);

  auto input_sys = emulator_->input_system();

  if (input_sys) {
    while (true) {
      // Collect controller states while holding the lock
      std::array<std::pair<bool, X_INPUT_STATE>, XUserMaxUserCount>
          controller_states;
      {
        auto input_lock = input_sys->lock();
        for (uint32_t user_index = 0; user_index < XUserMaxUserCount;
             ++user_index) {
          X_RESULT result = input_sys->GetState(
              user_index, X_INPUT_FLAG::X_INPUT_FLAG_GAMEPAD, &state);
          controller_states[user_index] = {result == X_ERROR_SUCCESS, state};
        }
      }  // Lock is released here when input_lock goes out of scope

      // Process hotkeys without holding the lock
      for (uint32_t user_index = 0; user_index < XUserMaxUserCount;
           ++user_index) {
        if (controller_states[user_index].first) {
          if (ProcessControllerHotkey(
                  controller_states[user_index].second.gamepad.buttons)
                  .rumble) {
            // Enable Vibration
            VibrateController(input_sys, user_index, true);

            // Disable Vibration
            VibrateController(input_sys, user_index, false);
          }
        }
      }

      xe::threading::Sleep(thread_delay);
    }
  }
}

void EmulatorWindow::ToggleGPUSetting(gpu::GPUSetting setting) {
  switch (setting) {
    case GPUSetting::ClearMemoryPageState:
      SaveGPUSetting(GPUSetting::ClearMemoryPageState,
                     !cvars::clear_memory_page_state);
      break;
    case GPUSetting::ReadbackMemexport:
      SaveGPUSetting(GPUSetting::ReadbackMemexport, !cvars::readback_memexport);
      break;
  }
}

void EmulatorWindow::CycleReadbackResolve() {
  const std::string& current = cvars::readback_resolve;
  if (current == "fast") {
    gpu::SetReadbackResolveMode("full");
  } else if (current == "full") {
    gpu::SetReadbackResolveMode("none");
  } else {
    gpu::SetReadbackResolveMode("fast");
  }
}

void EmulatorWindow::DisplayHotKeysConfig() {
  std::string msg = "";
  std::string msg_passthru = "";

  bool guide_enabled = !IsUseNexusForGameBarEnabled() && cvars::guide_button;

  for (auto const& [key, val] : controller_hotkey_map) {
    std::string pretty_text = val.pretty;

    if (!guide_enabled) {
      pretty_text = std::regex_replace(
          pretty_text, std::regex("Guide", std::regex_constants::icase),
          "Back");
    }

    if (emulator_->is_title_open() && !val.title_passthru) {
      pretty_text += " (Disabled)";
    }

    if (val.title_passthru && !cvars::controller_hotkeys) {
      pretty_text += " (Disabled)";
    }

    if (val.title_passthru) {
      msg += pretty_text + "\n";
    } else {
      msg_passthru += pretty_text + "\n";
    }
  }

  // Add Title
  msg.insert(0, "Gameplay Hotkeys\n");

  // Prepend non-passthru hotkeys
  msg_passthru += "\n";
  msg.insert(0, msg_passthru);
  msg += "\n";

  msg += "Readback Resolve: " + cvars::readback_resolve;
  msg += "\n";

  msg += "Clear Memory Page State: " +
         xe::string_util::BoolToString(cvars::clear_memory_page_state);
  msg += "\n";

  msg += "Controller Hotkeys: " +
         xe::string_util::BoolToString(cvars::controller_hotkeys);

  ClearDialogs();
  xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_.get(), "Controller Hotkeys",
                                      msg);
}

std::string EmulatorWindow::CanonicalizeFileExtension(
    const std::filesystem::path& path) {
  return xe::utf8::lower_ascii(xe::path_to_utf8(path.extension()));
}

xe::X_STATUS EmulatorWindow::RunTitle(
    const std::filesystem::path& path_to_file) {
#if XE_PLATFORM_WINRT
  if (title_close_result_.valid()) return X_STATUS_UNSUCCESSFUL;
#endif
  std::error_code ec = {};
  // Game Vault: the game it shared or a network game is there without a look at the drive (a network game's
  // stand-in path is on no drive at all)
  bool titleExists = xe::filesystem::IsGameVaultFile(path_to_file) ||
                     std::filesystem::exists(path_to_file, ec);

  if (path_to_file.empty() || !titleExists) {
    std::string log_msg =
        fmt::format("Failed to launch title path is {}.",
                    path_to_file.empty() ? "empty" : "invalid");

    if (!path_to_file.empty() && !titleExists) {
      log_msg.append(fmt::format("\nProvided Path: {}", path_to_file));
      // Game Vault: whether the network game / shared file was registered at all
      log_msg.append(fmt::format(
          "\nGame Vault: network game here: {}, shared or network file: {}",
          xe::filesystem::GetNetFile(path_to_file) != nullptr,
          xe::filesystem::IsGameVaultFile(path_to_file)));
    }

    if (ec) {
      log_msg.append(fmt::format("\nExtended message info: {} ({:08X})",
                                 ec.message(), ec.value()));
    }

    XELOGE("{}", log_msg);

    ClearDialogs();

#if XE_PLATFORM_WINRT
    if (gamelist_) {
      imgui_drawer_->AddDialog(gamelist_.get());
    }
#endif
    xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_.get(),
                                        "Title Launch Failed!", log_msg);

    return X_STATUS_NO_SUCH_FILE;
  }

  if (emulator_->is_title_open()) {
    // Terminate the current title and start a new title.
    // if (emulator_->TerminateTitle() == X_STATUS_SUCCESS) {
    //   return RunTitle(path);
    // }

    return X_STATUS_UNSUCCESSFUL;
  }

  // Prevent crashing the emulator by not loading a game if a game is already
  // loaded.
  auto abs_path = std::filesystem::absolute(path_to_file);

  auto extension = CanonicalizeFileExtension(abs_path);

  if (extension == ".7z" || extension == ".zip" || extension == ".rar" ||
      extension == ".tar" || extension == ".gz") {
    xe::ShowSimpleMessageBox(
        xe::SimpleMessageBoxType::Error,
        fmt::format(
            "Unsupported format!\n"
            "Xenia does not support running software in an archived format."));

    return X_STATUS_UNSUCCESSFUL;
  }

#if XE_PLATFORM_WINRT
  if (gamelist_) gamelist_->StopVideoBackground();
  if (dsu_) dsu_->OnLaunching();
#endif
  auto result = emulator_->LaunchPath(abs_path);
#if XE_PLATFORM_WINRT
  if (dsu_) dsu_->OnLaunch(abs_path, result == X_STATUS_SUCCESS);
#endif

  disable_hotkeys_ = false;

  ClearDialogs();

  if (result) {
    XELOGE("Failed to launch target: {:08X}", result);

#if XE_PLATFORM_WINRT
    if (gamelist_) {
      imgui_drawer_->AddDialog(gamelist_.get());
    }
#endif
    xe::ui::ImGuiDialog::ShowMessageBox(
        imgui_drawer_.get(), "Title Launch Failed!",
        fmt::format("Failed to launch title.\n\nPath: {}\nStatus: {:08X}\n\n"
                    "Check xenia.log for technical details.",
                    xe::path_to_utf8(abs_path), result));

    // Keep persistent storage and profile mounts available for the frontend
    // and subsequent launches. MountPath rolls back failed package devices.
  } else {
    AddRecentlyLaunchedTitle(path_to_file,
                             std::string(emulator_->title_name()));

    auto xam =
        emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
            "xam.xex");

    xam->loader_data().host_path = xe::path_to_utf8(abs_path);
  }

  return result;
}

void EmulatorWindow::RunPreviouslyPlayedTitle() {
  if (recently_launched_titles_.size() >= 1) {
    RunTitle(recently_launched_titles_[0].path_to_file);
  }
}

void EmulatorWindow::FillRecentlyLaunchedTitlesMenu(
    xe::ui::MenuItem* recent_menu) {
#if !XE_PLATFORM_WINRT
  for (int i = 0; i < recently_launched_titles_.size(); ++i) {
    std::string hotkey = (i == 0) ? "F9" : "";

    const RecentTitleEntry& entry = recently_launched_titles_[i];
    const std::string item_text = entry.title_name.empty()
                                      ? entry.path_to_file.string()
                                      : entry.title_name;

    recent_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, item_text, hotkey,
        std::bind(&EmulatorWindow::RunTitle, this, entry.path_to_file)));
  }
#endif
}

void EmulatorWindow::LoadRecentlyLaunchedTitles() {
  std::ifstream file(emulator()->storage_root() /
                     kRecentlyPlayedTitlesFilename);
  if (!file.is_open()) {
    return;
  }

  toml::parse_result parsed_file;
  try {
    parsed_file = toml::parse(file);
  } catch (toml::parse_error& exception) {
    XELOGE("Cannot parse file: recent.toml. Error: {}", exception.what());
    return;
  }

  if (parsed_file.is_table()) {
    for (const auto& [index, entry] : *parsed_file.as_table()) {
      if (!entry.is_table()) {
        continue;
      }

      const toml::table* entry_table = entry.as_table();

      std::string title_name =
          entry_table->get_as<std::string>("title_name")->get();
      std::string path = entry_table->get_as<std::string>("path")->get();
      std::time_t last_run_time =
          entry_table->get_as<int64_t>("last_run_time")->get();

      std::error_code ec = {};
      if (path.empty() || !std::filesystem::exists(path, ec)) {
        continue;
      }

      recently_launched_titles_.push_back({title_name, path, last_run_time});
    }
  }
}

void EmulatorWindow::AddRecentlyLaunchedTitle(
    std::filesystem::path path_to_file, std::string title_name) {
  if (cvars::recent_titles_entry_amount <= 0) {
    return;
  }

  // Check if game is already on list and pop it to front
  auto entry_index =
      std::ranges::find_if(std::as_const(recently_launched_titles_),
                           [&title_name](const RecentTitleEntry& entry) {
                             return entry.title_name == title_name;
                           });
  if (entry_index != recently_launched_titles_.cend()) {
    recently_launched_titles_.erase(entry_index);
  }

  recently_launched_titles_.insert(recently_launched_titles_.cbegin(),
                                   {title_name, path_to_file, time(nullptr)});
  // Serialize to toml
  auto toml_table = toml::table();

  uint8_t index = 0;
  for (const RecentTitleEntry& entry : recently_launched_titles_) {
    auto entry_table = toml::table();

    // Fill entry under specific index.
    std::string str_path = xe::path_to_utf8(entry.path_to_file);
    entry_table.insert("title_name", entry.title_name);
    entry_table.insert("path", str_path);
    entry_table.insert("last_run_time", entry.last_run_time);

    toml_table.insert(std::to_string(index++), entry_table);

    if (index >= cvars::recent_titles_entry_amount) {
      break;
    }
  }
  // Open and write serialized data.
  std::ofstream file(emulator()->storage_root() / kRecentlyPlayedTitlesFilename,
                     std::ofstream::trunc);
  file << toml_table;
  file.close();
}

void EmulatorWindow::ClearDialogs() {
#if XE_PLATFORM_WINRT
  // Cancel a guide selection that was queued before a title launch/reset.
  host_guide_lifetime_ = std::make_shared<int>(0);
  host_guide_.reset();
#endif
  if (profile_config_dialog_) {
    profile_config_dialog_.reset();
  }

  if (display_config_dialog_) {
    display_config_dialog_.reset();
  }

  if (console_settings_dialog_) {
    console_settings_dialog_.reset();
  }

  if (content_list_dialog_) {
    content_list_dialog_.reset();
  }

  if (xmp_config_dialog_) {
    xmp_config_dialog_.reset();
  }

  if (friends_manager_dialog_) {
    friends_manager_dialog_.reset();
  }

  if (gamerpic_browser_dialog_) {
    gamerpic_browser_dialog_.reset();
  }

  if (updater_dialog_) {
    updater_dialog_.reset();
  }

  if (updater_completion_dialog_) {
    updater_completion_dialog_.reset();
  }

  if (netplay_settings_dialog_) {
    netplay_settings_dialog_.reset();
  }

  if (netplay_status_dialog_) {
    netplay_status_dialog_.reset();
  }

  imgui_drawer_.get()->ClearDialogs();
  emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
}

#if XE_PLATFORM_WINRT
void EmulatorWindow::BeginTitleClose(
    std::optional<kernel::xam::LoaderData> restart) {
  if (!emulator_->is_title_open() || title_close_result_.valid()) return;
  XELOGI("UWP {} requested; frontend return: {}",
         restart ? "Restart Game" : "Quit Game",
         UWP::ShouldReturnToFrontend());
  xe::FlushLog();
  title_restart_data_ = std::move(restart);
  ClearDialogs();
  ShutdownDsu();
  emulator_initialized_ = false;
  disable_hotkeys_ = true;
  imgui_drawer_->SetIgnoreInput(true);
  UWP::SetCompanionTick([this]() { CompleteTitleClose(); });
  title_close_result_ = std::async(std::launch::async,
                                  [this]() { return emulator_->ResetTitle(); });
}

void EmulatorWindow::WaitForTitleClose() {
  if (title_close_result_.valid()) title_close_result_.wait();
}

void EmulatorWindow::CompleteTitleClose() {
  if (!title_close_result_.valid() ||
      title_close_result_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready || !UWP::ArePickerControlsReleased()) {
    return;
  }
  X_STATUS result = X_STATUS_UNSUCCESSFUL;
  try {
    result = title_close_result_.get();
  } catch (const std::exception& error) {
    XELOGE("Could not close title: {}", error.what());
  }
  auto restart = std::exchange(title_restart_data_, std::nullopt);
  imgui_drawer_->GetIO().ClearEventsQueue();
  imgui_drawer_->SetIgnoreInput(false);
  if (XFAILED(result)) {
    xe::FlushLog();
    ui::ImGuiDialog::ShowMessageBox(
        imgui_drawer_.get(), "Unable to close game",
        "The game runtime could not be reset. Close and reopen Xenia.");
    return;
  }
  emulator_initialized_ = true;
  disable_hotkeys_ = false;
  UWP::RegisterInputSystem(emulator_->input_system());
  dsu_ = std::make_unique<DsuIntegration>(*this);
  UWP::SetCompanionTick([this]() { if (dsu_) dsu_->Tick(); });
  if (restart) {
    auto xam = emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
        "xam.xex");
    const auto host_path = std::filesystem::u8path(restart->host_path);
    xam->loader_data() = std::move(*restart);
    const auto launch_result = RunTitle(host_path);
    XELOGI("Restart Game completed in process: {:08X}", launch_result);
    xe::FlushLog();
    return;
  }
  if (!gamelist_) {
    gamelist_ = std::make_unique<WinRTFrontendDialog>(imgui_drawer_.get(), *this);
  } else {
    imgui_drawer_->AddDialog(gamelist_.get());
  }
  UpdateTitle();
  XELOGI("Quit Game completed in process; library ready");
  xe::FlushLog();
  UWP::ReturnToFrontend(imgui_drawer_.get());
}

void EmulatorWindow::ShowHostGuide() {
  if (!emulator_initialized_ || host_guide_ ||
      imgui_drawer_->GetIgnoreInput()) {
    return;
  }
  const auto* xam_state = emulator_->kernel_state()->xam_state();
  const size_t background_dialogs =
      !emulator_->is_title_open() && gamelist_ ? 1 : 0;
  if (xam_state->IsUIActive() ||
      imgui_drawer_->GetDialogCount() > background_dialogs) {
    return;
  }
  imgui_drawer_->GetIO();  // Select the drawer's context for the popup query.
  if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                      ImGuiPopupFlags_AnyPopupLevel)) {
    return;
  }
  disable_hotkeys_ = true;
  emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                   true);
  emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(true);
  host_guide_ =
      std::make_unique<WinRTHostGuideDialog>(imgui_drawer_.get(), *this);
}

void EmulatorWindow::HostGuideClosed(WinRTHostGuideDialog* dialog) {
  if (host_guide_.get() != dialog) {
    return;
  }
  // ImGuiDialog::Draw deletes a closing dialog after OnClose returns.
  host_guide_.release();
  const std::weak_ptr<int> lifetime = host_guide_lifetime_;
  // The current ImGui draw data still references these textures after the
  // dialog self-deletes. Keep them alive until this frame has been submitted.
  app_context_.CallInUIThreadDeferred([this, lifetime,
                                       background = dialog->background_,
                                       button_a = dialog->button_a_,
                                       button_b = dialog->button_b_,
                                       action = dialog->close_action_]() {
    if (lifetime.expired()) {
      return;
    }
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     false);
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
    window_->Focus();
    if (action) {
      ExecuteHostGuideAction(*action);
    }
  });
}

void EmulatorWindow::ExecuteHostGuideAction(HostGuideAction action) {
  if (action == HostGuideAction::kExitXenia) {
    UWP::RequestQuit();
    return;
  }
  if (!emulator_->is_title_open()) {
    return;
  }
  if (action == HostGuideAction::kQuitGame) {
    FileClose();
    return;
  }
  if (action != HostGuideAction::kRestartGame) {
    return;
  }
  auto* kernel_state = emulator_->kernel_state();
  const auto module = kernel_state->GetExecutableModule();
  auto xam = kernel_state->GetKernelModule<kernel::xam::XamModule>("xam.xex");
  auto* entry =
      module ? emulator_->file_system()->ResolvePath(module->path()) : nullptr;
  if (!entry || !xam || xam->loader_data().host_path.empty()) {
    ui::ImGuiDialog::ShowMessageBox(
        imgui_drawer_.get(), "Unable to restart game",
        "The running game's launch path is unavailable.");
    return;
  }
  auto loader_data = xam->loader_data();
  loader_data.launch_path = entry->path();
  auto restart = kernel::xam::PrepareTitleRestartData(loader_data);
  if (!restart) {
    ui::ImGuiDialog::ShowMessageBox(
        imgui_drawer_.get(), "Unable to restart game",
        "The running game's launch path is invalid.");
    return;
  }
  BeginTitleClose(std::move(restart));
}

void EmulatorWindow::HostGuideOpenPage(WinRTHostGuideDialog* dialog,
                                       HostGuideAction action) {
  if (host_guide_.get() != dialog) {
    return;
  }
  dialog->navigation_.OpenChild(static_cast<int>(action));
  const std::weak_ptr<int> lifetime = host_guide_lifetime_;
  // Start after the guide frame ends. Keep the parent registered so guest input
  // remains captured across the transition, including self-owned child pages.
  app_context_.CallInUIThreadDeferred([this, lifetime, dialog, action]() {
    if (lifetime.expired() || host_guide_.get() != dialog ||
        !dialog->navigation_.launch_pending()) {
      return;
    }
    // The child entry points reserve the XAM UI flag themselves. This is an
    // ownership handoff, so no UI-closed notification is needed here.
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
    switch (action) {
      case HostGuideAction::kFriends:
        ToggleFriendsDialog(true);
        break;
      case HostGuideAction::kProfiles:
        ToggleProfilesConfigDialog();
        break;
      case HostGuideAction::kNetworkSettings:
        ToggleNetplaySettingsDialog();
        break;
      case HostGuideAction::kNetworkStatus:
        ToggleNetplayStatusDialog();
        break;
      case HostGuideAction::kGamerpics:
        ToggleGamerpicBrowserDialog();
        break;
      default:
        break;
    }
    dialog->navigation_.ChildLaunched();
  });
}

void EmulatorWindow::WinRTHostGuideDialog::OnClose() {
  navigation_.Close();
  emulator_window_.HostGuideClosed(this);
}

void EmulatorWindow::WinRTHostGuideDialog::OnDraw(ImGuiIO& io) {
  if (!navigation_.visible()) {
    const bool descendant_dialogs_open =
        imgui_drawer()->GetDialogCount() > background_dialog_count_ + 1;
    const bool popup_open = ImGui::IsPopupOpen(
        nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (!navigation_.Resume(descendant_dialogs_open, popup_open)) {
      return;
    }
    // The previous child may have cleared the shared UI flag on close. Restore
    // it only for this still-live guide, never for unrelated root dialogs.
    emulator_window_.disable_hotkeys_ = true;
    auto* kernel_state = emulator_window_.emulator_->kernel_state();
    kernel_state->BroadcastNotification(kXNotificationSystemUI, true);
    kernel_state->xam_state()->is_xam_dialog_present_.store(true);
    focus_requested_ = true;
  }
  navigation_.ObserveInputRelease(
      !ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) &&
      !ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight) &&
      !ImGui::IsKeyDown(ImGuiKey_Enter) &&
      !ImGui::IsKeyDown(ImGuiKey_KeypadEnter) &&
      !ImGui::IsKeyDown(ImGuiKey_Space) && !ImGui::IsKeyDown(ImGuiKey_Escape) &&
      !ImGui::IsAnyMouseDown());
  const auto* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  if (focus_requested_) {
    ImGui::SetNextWindowFocus();
  }
  const auto flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
  if (!ImGui::Begin("##uwp_host_guide", nullptr, flags)) {
    ImGui::End();
    return;
  }
  const auto guide_path = ResolveConfiguredGuideBackgroundAssetPath();
  if (!background_ || guide_path != background_texture_path_) {
    imgui_drawer()->RetainTextureForFrame(background_);
    background_ = LoadOverlayTexture(imgui_drawer(), guide_path.c_str(), true);
    background_texture_path_ = guide_path;
  }
  if (!button_a_) {
    button_a_ = LoadButtonTexture(imgui_drawer(), 'A');
  }
  if (!button_b_) {
    button_b_ = LoadButtonTexture(imgui_drawer(), 'B');
  }
  auto* draw_list = ImGui::GetWindowDrawList();
  const ImVec2 panel_size = GetGuidePanelSize(background_, io.DisplaySize.y);
  const ImVec2 panel_min = viewport->Pos;
  const ImVec2 panel_max(panel_min.x + panel_size.x,
                         panel_min.y + panel_size.y);
  draw_list->AddRectFilled(viewport->Pos,
                           ImVec2(viewport->Pos.x + viewport->Size.x,
                                  viewport->Pos.y + viewport->Size.y),
                           IM_COL32(0, 0, 0, 140));
  DrawGuidePanelBackground(draw_list, background_, panel_min, panel_max);
  const float ux = io.DisplaySize.x / 1024.0f;
  const float uy = io.DisplaySize.y / 576.0f;
  const ImVec2 padding = GetGuidePanelPadding();
  const auto content_bounds =
      ui::GetGuideContentBounds(panel_size.x, viewport->Size.x);
  const auto header = DrawOverlayHeader(draw_list, panel_min, panel_size,
                                        padding, ux, uy, "Xenia Guide");
  const auto body = ui::GetGuideBodyBounds(panel_size.y);
  const float content_y =
      std::max(panel_min.y + body.top,
               header.position.y + header.font_size + 45.0f * uy);
  ImGui::SetCursorScreenPos(
      ImVec2(panel_min.x + content_bounds.left - 2.0f * ux, content_y));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild(
      "##host_guide_actions",
      ImVec2(content_bounds.width() + 4.0f * ux,
             std::max(1.0f, panel_min.y + body.bottom - content_y)),
      ImGuiChildFlags_None,
      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  ImGui::Dummy(ImVec2(0, 2.0f * uy));
  const ImVec2 button_size(content_bounds.width(), 34.0f * uy);
  ImGui::BeginDisabled(!navigation_.input_armed());
  const auto action_button = [&](const char* label, HostGuideAction action,
                                 bool requires_game = false) {
    ImGui::SetCursorScreenPos(ImVec2(panel_min.x + content_bounds.left,
                                     ImGui::GetCursorScreenPos().y));
    if (focus_requested_ && navigation_.input_armed() &&
        navigation_.selected_option() == static_cast<int>(action)) {
      ImGui::SetKeyboardFocusHere();
      ImGui::SetNavCursorVisible(true);
      focus_requested_ = false;
    }
    ImGui::BeginDisabled(requires_game &&
                         !emulator_window_.emulator_->is_title_open());
    if (DrawTextEffectButton(label, button_size)) {
      if (action == HostGuideAction::kContinue) {
        Close();
      } else if (action == HostGuideAction::kRestartGame ||
                 action == HostGuideAction::kQuitGame ||
                 action == HostGuideAction::kExitXenia) {
        close_action_ = action;
        Close();
      } else {
        emulator_window_.HostGuideOpenPage(this, action);
      }
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
  };
  action_button("Continue", HostGuideAction::kContinue);
  action_button("Restart Game", HostGuideAction::kRestartGame, true);
  action_button("Quit Game", HostGuideAction::kQuitGame, true);
  action_button("Friends", HostGuideAction::kFriends);
  action_button("Profiles", HostGuideAction::kProfiles);
  action_button("Network Settings", HostGuideAction::kNetworkSettings);
  action_button("Network Status", HostGuideAction::kNetworkStatus);
  action_button("Gamerpic Browser", HostGuideAction::kGamerpics);
  action_button("Exit Xenia", HostGuideAction::kExitXenia);
  ImGui::EndDisabled();
  ImGui::EndChild();
  if (navigation_.visible() && navigation_.input_armed() &&
      ConsumeGuideBackPress()) {
    Close();
  }
  DrawFooterPrompt(draw_list, button_b_, 13.5f * uy, 15.6f * uy, "Back",
                   panel_max.y - 42.0f * uy, panel_min.x + 44.0f * ux,
                   -20.0f * ux);
  DrawFooterPrompt(draw_list, button_a_, 13.5f * uy, 15.6f * uy, "Select",
                   panel_max.y - 25.0f * uy, panel_min.x + 44.0f * ux,
                   -20.0f * ux);
  ImGui::End();
}

void EmulatorWindow::WinRTFrontendDialog::OpenPatchEditor(
    const std::filesystem::path& directory, const std::string& title_id) {
  if (!patch_editor_) patch_editor_ = std::make_unique<GamePatchDialog>();
  patch_editor_->Open(directory, title_id);
  show_game_context_menu_ = false;
  action_popup_should_close_ = true;
  show_action_status_ = false;
}

EmulatorWindow::WinRTFrontendDialog::~WinRTFrontendDialog() {
  async_lifetime_.reset();
  StopVideoBackground();
  if (storage_folder_result_) {
    imgui_drawer()->SetIgnoreInput(false);
  }
}

void EmulatorWindow::WinRTFrontendDialog::StopVideoBackground() {
  if (background_video_) background_video_->Stop();
  if (background_video_texture_) imgui_drawer()->RetainTextureForFrame(background_video_texture_);
  background_video_texture_.reset();
  background_video_path_.clear();
  background_video_error_.clear();
}

void EmulatorWindow::WinRTFrontendDialog::ApplyStorageFolder(
    StorageFolderTarget target, const std::string& path) {
  if (path.empty()) {
    return;
  }
  storage_path_status_.clear();
  if (target == StorageFolderTarget::kMedia) {
    const auto root = std::filesystem::u8path(path);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec || !UWP::TestPathPermissions(path)) {
      storage_path_status_ = "The selected media folder is not writable.";
    } else if (!emulator_window_.screenscraper_->SetMediaRoot(root)) {
      storage_path_status_ = "Cancel the current ScreenScraper job before changing folders.";
    } else {
      storage_path_status_ = "Media location saved. Existing downloads stay in their current folders.";
    }
    return;
  }
  if (target != StorageFolderTarget::kConfig) {
    UWP::SetGamePaths(UWP::SelectGameStoragePath(
        UWP::GetPaths(), path, target == StorageFolderTarget::kPrimaryGames));
    cached_game_title_ids_.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::u8path(path), ec)) {
      storage_path_status_ =
          "Games directory saved. Connect the drive or create the folder, then "
          "select Refresh List.";
    }
    return;
  }
  if (emulator_window_.emulator()->is_title_open()) {
    storage_path_status_ = "Close the game before changing config folders.";
    return;
  }
  const auto root = std::filesystem::u8path(path);
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec || !UWP::TestPathPermissions(path)) {
    storage_path_status_ = "The selected config folder is not writable.";
    return;
  }
  for (const auto* name : {"cache_root", "content_root", "storage_root"}) {
    auto* variable = dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(
        cvar::ConfigVars->find(name)->second);
    const std::string folder(name, std::strlen(name) - std::strlen("_root"));
    variable->SetConfigValue(root / folder);
  }
  config::SaveConfig();
  storage_path_status_ =
      "Config folder locations saved. Restart Xenia to use them. Existing "
      "profiles and saves stay in their current folders.";
}

void EmulatorWindow::WinRTFrontendDialog::PickStorageFolder(
    StorageFolderTarget target) {
  if (storage_folder_result_) {
    return;
  }
  auto result = std::make_shared<StorageFolderResult>();
  result->target = target;
  storage_folder_result_ = result;
  storage_path_status_.clear();
  imgui_drawer()->SetIgnoreInput(true);
  const wchar_t* token = target == StorageFolderTarget::kPrimaryGames
                             ? L"xenia-primary-game-folder"
                         : target == StorageFolderTarget::kAdditionalGames
                             ? L"xenia-additional-game-folder"
                         : target == StorageFolderTarget::kMedia
                             ? L"xenia-media-folder"
                             : L"xenia-config-folder";
  // The picker may outlive this frontend. Completion owns only the mailbox;
  // configuration, scanning and ImGui input are updated by the next draw.
  UWP::SelectStorageFolder(
      token, [result](std::string path, std::string error) {
        std::lock_guard lock(result->mutex);
        result->path = std::move(path);
        result->error = std::move(error);
        result->ready = true;
      });
}

void EmulatorWindow::WinRTFrontendDialog::CompleteStorageFolderPicker() {
  const auto result = storage_folder_result_;
  if (!result) {
    return;
  }
  std::lock_guard lock(result->mutex);
  // Ignoring input clears ImGui's key history. Read physical controls so the
  // button used to finish/cancel the native picker cannot become a fresh press
  // in the restored frontend.
  if (!result->ready || !UWP::ArePickerControlsReleased()) {
    return;
  }
  storage_folder_result_.reset();
  imgui_drawer()->SetIgnoreInput(false);
  storage_focus_requested_ = true;
  if (!result->error.empty()) {
    storage_path_status_ = "Could not select the folder: " + result->error;
  } else {
    ApplyStorageFolder(result->target, result->path);
  }
}

void EmulatorWindow::WinRTFrontendDialog::DrawNoProfilePrompt(ImGuiIO& io) {
  static_cast<void>(io);
  auto* emulator = emulator_window_.emulator();
  if (!emulator || !emulator->kernel_state() ||
      !emulator->kernel_state()->xam_state()) {
    return;
  }

  auto* profile_manager =
      emulator->kernel_state()->xam_state()->profile_manager();
  if (!profile_manager) {
    return;
  }

  const bool has_profiles = profile_manager->GetAccountCount() > 0;
  if (has_profiles) {
    no_profile_prompt_dismissed_ = false;
    return;
  }

  if (no_profile_prompt_dismissed_ || imgui_drawer()->GetDialogCount() > 1) {
    return;
  }

  no_profile_prompt_dismissed_ = true;
  // Game Vault: a profile is made for the player instead of asking first
  if (UWP::EnsureProfile(emulator)) {
    return;
  }
  new NoProfileDialog(imgui_drawer(), &emulator_window_);
}

void EmulatorWindow::WinRTFrontendDialog::OnDraw(ImGuiIO& io) {
  CompleteStorageFolderPicker();
  if (!emulator_window_.emulator_initialized_) {
    return;
  }
  if (title_update_dialog_) {
    title_update_dialog_->ProcessPending(emulator_window_.emulator());
  }
  auto& scraper = *emulator_window_.screenscraper_;
  scraper.BeginFrame();
  if (artwork_revision_ != scraper.revision()) {
    for (const auto& [path, texture] : game_image_textures_) {
      imgui_drawer()->RetainTextureForFrame(texture);
    }
    game_image_textures_.clear();
    artwork_revision_ = scraper.revision();
  }
  if (UWP::HasGamePath()) {
    emulator_window_.RunTitle(std::filesystem::u8path(UWP::ConsumeGamePath()));
    return;
  }

  const bool game_editor_open = scraper.MetadataEditorOpen() ||
      (patch_editor_ && patch_editor_->is_open()) ||
      (title_update_dialog_ && title_update_dialog_->is_open());
  const bool frontend_accepts_input = !game_editor_open && !imgui_drawer()->GetIgnoreInput() &&
                                      imgui_drawer()->GetDialogCount() == 1;
  const auto key_pressed = [&](ImGuiKey key, bool repeat = true) {
    return frontend_accepts_input && ImGui::IsKeyPressed(key, repeat);
  };
  const auto key_down = [&](ImGuiKey key) {
    return frontend_accepts_input && ImGui::IsKeyDown(key);
  };
  float display_scale = ((float)io.DisplaySize.x / 1920.0f);
  const float frontend_width = io.DisplaySize.x * 0.92f;
  const float frontend_height = io.DisplaySize.y * 0.88f;
  ImGui::SetNextWindowSize(ImVec2(frontend_width, frontend_height));
  ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - frontend_width) * 0.5f,
                                 (io.DisplaySize.y - frontend_height) * 0.5f));

  auto flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar |
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize;
  if (!frontend_accepts_input) {
    flags = flags | ImGuiWindowFlags_NoInputs;
  }

  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_NavWindowingHighlight,
                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_NavWindowingDimBg,
                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  bool launch_after_draw = false;
  std::filesystem::path launch_path_after_draw;

  ImGui::BeginDisabled(game_editor_open);
  if (ImGui::Begin("Frontend", nullptr, flags)) {
    ImDrawList* frontend_draw_list = ImGui::GetWindowDrawList();
    ImDrawListSplitter frontend_splitter;
    frontend_splitter.Split(frontend_draw_list, 4);

    // Channel 0: Static background (bottom layer)
    // Channel 1: Dynamic game background (middle layer)
    // Channel 2: Interface overlay (top layer)
    // Channel 3: UI elements

    // Always draw background as base layer (bottom layer)
    frontend_splitter.SetCurrentChannel(frontend_draw_list, 0);
    if (auto fallback_tex = GetOrCreateBackgroundFallback()) {
      frontend_draw_list->PushClipRectFullScreen();
      frontend_draw_list->AddImage(
          reinterpret_cast<ImTextureID>(fallback_tex.get()), ImVec2(0, 0),
          io.DisplaySize);
      frontend_draw_list->PopClipRect();
    }

    // Draw interface overlay on top if available (top layer)
    frontend_splitter.SetCurrentChannel(frontend_draw_list, 2);
    if (auto background_tex = GetOrCreateBackground()) {
      frontend_draw_list->PushClipRectFullScreen();
      frontend_draw_list->AddImage(
          reinterpret_cast<ImTextureID>(background_tex.get()), ImVec2(0, 0),
          io.DisplaySize);
      frontend_draw_list->PopClipRect();
    }

    frontend_splitter.SetCurrentChannel(frontend_draw_list, 3);

    const int page_count = static_cast<int>(FrontendPage::kCount);
    if (key_pressed(ImGuiKey_GamepadL1, false)) {
      int page_index = static_cast<int>(active_frontend_page_);
      page_index = (page_index + page_count - 1) % page_count;
      active_frontend_page_ = static_cast<FrontendPage>(page_index);
    }
    if (key_pressed(ImGuiKey_GamepadR1, false)) {
      int page_index = static_cast<int>(active_frontend_page_);
      page_index = (page_index + 1) % page_count;
      active_frontend_page_ = static_cast<FrontendPage>(page_index);
    }

    auto draw_nav_button = [this, display_scale](const char* label,
                                                 FrontendPage page) {
      bool selected = active_frontend_page_ == page;
      const ImVec4 accent_border = GetConfiguredUIAccentColor(0.75f);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_Border, accent_border);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
      ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
      if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.92f, 0.72f, 1.0f));
      }
      bool changed = ImGui::Button(label, ImVec2(-1.0f, 56.0f * display_scale));
      if (selected) {
        ImGui::PopStyleColor();
      }
      ImGui::PopItemFlag();
      ImGui::PopStyleColor(5);
      ImGui::PopStyleVar();
      if (changed) {
        active_frontend_page_ = page;
      }
    };

    auto draw_text_button = [display_scale](const char* id, const char* text,
                                            ImVec2 size = ImVec2(0.0f, 0.0f),
                                            ImU32 color = 0) {
      const ImGuiStyle& style = ImGui::GetStyle();
      if (size.x <= 0.0f && size.y <= 0.0f) {
        const ImVec2 text_size = ImGui::CalcTextSize(text);
        size = ImVec2(text_size.x + style.FramePadding.x * 2.0f,
                      text_size.y + style.FramePadding.y * 2.0f);
      }

      std::string label = fmt::format("{}{}", text, id);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                            ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
      ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
      bool pressed = ImGui::Button(label.c_str(), size);
      bool highlighted = ImGui::IsItemHovered() || ImGui::IsItemActive() ||
                         ImGui::IsItemFocused();
      ImGui::PopStyleColor(5);

      ImVec2 rect_min = ImGui::GetItemRectMin();
      ImVec2 rect_max = ImGui::GetItemRectMax();
      ImVec2 text_size = ImGui::CalcTextSize(text);
      ImVec2 text_pos(
          rect_min.x + (rect_max.x - rect_min.x - text_size.x) * 0.5f,
          rect_min.y + (rect_max.y - rect_min.y - text_size.y) * 0.5f);
      if (highlighted) {
        const ImVec4 button_border_color = GetConfiguredUIAccentColor(0.95f);
        const float rounding = style.FrameRounding > 0.0f
                                   ? style.FrameRounding
                                   : 8.0f * display_scale;
        const float border_thickness = std::max(1.5f, 2.0f * display_scale);
        ImGui::GetWindowDrawList()->AddRect(
            rect_min, rect_max, ImGui::GetColorU32(button_border_color),
            rounding, 0, border_thickness);
      }

      ImU32 text_color = color ? color : IM_COL32(255, 255, 255, 255);
      DrawTextWithConfiguredEffect(ImGui::GetWindowDrawList(), ImGui::GetFont(),
                                   ImGui::GetFontSize(), text_pos, text_color,
                                   text);
      return pressed;
    };

    if (ImGui::BeginTable(
            "##frontend_blade_layout", 3,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings,
            ImGui::GetContentRegionAvail())) {
      const float rail_width = std::max(100.0f, 118.0f * display_scale);
      ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthFixed,
                              rail_width);
      ImGui::TableSetupColumn("main", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthFixed,
                              rail_width);

      ImGui::TableNextColumn();
      if (ImGui::BeginChild("##left_rail", ImVec2(0, 0), false,
                            ImGuiWindowFlags_NoScrollbar)) {
        draw_nav_button("##games_tab", FrontendPage::kGameList);
        draw_nav_button("##settings_tab", FrontendPage::kSettings);
      }
      ImGui::EndChild();

      ImGui::TableNextColumn();
      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * display_scale);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                          ImVec2(6.0f * display_scale, 6.0f * display_scale));
      if (ImGui::BeginChild("##main_page", ImVec2(0, 0), false,
                            ImGuiWindowFlags_NoScrollbar)) {
        bool game_list_tab_open =
            active_frontend_page_ == FrontendPage::kGameList;
        bool settings_tab_open =
            active_frontend_page_ == FrontendPage::kSettings;
        bool paths_tab_open = active_frontend_page_ == FrontendPage::kPaths;
        bool about_tab_open = active_frontend_page_ == FrontendPage::kAbout;
        static bool game_list_tab_was_open = false;
        static bool settings_tab_was_open = false;
        static bool paths_tab_was_open = false;
        static bool about_tab_was_open = false;
        static bool gamelist_focus_requested = false;
        static bool gamelist_focus_selected_request = false;
        static bool gamelist_window_focus_requested = false;
        const bool remote_selection_this_frame = remote_focus_requested_;
        if (remote_selection_this_frame) {
          gamelist_focus_requested = true;
          gamelist_window_focus_requested = true;
          remote_focus_requested_ = false;
        }
        static int settings_selected_section = 0;
        static bool settings_focus_requested = false;
        static bool settings_focus_right_requested = false;
        static bool settings_focus_on_right = false;
        static bool paths_focus_requested = false;
        if (storage_focus_requested_) {
          if (active_frontend_page_ == FrontendPage::kSettings) {
            settings_focus_right_requested = true;
          } else {
            paths_focus_requested = true;
          }
          storage_focus_requested_ = false;
        }
        static bool about_focus_requested = false;
        static bool gamelist_x_was_down = false;
        static bool gamelist_b_was_down = false;
        static double gamelist_last_x_open_time = -1000.0;

        if (settings_tab_open && !settings_tab_was_open) {
          settings_selected_section = 0;
          settings_focus_requested = true;
          settings_focus_right_requested = false;
          settings_focus_on_right = false;
        }
        if (paths_tab_open && !paths_tab_was_open) {
          paths_focus_requested = true;
        }
        if (about_tab_open && !about_tab_was_open) {
          about_focus_requested = true;
        }

        const char* header_text = "Select a Game";
        switch (active_frontend_page_) {
          case FrontendPage::kGameList:
            header_text = "Select a Game";
            break;
          case FrontendPage::kSettings:
            header_text = "Make some Tweaks";
            break;
          case FrontendPage::kPaths:
            header_text = "Choose your Path";
            break;
          case FrontendPage::kAbout:
            header_text = "Get the Info";
            break;
          default:
            header_text = "Select a Game";
            break;
        }

        auto parse_title_id_from_filename = [](const std::string& name) {
          if (name.size() < 8) {
            return std::string();
          }
          std::string candidate = name.substr(0, 8);
          for (char& c : candidate) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (!std::isxdigit(uc)) {
              return std::string();
            }
            c = static_cast<char>(std::toupper(uc));
          }
          return candidate;
        };

        auto escape_toml_string = [](const std::string& value) {
          std::string escaped;
          escaped.reserve(value.size() + 2);
          for (char c : value) {
            if (c == '\\' || c == '"') {
              escaped.push_back('\\');
            }
            escaped.push_back(c);
          }
          return escaped;
        };

        auto quote_toml_string =
            [escape_toml_string](const std::string& value) {
              return std::string("\"") + escape_toml_string(value) + "\"";
            };

        auto unquote_toml_string = [](const std::string& value) {
          if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
            return value;
          }
          std::string out;
          out.reserve(value.size());
          bool escaped = false;
          for (size_t i = 1; i + 1 < value.size(); ++i) {
            char c = value[i];
            if (escaped) {
              out.push_back(c);
              escaped = false;
              continue;
            }
            if (c == '\\') {
              escaped = true;
              continue;
            }
            out.push_back(c);
          }
          return out;
        };

        auto per_game_value_type_for_cvar = [](cvar::IConfigVar* config_var) {
          if (dynamic_cast<cvar::ConfigVar<bool>*>(config_var)) {
            return PerGameConfigValueType::kBool;
          }
          if (dynamic_cast<cvar::ConfigVar<int32_t>*>(config_var) ||
              dynamic_cast<cvar::ConfigVar<int>*>(config_var)) {
            return PerGameConfigValueType::kInt;
          }
          if (dynamic_cast<cvar::ConfigVar<uint32_t>*>(config_var)) {
            return PerGameConfigValueType::kUInt;
          }
          if (dynamic_cast<cvar::ConfigVar<uint64_t>*>(config_var)) {
            return PerGameConfigValueType::kUInt64;
          }
          if (dynamic_cast<cvar::ConfigVar<double>*>(config_var)) {
            return PerGameConfigValueType::kDouble;
          }
          if (dynamic_cast<cvar::ConfigVar<std::string>*>(config_var)) {
            return PerGameConfigValueType::kString;
          }
          if (dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(
                  config_var)) {
            return PerGameConfigValueType::kPath;
          }
          return PerGameConfigValueType::kUnknown;
        };

        auto assign_known_enum_values = [](PerGameConfigOption* option) {
          if (option->name == "postprocess_scaling_and_sharpening") {
            option->enum_values = {"", "bilinear", "cas", "fsr"};
          } else if (option->name == "postprocess_antialiasing") {
            option->enum_values = {"", "fxaa", "fxaa_extreme"};
          } else if (option->name == "render_target_path_d3d12") {
            option->enum_values = {"", "rov", "rtv"};
          } else if (option->name == "readback_resolve") {
            option->enum_values = {"none", "fast", "full"};
          } else if (option->name == "user_language") {
            option->enum_values = {"1", "2", "3", "4",  "5",  "6",
                                   "7", "8", "9", "10", "11", "12"};
          }
        };

        auto find_per_game_option =
            [this](const std::string& name) -> PerGameConfigOption* {
          auto it = std::find_if(per_game_config_options_.begin(),
                                 per_game_config_options_.end(),
                                 [&name](const PerGameConfigOption& option) {
                                   return option.name == name;
                                 });
          return it == per_game_config_options_.end() ? nullptr : &(*it);
        };

        auto build_per_game_config_options = [this,
                                              per_game_value_type_for_cvar,
                                              assign_known_enum_values]() {
          per_game_config_options_.clear();
          if (!cvar::ConfigVars) {
            return;
          }

          for (const auto& item : *cvar::ConfigVars) {
            auto* config_var = item.second;
            // Tiled resource options are not offered per game (they crash the
            // Xbox driver).
            if (config_var->name() == "d3d12_enable_tiled_shared_memory" ||
                config_var->name() == "d3d12_tiled_scaled_resolve") {
              continue;
            }
            PerGameConfigOption option;
            option.name = config_var->name();
            option.category = config_var->category();
            option.description = config_var->description();
            option.default_value = config_var->config_value();
            option.value_type = per_game_value_type_for_cvar(config_var);
            assign_known_enum_values(&option);
            per_game_config_options_.push_back(std::move(option));
          }

          std::sort(
              per_game_config_options_.begin(), per_game_config_options_.end(),
              [](const PerGameConfigOption& a, const PerGameConfigOption& b) {
                if (a.category != b.category) {
                  return a.category < b.category;
                }
                return a.name < b.name;
              });
        };

        auto ensure_plugin_layout_for_title = [quote_toml_string](
                                                  const std::filesystem::path&
                                                      title_plugin_root,
                                                  const std::string& title_id,
                                                  const std::string& title_name,
                                                  std::string* out_error) {
          auto set_error = [out_error](const std::string& message) {
            if (out_error) {
              *out_error = message;
            }
            return false;
          };

          std::error_code ec;
          std::filesystem::create_directories(title_plugin_root, ec);
          if (ec) {
            return set_error("Could not create plugin install directory.");
          }

          const auto plugins_toml_path = title_plugin_root / "plugins.toml";
          if (std::filesystem::exists(plugins_toml_path)) {
            return true;
          }

          std::filesystem::path first_toml_path;
          std::vector<std::filesystem::path> plugin_xex_files;
          std::set<std::string> inferred_hashes;

          auto infer_hashes_from_text =
              [&inferred_hashes](const std::string& text) {
                size_t i = 0;
                while (i < text.size()) {
                  while (i < text.size() &&
                         !std::isxdigit(static_cast<unsigned char>(text[i]))) {
                    ++i;
                  }
                  size_t start = i;
                  while (i < text.size() &&
                         std::isxdigit(static_cast<unsigned char>(text[i]))) {
                    ++i;
                  }
                  if (i - start == 16) {
                    std::string hash = text.substr(start, 16);
                    std::transform(hash.begin(), hash.end(), hash.begin(),
                                   [](unsigned char c) {
                                     return static_cast<char>(std::toupper(c));
                                   });
                    inferred_hashes.insert(hash);
                  }
                }
              };

          for (const auto& entry :
               std::filesystem::directory_iterator(title_plugin_root, ec)) {
            if (ec) {
              break;
            }
            if (!entry.is_regular_file()) {
              continue;
            }

            std::filesystem::path file_path = entry.path();
            std::string ext = file_path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) {
                             return static_cast<char>(std::tolower(c));
                           });

            if (ext == ".toml") {
              if (first_toml_path.empty()) {
                first_toml_path = file_path;
              }
              continue;
            }
            if (ext == ".xex") {
              plugin_xex_files.push_back(file_path);
              infer_hashes_from_text(file_path.stem().string());
              infer_hashes_from_text(file_path.filename().string());
            }
          }

          if (!first_toml_path.empty()) {
            std::filesystem::rename(first_toml_path, plugins_toml_path, ec);
            if (ec) {
              ec.clear();
              std::filesystem::copy_file(
                  first_toml_path, plugins_toml_path,
                  std::filesystem::copy_options::overwrite_existing, ec);
              if (ec) {
                return set_error(
                    "Downloaded plugin config could not be written as "
                    "plugins.toml.");
              }
              ec.clear();
              std::filesystem::remove(first_toml_path, ec);
            }
            return true;
          }

          if (plugin_xex_files.empty()) {
            return set_error(
                "Downloaded plugin files are missing plugin .xex payloads.");
          }

          if (inferred_hashes.empty()) {
            return set_error(
                "Downloaded plugin files did not include a plugins.toml and no "
                "16-digit module hash could be inferred from filenames.");
          }

          std::vector<std::string> hashes(inferred_hashes.begin(),
                                          inferred_hashes.end());

          std::ofstream toml_file(plugins_toml_path, std::ios::binary);
          if (!toml_file.is_open()) {
            return set_error("Failed to create plugins.toml.");
          }

          const std::string normalized_title_name =
              title_name.empty() ? title_id : title_name;

          toml_file << "title_name = "
                    << quote_toml_string(normalized_title_name) << "\n";
          toml_file << "title_id = " << quote_toml_string(title_id) << "\n\n";

          for (const auto& plugin_path : plugin_xex_files) {
            toml_file << "[[plugin]]\n";
            toml_file << "name = "
                      << quote_toml_string(plugin_path.stem().string()) << "\n";
            toml_file << "file = "
                      << quote_toml_string(plugin_path.filename().string())
                      << "\n";
            toml_file << "desc = "
                      << quote_toml_string("Auto-generated plugin entry")
                      << "\n";
            toml_file << "is_enabled = true\n";
            if (hashes.size() == 1) {
              toml_file << "hash = " << quote_toml_string(hashes.front())
                        << "\n\n";
            } else {
              toml_file << "hash = [";
              for (size_t i = 0; i < hashes.size(); ++i) {
                if (i) {
                  toml_file << ", ";
                }
                toml_file << quote_toml_string(hashes[i]);
              }
              toml_file << "]\n\n";
            }
          }

          toml_file.flush();
          return true;
        };

        auto has_existing_patch_for_title =
            [](const std::filesystem::path& patches_dir,
               const std::string& title_id) {
              if (title_id.empty() || !std::filesystem::exists(patches_dir)) {
                return false;
              }

              std::string normalized_title_id = title_id;
              std::transform(normalized_title_id.begin(),
                             normalized_title_id.end(),
                             normalized_title_id.begin(), [](unsigned char c) {
                               return static_cast<char>(std::toupper(c));
                             });

              for (const auto& entry :
                   std::filesystem::directory_iterator(patches_dir)) {
                if (!entry.is_regular_file()) {
                  continue;
                }
                std::string name = entry.path().filename().string();
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) {
                                 return static_cast<char>(std::toupper(c));
                               });
                if (name.rfind(normalized_title_id, 0) == 0) {
                  return true;
                }
              }
              return false;
            };

        auto open_manual_prompt =
            [this](const std::filesystem::path& dest, ManualInstallKind kind,
                   const std::string& title_id, const std::string& message) {
              action_status_ = message;
              switch (kind) {
                case ManualInstallKind::kPatch:
                  action_popup_mode_ = ActionPopupMode::kManualPatchPrompt;
                  break;
                case ManualInstallKind::kPlugin:
                  action_popup_mode_ = ActionPopupMode::kManualPluginPrompt;
                  break;
                case ManualInstallKind::kConfig:
                  action_popup_mode_ = ActionPopupMode::kManualConfigPrompt;
                  break;
                default:
                  action_popup_mode_ = ActionPopupMode::kInfo;
                  break;
              }
              pending_manual_kind_ = kind;
              pending_manual_destination_ = dest;
              pending_manual_title_id_ = title_id;
              action_popup_should_close_ = false;
              show_action_status_ = true;
            };

        auto apply_per_game_config = [this](const std::string& title_id,
                                            const std::filesystem::path& path) {
          if (title_id.empty()) {
            return false;
          }
          if (!std::filesystem::exists(path)) {
            return false;
          }

          try {
            ParseFile(path);
          } catch (const std::exception& e) {
            return false;
          } catch (...) {
            return false;
          }

          config::LoadGameConfig(title_id);
          emulator_window_.ApplyDisplayConfigForCvars();
          return true;
        };

        auto begin_patch_download =
            [this, open_manual_prompt](
                const std::string& title_id,
                const std::filesystem::path& patches_root) {
              std::weak_ptr<int> lifetime = async_lifetime_;
              auto* context = &emulator_window_.app_context();
              UWP::DownloadPatchesForGame(
                  title_id, xe::path_to_utf8(patches_root),
                  [this, lifetime, context, open_manual_prompt, title_id, patches_root](
                      bool ok, std::string msg) {
                    context->CallInUIThreadDeferred(
                        [this, lifetime, open_manual_prompt, title_id, patches_root,
                         ok, msg = std::move(msg)]() {
                          if (lifetime.expired()) return;
                          if (ok) {
                            OpenPatchEditor(patches_root, title_id);
                            action_popup_should_close_ = true;
                            show_action_status_ = false;
                          } else {
                            open_manual_prompt(
                                patches_root, ManualInstallKind::kPatch, title_id,
                                (msg.empty() ? "No matching patches were found online." : msg) +
                                    "\nSelect a patch file manually?");
                          }
                        });
                  });
            };

        auto begin_optimized_settings_download = [this, open_manual_prompt,
                                                  apply_per_game_config](
                                                     const std::string&
                                                         title_id,
                                                     const std::filesystem::
                                                         path& config_root) {
          std::weak_ptr<int> lifetime = async_lifetime_;
          auto* context = &emulator_window_.app_context();
          // The download completes on a background thread; apply the config
          // and update the frontend on the UI thread.
          UWP::DownloadConfigForGame(
              title_id, config_root.string(),
              [this, lifetime, context, open_manual_prompt,
               apply_per_game_config, title_id,
               config_root](bool ok, std::string msg) {
                context->CallInUIThreadDeferred([this, lifetime,
                                                 open_manual_prompt,
                                                 apply_per_game_config,
                                                 title_id, config_root, ok,
                                                 msg = std::move(msg)]() {
                  if (lifetime.expired()) return;
                  const auto config_path =
                      config_root / fmt::format("{}.config.toml", title_id);
                  if (ok) {
                    bool applied = apply_per_game_config(title_id, config_path);
                    if (msg == "already_installed") {
                      action_status_ =
                          applied ? fmt::format(
                                        "Optimized settings already installed "
                                        "and re-applied for {}.",
                                        title_id)
                                  : fmt::format(
                                        "Optimized settings already installed "
                                        "for {}, but apply failed.",
                                        title_id);
                    } else {
                      action_status_ = applied
                                           ? fmt::format(
                                                 "Downloaded optimized settings "
                                                 "and applied config for {}.",
                                                 title_id)
                                           : fmt::format(
                                                 "Downloaded optimized settings "
                                                 "for {}, but apply failed.",
                                                 title_id);
                    }
                    action_popup_mode_ = ActionPopupMode::kInfo;
                    pending_manual_kind_ = ManualInstallKind::kConfig;
                    pending_manual_destination_ = config_root;
                    pending_manual_title_id_ = title_id;
                    action_popup_should_close_ = true;
                    show_action_status_ = true;
                  } else {
                    if (msg == "not_found") {
                      action_status_ =
                          "No optimized settings were found online for this "
                          "title.";
                      action_popup_mode_ = ActionPopupMode::kInfo;
                      show_action_status_ = true;
                    } else {
                      open_manual_prompt(
                          config_root, ManualInstallKind::kConfig, title_id,
                          "Failed to download optimized settings automatically.\n"
                          "Do you want to select a config manually?");
                    }
                  }
                });
              });
        };

        auto canonicalize_per_game_value =
            [quote_toml_string, unquote_toml_string](
                const PerGameConfigOption& option,
                const std::string& raw_value) {
              std::string value = raw_value;
              if (option.value_type == PerGameConfigValueType::kBool) {
                std::string lowered = value;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                               [](unsigned char c) {
                                 return static_cast<char>(std::tolower(c));
                               });
                if (lowered == "1") {
                  lowered = "true";
                }
                if (lowered == "0") {
                  lowered = "false";
                }
                if (lowered != "true" && lowered != "false") {
                  lowered = "false";
                }
                return lowered;
              }
              if (option.value_type == PerGameConfigValueType::kString ||
                  option.value_type == PerGameConfigValueType::kPath) {
                return quote_toml_string(unquote_toml_string(value));
              }
              return value;
            };

        auto default_per_game_value =
            [canonicalize_per_game_value](const PerGameConfigOption& option) {
              std::string value = option.default_value;
              if (value.empty()) {
                switch (option.value_type) {
                  case PerGameConfigValueType::kBool:
                    value = "false";
                    break;
                  case PerGameConfigValueType::kInt:
                  case PerGameConfigValueType::kUInt:
                  case PerGameConfigValueType::kUInt64:
                    value = "0";
                    break;
                  case PerGameConfigValueType::kDouble:
                    value = "0.0";
                    break;
                  case PerGameConfigValueType::kString:
                  case PerGameConfigValueType::kPath:
                    value = "\"\"";
                    break;
                  default:
                    value = "\"\"";
                    break;
                }
              }
              return canonicalize_per_game_value(option, value);
            };

        auto add_or_update_per_game_entry = [this, find_per_game_option,
                                             default_per_game_value,
                                             canonicalize_per_game_value](
                                                const std::string& option_name,
                                                const std::string& raw_value) {
          auto* option = find_per_game_option(option_name);
          std::string category = "General";
          std::string value = raw_value;
          if (option) {
            category = option->category.empty() ? "General" : option->category;
            if (value.empty()) {
              value = default_per_game_value(*option);
            } else {
              value = canonicalize_per_game_value(*option, value);
            }
          }

          auto existing_it = per_game_config_entries_.find(option_name);
          if (existing_it != per_game_config_entries_.end() &&
              category == "General" && !existing_it->second.category.empty()) {
            category = existing_it->second.category;
          }

          per_game_config_entries_[option_name] = {category, value};
        };

        auto build_per_game_config_text = [this]() {
          std::map<std::string,
                   std::vector<std::pair<std::string, std::string>>>
              grouped;
          for (const auto& entry_pair : per_game_config_entries_) {
            const auto& option_name = entry_pair.first;
            const auto& entry = entry_pair.second;
            std::string category =
                entry.category.empty() ? "General" : entry.category;
            grouped[category].push_back({option_name, entry.value});
          }

          std::string out =
              "# Per-game config overrides\n"
              "# Generated by Game Menu editor.\n\n";
          for (auto& category_pair : grouped) {
            auto& entries = category_pair.second;
            std::sort(
                entries.begin(), entries.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            out += fmt::format("[{}]\n", category_pair.first);
            for (const auto& entry : entries) {
              out += fmt::format("{} = {}\n", entry.first, entry.second);
            }
            out += "\n";
          }
          return out;
        };

        auto open_per_game_config_editor =
            [this, build_per_game_config_options, find_per_game_option,
             quote_toml_string](const std::string& title_id,
                                const std::filesystem::path& path,
                                const std::string& initial_contents,
                                const std::string& title_display) {
              pending_per_game_config_title_id_ = title_id;
              pending_per_game_config_path_ = path;
              pending_per_game_config_title_display_ =
                  title_display.empty() ? title_id : title_display;
              selected_per_game_config_option_.clear();
              per_game_config_remove_mode_ = false;
              per_game_config_focus_jump_request_ =
                  PerGameConfigFocusSide::kNone;
              per_game_config_last_focused_control_id_.clear();
              per_game_config_entries_.clear();

              build_per_game_config_options();

              if (std::filesystem::exists(path)) {
                try {
                  auto table = ParseFile(path);
                  for (const auto& category_node : table) {
                    const auto category_table = category_node.second.as_table();
                    if (!category_table) {
                      continue;
                    }

                    const std::string category(category_node.first);
                    for (const auto& option_node : *category_table) {
                      const std::string option_name(option_node.first);
                      const auto& value_node = option_node.second;
                      if (option_name == "d3d12_enable_tiled_shared_memory" ||
                          option_name == "d3d12_tiled_scaled_resolve") {
                        continue;
                      }
                      std::string value_string;

                      if (auto bool_value = value_node.as_boolean()) {
                        value_string = bool_value->get() ? "true" : "false";
                      } else if (auto int_value = value_node.as_integer()) {
                        value_string = fmt::format("{}", int_value->get());
                      } else if (auto double_value =
                                     value_node.as_floating_point()) {
                        value_string = fmt::format("{}", double_value->get());
                      } else if (auto string_value = value_node.as_string()) {
                        value_string = quote_toml_string(string_value->get());
                      } else {
                        continue;
                      }

                      per_game_config_entries_[option_name] = {category,
                                                               value_string};
                    }
                  }
                } catch (const std::exception& e) {
                  (void)e;
                } catch (...) {
                }
              }

              for (const auto& entry : per_game_config_entries_) {
                if (find_per_game_option(entry.first)) {
                  continue;
                }
                PerGameConfigOption unknown_option;
                unknown_option.name = entry.first;
                unknown_option.category = entry.second.category;
                unknown_option.description =
                    "Unknown or legacy option loaded from existing file.";
                unknown_option.default_value = entry.second.value;
                unknown_option.value_type = PerGameConfigValueType::kUnknown;
                per_game_config_options_.push_back(std::move(unknown_option));
              }

              std::sort(per_game_config_options_.begin(),
                        per_game_config_options_.end(),
                        [](const PerGameConfigOption& a,
                           const PerGameConfigOption& b) {
                          if (a.category != b.category) {
                            return a.category < b.category;
                          }
                          return a.name < b.name;
                        });

              if (!per_game_config_options_.empty()) {
                per_game_config_add_candidate_ =
                    per_game_config_options_.front().name;
              } else {
                per_game_config_add_candidate_.clear();
              }

              if (!per_game_config_entries_.empty()) {
                selected_per_game_config_option_ =
                    per_game_config_entries_.begin()->first;
              }

              std::memset(per_game_config_buffer_, 0,
                          sizeof(per_game_config_buffer_));
              std::strncpy(per_game_config_buffer_, initial_contents.c_str(),
                           sizeof(per_game_config_buffer_) - 1);
              per_game_config_popup_focus_requested_ = true;
              show_per_game_config_editor_ = true;
            };

        if (game_list_tab_open) {
          const bool controller_x_down = key_down(ImGuiKey_GamepadFaceLeft);
          const bool controller_b_down = key_down(ImGuiKey_GamepadFaceRight);
          const bool controller_y_down = key_down(ImGuiKey_GamepadFaceUp);
          const bool controller_x_pressed =
              controller_x_down && !gamelist_x_was_down;
          const bool controller_x_debounced =
              controller_x_pressed &&
              (ImGui::GetTime() - gamelist_last_x_open_time) > 0.20;
          const bool controller_b_pressed =
              controller_b_down && !gamelist_b_was_down;
          const bool controller_y_pressed =
              controller_y_down && !search_gamepad_y_was_down_;

          if (!game_list_tab_was_open) {
            gamelist_focus_requested = true;
            gamelist_focus_selected_request = true;
            gamelist_window_focus_requested = true;
          }

          // Additional focus restoration if coming from another tab
          if (game_list_tab_open &&
              (settings_tab_was_open || paths_tab_was_open ||
               about_tab_was_open)) {
            gamelist_focus_requested = true;
            gamelist_focus_selected_request = true;
            gamelist_window_focus_requested = true;
          }

          const bool is_any_popup_open =
              ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                             ImGuiPopupFlags_AnyPopupLevel);
          const bool is_any_game_context_open = is_any_popup_open ||
                                                show_game_context_menu_ ||
                                                show_per_game_config_editor_;

          if (controller_b_pressed && !is_any_game_context_open) {
            ImGui::SetWindowFocus("##gamelist_left");
            gamelist_focus_selected_request = true;
          }

          if (controller_y_pressed && !is_any_game_context_open) {
            show_search_panel_ = !show_search_panel_;
            if (show_search_panel_) {
              if (search_selected_game_path_.empty()) {
                search_selected_game_path_ = selected_game_path_;
              }
              search_panel_focus_input_requested_ = true;
              search_panel_list_focus_requested_ = false;
              UWP::ShowKeyboard();
            } else {
              search_panel_focus_input_requested_ = false;
              search_panel_list_focus_requested_ = false;
              gamelist_window_focus_requested = true;
              gamelist_focus_selected_request = true;
            }
          }
          search_gamepad_y_was_down_ = controller_y_down;

          struct GameListRow {
            std::string path;
            std::string filename;
            std::string title_id;
            std::string disambiguation;
            UWP::UnityGameMetadata metadata;
            bool has_metadata = false;
            bool cache_checked = false;
            std::string list_art_path;
            std::string detail_art_path;
            std::string dynamic_art_path;
            std::string video_path;
            screenscraper::Metadata editable_metadata;
          };

          std::vector<GameListRow> game_rows;
          game_rows.reserve(UWP::GetGames().size());
          for (const auto& set : UWP::GetGames()) {
            GameListRow row;
            std::tie(row.path, row.filename) = set;

            auto cached_title_id = cached_game_title_ids_.find(row.path);
            if (cached_title_id == cached_game_title_ids_.end()) {
              std::string title_id = UWP::GetTitleIdFromPath(row.path);
              cached_game_title_ids_.insert({row.path, title_id});
              row.title_id = std::move(title_id);
            } else {
              row.title_id = cached_title_id->second;
            }

            const auto* cached = scraper.Cached(
                MakeScreenScraperTarget(std::filesystem::u8path(row.path),
                                        row.filename),
                row.path == selected_game_path_, &row.cache_checked);
            bool needs_metadata_fallback = !cached;
            if (cached && !cached->local_override) {
              const auto& source = cached->metadata;
              needs_metadata_fallback =
                  source.title.empty() || source.description.empty() ||
                  source.publisher.empty() ||
                  (cvars::ui_show_genre_metadata && source.genre.empty()) ||
                  (cvars::ui_show_developer_metadata &&
                   source.developer.empty()) ||
                  (cvars::ui_show_release_date_metadata &&
                   source.release_date.empty()) ||
                  (cvars::ui_show_user_rating_metadata && source.rating <= 0);
            }
            if (!row.title_id.empty()) {
              if (row.cache_checked && needs_metadata_fallback) {
                UWP::EnsureUnityMetadataFetch(row.title_id);
              }
              row.has_metadata =
                  UWP::TryGetUnityMetadata(row.title_id, &row.metadata);
            }

            if (cached) {
              const auto& source = cached->metadata;
              row.editable_metadata = source;
              auto& metadata = row.metadata;
              if (cached->local_override || !source.title.empty()) {
                metadata.display_name = source.title;
              }
              if (cached->local_override || !source.description.empty()) {
                metadata.description_full = source.description;
                metadata.description_short = source.description;
              }
              if (cached->local_override || !source.publisher.empty()) {
                metadata.publisher = source.publisher;
              }
              if (cached->local_override || !source.developer.empty()) {
                metadata.developer = source.developer;
              }
              if (cached->local_override || !source.genre.empty()) {
                metadata.genre.clear();
                if (!source.genre.empty()) {
                  metadata.genre.push_back(source.genre);
                }
              }
              if (cached->local_override || !source.release_date.empty()) {
                metadata.release_date = source.release_date;
              }
              if (cached->local_override || source.rating > 0) {
                metadata.user_rating =
                    source.rating > 0
                        ? fmt::format("{:.1f}/5", source.rating * 5.0)
                        : std::string();
              }
              row.list_art_path = scraper.ArtPath(*cached, false);
              row.detail_art_path = scraper.ArtPath(*cached, true);
              row.dynamic_art_path = scraper.BackgroundPath(*cached);
              row.video_path = scraper.VideoPath(*cached);
              row.has_metadata = true;
            }

            auto& editable = row.editable_metadata;
            editable.title = row.metadata.display_name.empty()
                                 ? row.filename : row.metadata.display_name;
            editable.description = row.metadata.description_full.empty()
                                       ? row.metadata.description_short
                                       : row.metadata.description_full;
            editable.developer = row.metadata.developer;
            editable.publisher = row.metadata.publisher;
            editable.release_date = row.metadata.release_date;
            editable.genre.clear();
            for (const auto& genre : row.metadata.genre) {
              if (!editable.genre.empty()) editable.genre += ", ";
              editable.genre += genre;
            }
            if (!cached || (!cached->local_override && editable.rating <= 0)) {
              try {
                editable.rating = std::clamp(
                    std::stod(row.metadata.user_rating) / 5.0, 0.0, 1.0);
              } catch (...) {}
            }

            game_rows.push_back(std::move(row));
          }

          auto get_game_display_title = [](const GameListRow& row) {
            if (row.has_metadata && !row.metadata.display_name.empty()) {
              return row.metadata.display_name;
            }
            return row.filename;
          };

          std::map<std::string, std::vector<GameListRow*>> title_groups;
          for (auto& row : game_rows) {
            title_groups[xe::utf8::lower_ascii(get_game_display_title(row))]
                .push_back(&row);
          }
          for (auto& group : title_groups) {
            auto& rows = group.second;
            if (rows.size() < 2) {
              continue;
            }
            std::map<std::string, size_t> scan_label_counts;
            for (const auto* row : rows) {
              ++scan_label_counts[xe::utf8::lower_ascii(row->filename)];
            }
            std::map<std::string, size_t> subtitle_counts;
            for (auto* row : rows) {
              if (!row->filename.empty() &&
                  scan_label_counts[xe::utf8::lower_ascii(row->filename)] == 1) {
                row->disambiguation = row->filename;
              } else {
                const auto target = MakeScreenScraperTarget(
                    std::filesystem::u8path(row->path), row->filename);
                row->disambiguation = target.relative_game_path.empty()
                                          ? row->path
                                          : xe::path_to_utf8(
                                                target.relative_game_path);
              }
              ++subtitle_counts[xe::utf8::lower_ascii(row->disambiguation)];
            }
            for (auto* row : rows) {
              // Different game roots can contain the same relative filename.
              if (subtitle_counts[xe::utf8::lower_ascii(row->disambiguation)] >
                  1) {
                row->disambiguation = row->path;
              }
            }
          }

          auto get_game_subtitle = [](const GameListRow& row,
                                      const std::string& display_title) {
            if (!row.disambiguation.empty()) {
              return row.disambiguation;
            }
            if (row.has_metadata && !row.metadata.publisher.empty()) {
              return row.metadata.publisher;
            }
            std::string fallback =
                row.title_id.empty() ? row.filename : row.title_id;
            if (fallback == display_title) {
              fallback = row.filename;
            }
            return fallback;
          };

          auto load_game_art =
              [&](const GameListRow& row, const std::string& preferred_path,
                  std::initializer_list<std::string_view> fallback_paths) {
                auto texture = GetOrCreateImageTexture(preferred_path);
                if (!texture) {
                  if (row.cache_checked && !row.title_id.empty()) {
                    UWP::EnsureUnityMetadataFetch(row.title_id);
                  }
                  for (const auto path : fallback_paths) {
                    if (path.empty()) {
                      continue;
                    }
                    texture = GetOrCreateImageTexture(std::string(path));
                    if (texture) {
                      break;
                    }
                  }
                }
                return texture;
              };

          auto normalize_search_text = [](const std::string& value) {
            return xe::utf8::lower_ascii(value);
          };

          auto build_top_loop_order = [](size_t item_count,
                                         size_t selected_index) {
            std::vector<size_t> ordered_indices;
            ordered_indices.reserve(item_count);
            if (!item_count) {
              return ordered_indices;
            }
            const size_t normalized_selected =
                item_count ? (selected_index % item_count) : 0;
            for (size_t i = 0; i < item_count; ++i) {
              ordered_indices.push_back((normalized_selected + i) % item_count);
            }
            return ordered_indices;
          };

          const GameListRow* selected_row = nullptr;
          size_t selected_game_row_index = 0;
          for (const auto& row : game_rows) {
            if (row.path == selected_game_path_) {
              selected_row = &row;
              break;
            }
            ++selected_game_row_index;
          }
          auto focus_game_row_by_index = [&](size_t new_index,
                                             bool request_focus) {
            if (game_rows.empty()) {
              return;
            }
            new_index %= game_rows.size();
            selected_game_row_index = new_index;
            selected_row = &game_rows[new_index];
            selected_game_path_ = selected_row->path;
            selected_game_name_ = selected_row->filename;
            if (request_focus) {
              gamelist_focus_selected_request = true;
              gamelist_focus_requested = true;
              gamelist_window_focus_requested = true;
            }
          };

          if (!selected_row && !game_rows.empty()) {
            focus_game_row_by_index(0, false);
          }

          ImVec2 main_page_min = ImGui::GetWindowPos();
          ImVec2 main_page_max =
              ImVec2(main_page_min.x + ImGui::GetWindowSize().x,
                     main_page_min.y + ImGui::GetWindowSize().y);
          if (selected_row && scraper.BackgroundEnabled()) {
            auto bg_tex =
                cvars::ui_dynamic_background_type == "fanart"
                    ? load_game_art(*selected_row, selected_row->dynamic_art_path,
                                    {selected_row->metadata.background_path})
                    : GetOrCreateImageTexture(selected_row->dynamic_art_path);
            if (!selected_row->video_path.empty()) {
              if (!background_video_) {
                background_video_ = std::make_unique<FrontendVideoPlayer>();
              }
              if (background_video_path_ != selected_row->video_path) {
                imgui_drawer()->RetainTextureForFrame(
                    background_video_texture_);
                background_video_texture_.reset();
                background_video_path_ = selected_row->video_path;
              }
              background_video_->SetPath(
                  std::filesystem::u8path(selected_row->video_path),
                  scraper.revision());
              background_video_error_ = background_video_->GetError();
              if (auto frame = background_video_->TakeFrame()) {
                if (emulator_window_.immediate_drawer_ && frame->width &&
                    frame->height &&
                    frame->rgba.size() ==
                        size_t(frame->width) * frame->height * 4) {
                  imgui_drawer()->RetainTextureForFrame(
                      background_video_texture_);
                  background_video_texture_ =
                      emulator_window_.immediate_drawer_->CreateTexture(
                          frame->width, frame->height,
                          ui::ImmediateTextureFilter::kLinear, false,
                          frame->rgba.data());
                }
              }
              if (background_video_texture_) {
                bg_tex = background_video_texture_;
              }
            } else {
              StopVideoBackground();
            }
            if (bg_tex) {
              imgui_drawer()->RetainTextureForFrame(bg_tex);
              frontend_splitter.SetCurrentChannel(frontend_draw_list, 1);
              frontend_draw_list->PushClipRect(main_page_min, main_page_max,
                                               true);
              frontend_draw_list->AddImage(
                  reinterpret_cast<ImTextureID>(bg_tex.get()), main_page_min,
                  main_page_max);
              frontend_draw_list->PopClipRect();
              frontend_splitter.SetCurrentChannel(frontend_draw_list, 3);
            }
          } else {
            StopVideoBackground();
          }

          ImGui::Dummy(ImVec2(0.0f, 42.0f * display_scale));

          bool request_open_game_context_menu = false;
          bool request_launch_selected_game = false;
          std::string launch_game_path;
          const float gamelist_bottom_lift = 25.0f * display_scale;
          float content_height =
              ImGui::GetContentRegionAvail().y - gamelist_bottom_lift;
          if (content_height < 120.0f) {
            content_height = 120.0f;
          }

          const float avail_width = ImGui::GetContentRegionAvail().x;
          float left_panel_width = avail_width * 0.584f;

          if (ImGui::BeginChild("##gamelist_left",
                                ImVec2(left_panel_width, content_height), false,
                                ImGuiWindowFlags_NoScrollbar)) {
            if (gamelist_window_focus_requested && !show_search_panel_ &&
                !search_panel_focus_input_requested_ &&
                !search_panel_list_focus_requested_) {
              ImGui::SetWindowFocus("##gamelist_left");
              gamelist_window_focus_requested = false;
            }

            const float carousel_outer_padding = 14.0f * display_scale;
            const float carousel_spacing = 16.0f * display_scale;
            const float carousel_card_height = 126.0f * display_scale;
            const float carousel_horizontal_offset = 30.0f * display_scale;
            const std::vector<size_t> ordered_game_row_indices =
                build_top_loop_order(game_rows.size(), selected_game_row_index);
            // Apply navigation queued during this draw on the next frame.
            const bool focus_selected_row_now =
                gamelist_focus_requested || gamelist_focus_selected_request;
            const std::string focus_selected_row_path = selected_game_path_;
            ImGui::Dummy(ImVec2(0.0f, 10.0f * display_scale));

            for (size_t ordered_index = 0;
                 ordered_index < ordered_game_row_indices.size();
                 ++ordered_index) {
              const auto& row =
                  game_rows[ordered_game_row_indices[ordered_index]];
              ImGui::PushID(row.path.c_str());
              const bool is_selected = selected_game_path_ == row.path;
              const float row_height = carousel_card_height;
              const float side_inset = carousel_outer_padding;
              const float card_width =
                  std::max(160.0f * display_scale,
                           ImGui::GetContentRegionAvail().x -
                               side_inset * 2.0f - carousel_horizontal_offset);
              const float row_start_x = ImGui::GetCursorPosX() + side_inset +
                                        carousel_horizontal_offset;
              const bool focus_this_row = frontend_accepts_input &&
                  !is_any_game_context_open &&
                  is_selected &&
                  ((focus_selected_row_now &&
                    row.path == focus_selected_row_path) ||
                   (!ImGui::IsAnyItemFocused() &&
                    ImGui::IsWindowFocused(
                        ImGuiFocusedFlags_RootAndChildWindows)));
              const ImGuiSelectableFlags row_selectable_flags =
                  is_any_game_context_open ? ImGuiSelectableFlags_Disabled : 0;
              ImGui::SetCursorPosX(row_start_x);
              ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
              ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
              ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
              ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
              if (ImGui::Selectable("##game_row", is_selected,
                                    row_selectable_flags,
                                    ImVec2(card_width, row_height))) {
                selected_game_path_ = row.path;
                selected_game_name_ = row.filename;
                gamelist_focus_selected_request = true;
              }
              ImGui::PopStyleColor(4);
              if (focus_this_row && xe::ui::FocusFrontendSelection()) {
                gamelist_focus_requested = false;
                gamelist_focus_selected_request = false;
              }
              const bool row_focused = ImGui::IsItemFocused();
              if (!is_any_game_context_open && row_focused && !is_selected) {
                selected_game_path_ = row.path;
                selected_game_name_ = row.filename;
              }
              if (!is_any_game_context_open && row_focused &&
                  !remote_selection_this_frame) {
                const bool nav_up =
                    key_pressed(ImGuiKey_GamepadDpadUp, false) ||
                    key_pressed(ImGuiKey_UpArrow, false);
                const bool nav_down =
                    key_pressed(ImGuiKey_GamepadDpadDown, false) ||
                    key_pressed(ImGuiKey_DownArrow, false);
                if (nav_up && game_rows.size() > 1) {
                  size_t new_index =
                      (selected_game_row_index + game_rows.size() - 1) %
                      game_rows.size();
                  focus_game_row_by_index(new_index, true);
                } else if (nav_down && game_rows.size() > 1) {
                  focus_game_row_by_index(selected_game_row_index + 1, true);
                }
              }
              if (!is_any_game_context_open && is_selected) {
                ImGui::SetItemDefaultFocus();
              }

              const ImVec2 item_min = ImGui::GetItemRectMin();
              const ImVec2 item_max = ImGui::GetItemRectMax();
              const float pad = 10.0f * display_scale;
              const float art_height = carousel_card_height - pad * 2.0f;
              auto gamelist_tex = load_game_art(
                  row, row.list_art_path,
                  {row.metadata.cover_path, row.metadata.icon_path});
              const float art_width =
                  gamelist_tex && gamelist_tex->height
                      ? art_height * float(gamelist_tex->width) /
                            float(gamelist_tex->height)
                      : art_height;
              const float image_x = item_min.x + pad;
              const float image_y =
                  item_min.y + std::max(0.0f, (row_height - art_height) * 0.5f);
              const float text_area_min_x = image_x + art_width + pad * 2.2f;
              const float text_area_max_x = item_max.x - pad;
              const float text_area_width =
                  std::max(0.0f, text_area_max_x - text_area_min_x);

              if (gamelist_tex) {
                ImGui::SetCursorScreenPos(ImVec2(image_x, image_y));
                ImGui::Image(reinterpret_cast<ImTextureID>(gamelist_tex.get()),
                             ImVec2(art_width, art_height));
              }

              const std::string display_title = get_game_display_title(row);
              const ImVec2 title_size =
                  ImGui::CalcTextSize(display_title.c_str());
              const std::string secondary_text =
                  get_game_subtitle(row, display_title);
              const ImVec2 secondary_size =
                  secondary_text.empty()
                      ? ImVec2(0.0f, 0.0f)
                      : ImGui::CalcTextSize(secondary_text.c_str());
              const float text_line_spacing = 10.0f * display_scale;
              const float combined_text_height =
                  title_size.y + (secondary_text.empty()
                                      ? 0.0f
                                      : (text_line_spacing + secondary_size.y));
              const float title_y =
                  item_min.y +
                  std::max(0.0f, (row_height - combined_text_height) * 0.5f);
              const float title_x = text_area_min_x;
              DrawTextWithConfiguredEffect(
                  ImGui::GetWindowDrawList(), ImGui::GetFont(),
                  ImGui::GetFontSize(), ImVec2(title_x, title_y),
                  IM_COL32(228, 228, 228, 255), display_title.c_str());

              if (!secondary_text.empty()) {
                const float secondary_x = text_area_min_x;
                DrawTextWithConfiguredEffect(
                    ImGui::GetWindowDrawList(), ImGui::GetFont(),
                    ImGui::GetFontSize(),
                    ImVec2(secondary_x,
                           title_y + title_size.y + text_line_spacing),
                    IM_COL32(153, 153, 153, 255), secondary_text.c_str());
              }

              if (!is_any_game_context_open && row_focused &&
                  selected_game_path_ == row.path &&
                  !remote_selection_this_frame &&
                  key_pressed(ImGuiKey_GamepadFaceDown, false)) {
                request_launch_selected_game = true;
                launch_game_path = row.path;
              }

              if (!is_any_game_context_open && row_focused &&
                  key_pressed(ImGuiKey_F12, false)) {
                selected_game_path_ = row.path;
                selected_game_name_ = row.filename;
                request_open_game_context_menu = true;
              }
              ImGui::PopID();
              ImGui::Dummy(ImVec2(0.0f, carousel_spacing));
            }
          }
          ImGui::EndChild();

          if (!is_any_game_context_open && controller_x_debounced) {
            if (selected_row) {
              selected_game_path_ = selected_row->path;
              selected_game_name_ = selected_row->filename;
              request_open_game_context_menu = true;
            }
          }

          ImGui::SameLine();

          if (ImGui::BeginChild("##main_page", ImVec2(0, content_height), false,
                                ImGuiWindowFlags_NoScrollbar |
                                    ImGuiWindowFlags_NoScrollWithMouse |
                                    ImGuiWindowFlags_NoInputs)) {
            ImGui::SetScrollY(0);
            if (selected_row) {
              ImGui::Dummy(ImVec2(0.0f, 21.0f * display_scale));
              std::string detail_title = selected_row->filename;
              if (selected_row->has_metadata &&
                  !selected_row->metadata.display_name.empty()) {
                detail_title = selected_row->metadata.display_name;
              }

              auto hero_tex = load_game_art(
                  *selected_row, selected_row->detail_art_path,
                  {selected_row->metadata.banner_path,
                   selected_row->metadata.gallery_paths.empty()
                       ? std::string_view()
                       : selected_row->metadata.gallery_paths.front()});
              if (hero_tex) {
                const float avail_w =
                    ImGui::GetContentRegionAvail().x - 42.0f * display_scale;
                const float aspect_ratio = static_cast<float>(hero_tex->width) /
                                           static_cast<float>(hero_tex->height);
                const float uniform_h =
                    std::min(avail_w / aspect_ratio,
                             std::max(1.0f, content_height * 0.48f));
                ImGui::Image(reinterpret_cast<ImTextureID>(hero_tex.get()),
                             ImVec2(uniform_h * aspect_ratio, uniform_h),
                             ImVec2(0, 0), ImVec2(1, 1));
              }

              ImGui::BeginChild(
                  "##metadata_text",
                  ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x -
                                           42.0f * display_scale),
                         std::max(1.0f, ImGui::GetContentRegionAvail().y)),
                  ImGuiChildFlags_None,
                  ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse |
                      ImGuiWindowFlags_NoInputs);
              const float scroll_range = ImGui::GetScrollMaxY();
              const int frame = ImGui::GetFrameCount();
              if (metadata_scroll_path_ != selected_row->path ||
                  metadata_scroll_frame_ != frame - 1 ||
                  std::abs(metadata_scroll_range_ - scroll_range) > 0.5f ||
                  is_any_game_context_open || show_search_panel_) {
                metadata_scroll_elapsed_ = 0;
              } else {
                metadata_scroll_elapsed_ += io.DeltaTime;
              }
              metadata_scroll_path_ = selected_row->path;
              metadata_scroll_frame_ = frame;
              metadata_scroll_range_ = scroll_range;
              const double speed = ImGui::GetTextLineHeight() * 0.5;
              const double phase = std::fmod(
                  metadata_scroll_elapsed_, scroll_range / speed + 6.0);
              ImGui::SetScrollY(float(std::clamp(
                  (phase - 3.0) * speed, 0.0, double(scroll_range))));
              ImGui::PushTextWrapPos(0.0f);
              if (cvars::ui_show_title_metadata) {
                ImGui::TextWrapped("%s", detail_title.c_str());
              }
              if (!background_video_error_.empty()) {
                ImGui::TextWrapped("Video background: %s", background_video_error_.c_str());
              }

              const auto& metadata = selected_row->metadata;
              const std::string* detail_description =
                  !metadata.description_full.empty()
                      ? &metadata.description_full
                  : !metadata.description_short.empty()
                      ? &metadata.description_short
                      : nullptr;

              if (detail_description) {
                ImGui::Spacing();
                ImGui::TextWrapped("%s", detail_description->c_str());
              }

              if (selected_row->has_metadata) {
                if (!selected_row->metadata.publisher.empty()) {
                  ImGui::Spacing();
                  ImGui::TextDisabled("Publisher: %s",
                                      selected_row->metadata.publisher.c_str());
                }
                if (cvars::ui_show_developer_metadata &&
                    !selected_row->metadata.developer.empty()) {
                  ImGui::TextDisabled("Developer: %s",
                                      selected_row->metadata.developer.c_str());
                }
                if (cvars::ui_show_genre_metadata &&
                    !selected_row->metadata.genre.empty()) {
                  std::string genre_str;
                  for (size_t i = 0; i < selected_row->metadata.genre.size();
                       ++i) {
                    if (i > 0) {
                      genre_str += ",";
                    }
                    genre_str += selected_row->metadata.genre[i];
                  }
                  ImGui::TextDisabled("Genre: %s", genre_str.c_str());
                }
                if (cvars::ui_show_release_date_metadata &&
                    !selected_row->metadata.release_date.empty()) {
                  ImGui::TextDisabled(
                      "Release Date: %s",
                      selected_row->metadata.release_date.c_str());
                }
                if (cvars::ui_show_user_rating_metadata &&
                    !selected_row->metadata.user_rating.empty()) {
                  ImGui::TextDisabled(
                      "Rating: %s", selected_row->metadata.user_rating.c_str());
                }
              }

              if (cvars::ui_show_title_id_metadata &&
                  !selected_row->title_id.empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled("Title ID: %s",
                                    selected_row->title_id.c_str());
              }
              if (selected_row->has_metadata &&
                  selected_row->metadata.metadata_ready) {
                if (!selected_row->metadata.title_type.empty()) {
                  ImGui::TextDisabled(
                      "Type: %s", selected_row->metadata.title_type.c_str());
                }
              } else if (selected_row->metadata.request_started &&
                         !selected_row->metadata.request_finished) {
                ImGui::TextDisabled("Loading fallback metadata...");
              }
              ImGui::PopTextWrapPos();
              ImGui::EndChild();
            }
          }
          ImGui::EndChild();

          if (show_search_panel_) {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->Pos);
            ImGui::SetNextWindowSize(viewport->Size);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            const ImGuiWindowFlags dim_flags =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoCollapse;
            if (ImGui::Begin("##gamelist_search_dim", nullptr, dim_flags)) {
              ImDrawList* dim_draw_list = ImGui::GetWindowDrawList();
              const ImVec2 dim_min = ImGui::GetWindowPos();
              const ImVec2 dim_max =
                  ImVec2(dim_min.x + ImGui::GetWindowSize().x,
                         dim_min.y + ImGui::GetWindowSize().y);
              dim_draw_list->AddRectFilled(
                  dim_min, dim_max,
                  ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f)));
            }
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);

            // Get the selected guide image from config
            std::string guide_asset_path =
                ResolveConfiguredGuideBackgroundAssetPath(false);

            auto search_panel_tex = GetOrCreateImageTexture(guide_asset_path);
            const float fallback_aspect = 520.0f / 640.0f;
            float texture_aspect = fallback_aspect;
            if (search_panel_tex && search_panel_tex->height > 0) {
              texture_aspect = static_cast<float>(search_panel_tex->width) /
                               static_cast<float>(search_panel_tex->height);
            }

            const float panel_height = io.DisplaySize.y;
            const float panel_width = texture_aspect * panel_height;
            const ImVec2 panel_size(panel_width, panel_height);
            const ImVec2 panel_padding(32.0f * display_scale,
                                       34.0f * display_scale);

            const ImVec2 viewport_pos = ImGui::GetMainViewport()->Pos;
            const ImVec2 overlay_pos(viewport_pos.x, viewport_pos.y);

            if (search_panel_focus_input_requested_ ||
                search_panel_list_focus_requested_) {
              ImGui::SetNextWindowFocus();
            }
            ImGui::SetNextWindowPos(overlay_pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(panel_size, ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            const ImGuiWindowFlags search_flags =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove;
            bool search_panel_should_close = false;
            if (ImGui::Begin("##gamelist_search_overlay", nullptr,
                             search_flags)) {
              ImDrawList* overlay_draw_list = ImGui::GetWindowDrawList();
              const ImVec2 overlay_min = ImGui::GetWindowPos();
              const ImVec2 overlay_max =
                  ImVec2(overlay_min.x + ImGui::GetWindowSize().x,
                         overlay_min.y + ImGui::GetWindowSize().y);
              DrawGuidePanelBackground(overlay_draw_list, search_panel_tex,
                                       overlay_min, overlay_max);

              if (key_pressed(ImGuiKey_GamepadFaceRight, false)) {
                search_panel_should_close = true;
              }

              const float ux = io.DisplaySize.x / 1024.0f;
              const float uy = io.DisplaySize.y / 576.0f;
              const OverlayHeaderLayout header_layout =
                  DrawOverlayHeader(overlay_draw_list, overlay_min, panel_size,
                                    panel_padding, ux, uy, "Search Games");
              const float content_start_y = header_layout.position.y +
                                            header_layout.font_size +
                                            (65.0f * display_scale);
              const float min_search_width = 240.0f * display_scale;
              const float target_search_width = panel_size.x -
                                                (panel_padding.x * 2.0f) -
                                                (160.0f * display_scale);
              const float search_field_width =
                  std::max(min_search_width, target_search_width);
              const float search_field_offset = 35.0f * display_scale;
              const float search_field_x =
                  overlay_min.x + (panel_size.x * 0.5f) -
                  (search_field_width * 0.5f) - search_field_offset;
              ImGui::SetCursorScreenPos(
                  ImVec2(search_field_x, content_start_y));
              ImGui::PushItemWidth(search_field_width);

              if (search_panel_focus_input_requested_) {
                ImGui::SetKeyboardFocusHere();
                search_panel_focus_input_requested_ = false;
                gamelist_window_focus_requested = false;
              }
              ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                                  14.0f * display_scale);
              ImGui::InputText("##search_query", search_input_buffer_,
                               sizeof(search_input_buffer_));
              ImGui::PopStyleVar();
              if (search_input_buffer_[0] != '\0') {
                const ImVec2 input_min = ImGui::GetItemRectMin();
                const ImVec2 input_max = ImGui::GetItemRectMax();
                const ImGuiStyle& input_style = ImGui::GetStyle();
                const float text_height = ImGui::GetFontSize();
                const float text_y =
                    input_min.y +
                    (input_max.y - input_min.y - text_height) * 0.5f;
                const ImVec2 text_pos(input_min.x + input_style.FramePadding.x,
                                      text_y);
                DrawTextWithConfiguredEffect(
                    ImGui::GetWindowDrawList(), ImGui::GetFont(),
                    ImGui::GetFontSize(), text_pos,
                    ImGui::GetColorU32(ImGuiCol_Text), search_input_buffer_);
              }
              const bool search_input_focused = ImGui::IsItemFocused();
              ImGui::PopItemWidth();
              if (ImGui::IsItemActivated()) {
                UWP::ShowKeyboard();
              }

              if (search_input_focused) {
                gamelist_window_focus_requested = false;
              }

              const std::string lowered_query =
                  normalize_search_text(search_input_buffer_);
              const bool has_query = !lowered_query.empty();
              std::vector<const GameListRow*> filtered_rows;
              filtered_rows.reserve(game_rows.size());
              if (has_query) {
                for (const auto& row : game_rows) {
                  bool matches =
                      normalize_search_text(get_game_display_title(row))
                          .find(lowered_query) != std::string::npos;
                  if (!matches) {
                    matches = normalize_search_text(row.filename)
                                  .find(lowered_query) != std::string::npos;
                  }
                  if (!matches) {
                    matches = normalize_search_text(row.path)
                                  .find(lowered_query) != std::string::npos;
                  }
                  if (!matches && !row.title_id.empty()) {
                    matches = normalize_search_text(row.title_id)
                                  .find(lowered_query) != std::string::npos;
                  }
                  if (matches) {
                    filtered_rows.push_back(&row);
                  }
                }
              } else {
                search_selected_game_path_.clear();
              }

              const GameListRow* search_selected_row = nullptr;
              for (const auto* row : filtered_rows) {
                if (row->path == search_selected_game_path_) {
                  search_selected_row = row;
                  break;
                }
              }
              if (!filtered_rows.empty() && !search_selected_row) {
                search_selected_game_path_ = filtered_rows.front()->path;
                search_selected_row = filtered_rows.front();
              }

              size_t search_selected_row_index = 0;
              if (search_selected_row) {
                for (size_t i = 0; i < filtered_rows.size(); ++i) {
                  if (filtered_rows[i]->path == search_selected_row->path) {
                    search_selected_row_index = i;
                    break;
                  }
                }
              }

              if (search_input_focused && !filtered_rows.empty() &&
                  (key_pressed(ImGuiKey_GamepadDpadDown, false) ||
                   key_pressed(ImGuiKey_DownArrow, false))) {
                search_panel_list_focus_requested_ = true;
              }

              ImGui::Dummy(ImVec2(0.0f, 8.0f * display_scale));
              const float results_height = ImGui::GetContentRegionAvail().y;
              if (ImGui::BeginChild("##search_results",
                                    ImVec2(-1.0f, results_height), false,
                                    ImGuiWindowFlags_NoScrollbar)) {
                const float carousel_outer_padding = 14.0f * display_scale;
                const float carousel_spacing = 16.0f * display_scale;
                const float carousel_card_height = 126.0f * display_scale;
                const std::vector<size_t> ordered_search_row_indices =
                    build_top_loop_order(filtered_rows.size(),
                                         search_selected_row_index);
                ImGui::Dummy(ImVec2(0.0f, 6.0f * display_scale));
                for (size_t ordered_index = 0;
                     ordered_index < ordered_search_row_indices.size();
                     ++ordered_index) {
                  const auto* row =
                      filtered_rows[ordered_search_row_indices[ordered_index]];
                  ImGui::PushID(row->path.c_str());
                  const bool is_selected =
                      search_selected_game_path_ == row->path;
                  const float row_height = carousel_card_height;
                  const float side_inset = carousel_outer_padding;
                  const float card_width = std::max(
                      160.0f * display_scale,
                      ImGui::GetContentRegionAvail().x - side_inset * 2.0f);
                  const float row_start_x = ImGui::GetCursorPosX() + side_inset;
                  if (is_selected && search_panel_list_focus_requested_) {
                    ImGui::SetKeyboardFocusHere();
                    search_panel_list_focus_requested_ = false;
                  }
                  ImGui::SetCursorPosX(row_start_x);
                  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                  ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                        ImVec4(0, 0, 0, 0));
                  ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                        ImVec4(0, 0, 0, 0));
                  ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
                  if (ImGui::Selectable("##search_row", is_selected, 0,
                                        ImVec2(card_width, row_height))) {
                    search_selected_game_path_ = row->path;
                    selected_game_path_ = row->path;
                    selected_game_name_ = row->filename;
                    gamelist_focus_selected_request = true;
                  }
                  ImGui::PopStyleColor(4);
                  const bool row_focused = ImGui::IsItemFocused();
                  if (row_focused && !is_selected) {
                    search_selected_game_path_ = row->path;
                    selected_game_path_ = row->path;
                    selected_game_name_ = row->filename;
                  }
                  if (row_focused && filtered_rows.size() > 1) {
                    const bool nav_up =
                        key_pressed(ImGuiKey_GamepadDpadUp, false) ||
                        key_pressed(ImGuiKey_UpArrow, false);
                    const bool nav_down =
                        key_pressed(ImGuiKey_GamepadDpadDown, false) ||
                        key_pressed(ImGuiKey_DownArrow, false);
                    if (nav_up) {
                      size_t new_index = (search_selected_row_index +
                                          filtered_rows.size() - 1) %
                                         filtered_rows.size();
                      search_selected_row_index = new_index;
                      search_selected_game_path_ =
                          filtered_rows[new_index]->path;
                      selected_game_path_ = search_selected_game_path_;
                      selected_game_name_ = filtered_rows[new_index]->filename;
                      search_panel_list_focus_requested_ = true;
                    } else if (nav_down) {
                      size_t new_index = (search_selected_row_index + 1) %
                                         filtered_rows.size();
                      search_selected_row_index = new_index;
                      search_selected_game_path_ =
                          filtered_rows[new_index]->path;
                      selected_game_path_ = search_selected_game_path_;
                      selected_game_name_ = filtered_rows[new_index]->filename;
                      search_panel_list_focus_requested_ = true;
                    }
                  }
                  if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                  }
                  const ImVec2 item_min = ImGui::GetItemRectMin();
                  const ImVec2 item_max = ImGui::GetItemRectMax();
                  const float pad = 10.0f * display_scale;
                  const float art_height = carousel_card_height - pad * 2.0f;
                  auto art_tex = load_game_art(
                      *row, row->list_art_path,
                      {row->metadata.cover_path, row->metadata.icon_path});
                  const float art_width =
                      art_tex && art_tex->height
                          ? art_height * float(art_tex->width) /
                                float(art_tex->height)
                          : art_height;
                  const float image_x = item_min.x + pad;
                  const float image_y =
                      item_min.y +
                      std::max(0.0f, (row_height - art_height) * 0.5f);
                  const float text_area_min_x = image_x + art_width + pad * 2.2f;
                  const float text_area_max_x = item_max.x - pad;
                  const float text_area_width =
                      std::max(0.0f, text_area_max_x - text_area_min_x);
                  if (art_tex) {
                    ImGui::SetCursorScreenPos(ImVec2(image_x, image_y));
                    ImGui::Image(reinterpret_cast<ImTextureID>(art_tex.get()),
                                 ImVec2(art_width, art_height));
                  }

                  const std::string display = get_game_display_title(*row);
                  const std::string secondary_text =
                      get_game_subtitle(*row, display);
                  const ImVec2 title_size =
                      ImGui::CalcTextSize(display.c_str());
                  const ImVec2 secondary_size =
                      secondary_text.empty()
                          ? ImVec2(0.0f, 0.0f)
                          : ImGui::CalcTextSize(secondary_text.c_str());
                  const float text_line_spacing = 10.0f * display_scale;
                  const float combined_text_height =
                      title_size.y +
                      (secondary_text.empty()
                           ? 0.0f
                           : (text_line_spacing + secondary_size.y));
                  const float text_y =
                      item_min.y +
                      std::max(0.0f,
                               (row_height - combined_text_height) * 0.5f);
                  DrawTextWithConfiguredEffect(
                      ImGui::GetWindowDrawList(), ImGui::GetFont(),
                      ImGui::GetFontSize(), ImVec2(text_area_min_x, text_y),
                      IM_COL32(228, 228, 228, 255), display.c_str());

                  if (!secondary_text.empty()) {
                    DrawTextWithConfiguredEffect(
                        ImGui::GetWindowDrawList(), ImGui::GetFont(),
                        ImGui::GetFontSize(),
                        ImVec2(text_area_min_x,
                               text_y + title_size.y + text_line_spacing),
                        IM_COL32(153, 153, 153, 255), secondary_text.c_str());
                  }

                  if (row_focused && is_selected &&
                      key_pressed(ImGuiKey_GamepadFaceDown, false)) {
                    request_launch_selected_game = true;
                    launch_game_path = row->path;
                  }
                  if (row_focused && is_selected &&
                      key_pressed(ImGuiKey_F12, false)) {
                    selected_game_path_ = row->path;
                    selected_game_name_ = row->filename;
                    request_open_game_context_menu = true;
                  }

                  ImGui::PopID();
                  ImGui::Dummy(ImVec2(0.0f, carousel_spacing));
                }

                if (has_query && filtered_rows.empty()) {
                  ImGui::TextWrapped("No games match your search.");
                }
              }
              ImGui::EndChild();

              const float footer_text_size = 13.5f * uy;
              const float footer_icon_size = 15.6f * uy;
              const float footer_icon_gap = 22.0f * ux;
              const float footer_spacing_y = 17.0f * uy;
              const float footer_select_y = overlay_max.y - (32.0f * uy);
              const float footer_back_y = footer_select_y - footer_spacing_y;
              const float footer_base_x = overlay_min.x + panel_size.x * 0.9f;
              const float footer_back_text_x = footer_base_x - (70.0f * ux);
              const float footer_back_icon_offset = 35.0f * ux;
              const float footer_select_text_x = footer_base_x - (60.0f * ux);
              const float footer_select_icon_offset = 42.0f * ux;
              auto draw_footer_prompt = [&](char button, const char* text,
                                            float text_y, float text_x,
                                            float icon_offset) {
                DrawTextWithConfiguredEffect(
                    overlay_draw_list, ImGui::GetFont(), footer_text_size,
                    ImVec2(text_x, text_y), IM_COL32(205, 205, 205, 255), text);
                auto tex = GetOrCreateButtonTexture(button);
                if (!tex) {
                  return;
                }
                const ImVec2 icon_center(text_x + icon_offset,
                                         text_y + footer_text_size * 0.5f);
                const ImVec2 icon_half(footer_icon_size * 0.5f,
                                       footer_icon_size * 0.5f);
                overlay_draw_list->AddImage(
                    reinterpret_cast<ImTextureID>(tex.get()),
                    ImVec2(icon_center.x - icon_half.x,
                           icon_center.y - icon_half.y),
                    ImVec2(icon_center.x + icon_half.x,
                           icon_center.y + icon_half.y));
              };
              draw_footer_prompt('B', "Back", footer_back_y, footer_back_text_x,
                                 footer_back_icon_offset);
              draw_footer_prompt('A', "Select", footer_select_y,
                                 footer_select_text_x,
                                 footer_select_icon_offset);
            }
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);

            if (search_panel_should_close) {
              show_search_panel_ = false;
              search_panel_focus_input_requested_ = false;
              search_panel_list_focus_requested_ = false;
              search_gamepad_y_was_down_ = false;
            }
          }

          if (request_open_game_context_menu &&
              !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
              !selected_game_name_.empty()) {
            gamelist_last_x_open_time = ImGui::GetTime();
            show_game_context_menu_ = true;
            game_context_menu_focus_requested_ = true;
            if (show_search_panel_) {
              show_search_panel_ = false;
              search_panel_focus_input_requested_ = false;
              search_panel_list_focus_requested_ = false;
            }
          }

          bool close_context_menu = false;
          if (show_game_context_menu_ && !selected_game_name_.empty()) {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            std::string guide_asset_path =
                ResolveConfiguredGuideBackgroundAssetPath(false);

            auto game_menu_panel_tex =
                GetOrCreateImageTexture(guide_asset_path);
            const float fallback_aspect = 520.0f / 640.0f;
            float texture_aspect = fallback_aspect;
            if (game_menu_panel_tex && game_menu_panel_tex->height > 0) {
              texture_aspect = static_cast<float>(game_menu_panel_tex->width) /
                               static_cast<float>(game_menu_panel_tex->height);
            }

            const float panel_height = io.DisplaySize.y;
            const float panel_width = texture_aspect * panel_height;
            const ImVec2 panel_size(panel_width, panel_height);
            const ImVec2 panel_padding(32.0f * display_scale,
                                       34.0f * display_scale);

            ImGui::SetNextWindowPos(viewport->Pos);
            ImGui::SetNextWindowSize(viewport->Size);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            const ImGuiWindowFlags dim_flags =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoCollapse;
            if (ImGui::Begin("##gamelist_game_menu_dim", nullptr, dim_flags)) {
              ImDrawList* dim_draw_list = ImGui::GetWindowDrawList();
              const ImVec2 dim_min = ImGui::GetWindowPos();
              const ImVec2 dim_max =
                  ImVec2(dim_min.x + ImGui::GetWindowSize().x,
                         dim_min.y + ImGui::GetWindowSize().y);
              dim_draw_list->AddRectFilled(
                  dim_min, dim_max,
                  ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f)));
            }
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);

            if (game_context_menu_focus_requested_) {
              ImGui::SetNextWindowFocus();
            }
            ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(panel_size, ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            const ImGuiWindowFlags game_menu_flags =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove;
            if (ImGui::Begin("##gamelist_game_menu_overlay", nullptr,
                             game_menu_flags)) {
              ImDrawList* overlay_draw_list = ImGui::GetWindowDrawList();
              const ImVec2 overlay_min = ImGui::GetWindowPos();
              const ImVec2 overlay_max =
                  ImVec2(overlay_min.x + ImGui::GetWindowSize().x,
                         overlay_min.y + ImGui::GetWindowSize().y);
              DrawGuidePanelBackground(overlay_draw_list, game_menu_panel_tex,
                                       overlay_min, overlay_max);

              if (key_pressed(ImGuiKey_GamepadFaceRight, false) ||
                  key_pressed(ImGuiKey_Escape, false)) {
                close_context_menu = true;
              }

              const float ux = io.DisplaySize.x / 1024.0f;
              const float uy = io.DisplaySize.y / 576.0f;
              const OverlayHeaderLayout header_layout =
                  DrawOverlayHeader(overlay_draw_list, overlay_min, panel_size,
                                    panel_padding, ux, uy, "Game Menu");
              const std::string game_menu_title =
                  selected_row ? get_game_display_title(*selected_row)
                               : selected_game_name_;
              const float content_vertical_offset = 30.0f * display_scale;
              const float min_button_width = 240.0f * display_scale;
              const float target_button_width = panel_size.x -
                                                (panel_padding.x * 2.0f) -
                                                (160.0f * display_scale);
              const float button_width =
                  std::max(min_button_width, target_button_width);
              const float button_offset = 35.0f * display_scale;
              const float button_x = overlay_min.x + (panel_size.x * 0.5f) -
                                     (button_width * 0.5f) - button_offset;
              const float title_x = button_x + ImGui::GetStyle().FramePadding.x;
              const float title_y =
                  header_layout.position.y + header_layout.font_size +
                  (30.0f * display_scale) + content_vertical_offset;
              DrawTextWithConfiguredEffect(overlay_draw_list, ImGui::GetFont(),
                                           22.0f * uy, ImVec2(title_x, title_y),
                                           IM_COL32(228, 228, 228, 255),
                                           game_menu_title.c_str());
              float content_start_y =
                  title_y + (34.0f * display_scale) + (8.0f * display_scale);

              ImGui::SetCursorScreenPos(ImVec2(button_x, content_start_y));

              auto cstorage_root =
                  dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(
                      cvar::ConfigVars->find("storage_root")->second);
              std::filesystem::path storage_root =
                  cstorage_root->GetTypedConfigValue();
              if (storage_root.empty()) {
                storage_root = emulator_window_.emulator()->storage_root();
              }

              bool focus_first_game_context_option =
                  game_context_menu_focus_requested_;
              auto draw_game_context_option = [&](const char* visible_label) {
                const float option_y = ImGui::GetCursorScreenPos().y;
                ImGui::SetCursorScreenPos(ImVec2(button_x, option_y));
                const ImGuiStyle& option_style = ImGui::GetStyle();
                std::string option_id =
                    fmt::format("{}##game_ctx_option", visible_label);
                if (focus_first_game_context_option) {
                  ImGui::SetKeyboardFocusHere();
                  focus_first_game_context_option = false;
                  game_context_menu_focus_requested_ = false;
                }
                ImGui::PushStyleColor(ImGuiCol_Header,
                                      ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_NavCursor,
                                      ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                bool clicked = ImGui::Selectable(option_id.c_str(), false, 0,
                                                 ImVec2(button_width, 0.0f));
                bool highlighted = ImGui::IsItemHovered() ||
                                   ImGui::IsItemActive() ||
                                   ImGui::IsItemFocused();
                ImVec2 item_min = ImGui::GetItemRectMin();
                ImVec2 item_max = ImGui::GetItemRectMax();
                ImGui::PopStyleColor(5);
                if (highlighted) {
                  xe::app::DrawConfiguredComboHighlight(item_min, item_max,
                                                        1.6f, 6.0f);
                }
                const float line_height = ImGui::GetTextLineHeight();
                const ImVec2 text_pos(
                    item_min.x + option_style.FramePadding.x,
                    item_min.y +
                        (item_max.y - item_min.y - line_height) * 0.5f);
                DrawTextWithConfiguredEffect(
                    ImGui::GetWindowDrawList(), ImGui::GetFont(), ImGui::GetFontSize(),
                    text_pos, IM_COL32(228, 228, 228, 255), visible_label);
                if (clicked) {
                  close_context_menu = true;
                }
                ImGui::Spacing();
                return clicked;
              };

              if (!close_context_menu) {
                const auto body = ui::GetGuideBodyBounds(panel_size.y);
                ImGui::BeginChild("##game_menu_options",
                    ImVec2(button_width + 4.0f * display_scale,
                           std::max(1.0f, overlay_min.y + body.bottom - content_start_y)),
                    ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
                if (draw_game_context_option("Edit Metadata")) {
                  scraper.OpenMetadataEditor(
                      MakeScreenScraperTarget(std::filesystem::u8path(selected_game_path_),
                                             selected_game_name_),
                      selected_row ? selected_row->editable_metadata : screenscraper::Metadata{},
                      selected_row ? load_game_art(*selected_row, selected_row->list_art_path,
                          {selected_row->metadata.cover_path, selected_row->metadata.icon_path}) : nullptr,
                      selected_row ? load_game_art(*selected_row, selected_row->detail_art_path,
                          {selected_row->metadata.banner_path,
                           selected_row->metadata.gallery_paths.empty() ? std::string_view() :
                               selected_row->metadata.gallery_paths.front()}) : nullptr);
                }
                if (draw_game_context_option("Edit Patches")) {
                  OpenPatchEditor(storage_root / "patches",
                                  UWP::GetTitleIdFromPath(selected_game_path_));
                }

                if (draw_game_context_option("Download Patches")) {
                  std::filesystem::path patches_root = storage_root / "patches";
                  std::string patches_dir = patches_root.string();
                  std::string title_id =
                      UWP::GetTitleIdFromPath(selected_game_path_);
                  if (!title_id.empty()) {
                    if (has_existing_patch_for_title(patches_root, title_id)) {
                      pending_existing_download_kind_ =
                          ManualInstallKind::kPatch;
                      pending_existing_download_destination_ = patches_root;
                      pending_existing_download_title_id_ = title_id;
                      action_status_ = fmt::format(
                          "Patch files for {} already exist.\n"
                          "Re-download and override existing files, or cancel?",
                          title_id);
                      action_popup_mode_ =
                          ActionPopupMode::kExistingDownloadPrompt;
                      show_action_status_ = true;
                    } else {
                      begin_patch_download(title_id, patches_root);
                    }
                  } else {
                    action_status_ =
                        "Could not resolve title ID for patch download.";
                    action_popup_mode_ = ActionPopupMode::kInfo;
                    show_action_status_ = true;
                  }
                }

                if (draw_game_context_option("Apply Optimized Settings")) {
                  std::filesystem::path config_root = storage_root / "config";
                  std::string config_dir = config_root.string();
                  std::string title_id =
                      UWP::GetTitleIdFromPath(selected_game_path_);
                  if (!title_id.empty()) {
                    const auto config_path =
                        config_root / fmt::format("{}.config.toml", title_id);
                    if (std::filesystem::exists(config_path)) {
                      pending_existing_download_kind_ =
                          ManualInstallKind::kConfig;
                      pending_existing_download_destination_ = config_root;
                      pending_existing_download_title_id_ = title_id;
                      action_status_ = fmt::format(
                          "Optimized settings for {} already exist.\n"
                          "Re-download and override existing settings, or "
                          "cancel?",
                          title_id);
                      action_popup_mode_ =
                          ActionPopupMode::kExistingDownloadPrompt;
                      show_action_status_ = true;
                    } else {
                      begin_optimized_settings_download(title_id, config_root);
                    }
                  } else {
                    action_status_ =
                        "Could not resolve title ID for optimized settings "
                        "download.";
                    action_popup_mode_ = ActionPopupMode::kInfo;
                    show_action_status_ = true;
                  }
                }

                if (draw_game_context_option("Create / Edit Per-Game Config")) {
                  std::string title_id =
                      UWP::GetTitleIdFromPath(selected_game_path_);
                  const auto config_root = storage_root / "config";
                  const auto config_path =
                      config_root / fmt::format("{}.config.toml", title_id);
                  const std::string pergame_display_name =
                      [&]() -> std::string {
                    if (selected_row) {
                      auto display = get_game_display_title(*selected_row);
                      if (!display.empty()) {
                        return display;
                      }
                    }
                    if (!selected_game_name_.empty()) {
                      return selected_game_name_;
                    }
                    return std::string();
                  }();
                  if (title_id.empty()) {
                    action_status_ =
                        "Could not resolve title ID for per-game config.";
                    action_popup_mode_ = ActionPopupMode::kInfo;
                    show_action_status_ = true;
                  } else if (!std::filesystem::exists(config_path)) {
                    pending_per_game_config_title_id_ = title_id;
                    pending_per_game_config_path_ = config_path;
                    pending_per_game_config_title_display_ =
                        pergame_display_name.empty() ? title_id
                                                     : pergame_display_name;
                    action_status_ =
                        "No per-game config exists for this title yet.\n"
                        "Create one now, or browse and edit an existing file.";
                    action_popup_mode_ = ActionPopupMode::kPerGameConfigPrompt;
                    show_action_status_ = true;
                  } else {
                    std::ifstream ifs(config_path, std::ios::binary);
                    std::string existing_text(
                        (std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
                    open_per_game_config_editor(
                        title_id, config_path, existing_text,
                        pergame_display_name.empty() ? title_id
                                                     : pergame_display_name);
                  }
                }

                if (draw_game_context_option("Apply Config Overrides")) {
                  imgui_drawer()->SetIgnoreInput(true);
                  UWP::SelectFile([this, apply_per_game_config,
                                   parse_title_id_from_filename,
                                   ensure_plugin_layout_for_title](
                                      std::string file_path) {
                    imgui_drawer()->SetIgnoreInput(false);
                    if (file_path.empty()) {
                      return;
                    }
                    auto dest = pending_manual_destination_;
                    if (!dest.empty()) {
                      std::filesystem::create_directories(dest);
                      std::filesystem::path source_path = file_path;
                      std::filesystem::path destination_path =
                          dest / source_path.filename();
                      std::string title_id = pending_manual_title_id_;
                      if (title_id.empty()) {
                        title_id = parse_title_id_from_filename(
                            destination_path.filename().string());
                      }
                      if (pending_manual_kind_ == ManualInstallKind::kConfig &&
                          !title_id.empty()) {
                        destination_path =
                            dest / fmt::format("{}.config.toml", title_id);
                      }
                      std::error_code copy_ec;
                      std::filesystem::copy_file(
                          source_path, destination_path,
                          std::filesystem::copy_options::overwrite_existing,
                          copy_ec);
                      if (copy_ec) {
                        action_status_ =
                            "Failed to copy file. Please try again.";
                      } else {
                        if (pending_manual_kind_ == ManualInstallKind::kPatch) {
                          OpenPatchEditor(dest, title_id);
                        }
                        bool config_applied = true;
                        bool plugin_layout_ok = true;
                        std::string plugin_layout_error;
                        if (pending_manual_kind_ ==
                                ManualInstallKind::kConfig &&
                            !title_id.empty()) {
                          config_applied =
                              apply_per_game_config(title_id, destination_path);
                        } else if (pending_manual_kind_ ==
                                       ManualInstallKind::kPlugin &&
                                   !title_id.empty()) {
                          plugin_layout_ok = ensure_plugin_layout_for_title(
                              dest, title_id, selected_game_name_,
                              &plugin_layout_error);
                        }
                        switch (pending_manual_kind_) {
                          case ManualInstallKind::kPatch:
                            action_status_ =
                                "Patch installed. Choose its options in Edit Patches.";
                            break;
                          case ManualInstallKind::kPlugin:
                            action_status_ =
                                plugin_layout_ok
                                    ? "Plugin installed manually."
                                    : fmt::format(
                                          "Plugin copied manually, but "
                                          "plugins.toml layout is "
                                          "incomplete.\n{}",
                                          plugin_layout_error);
                            break;
                          case ManualInstallKind::kConfig:
                            action_status_ =
                                config_applied ? "Optimized settings installed "
                                                 "and applied manually."
                                               : "Optimized settings copied "
                                                 "manually, but apply failed.";
                            break;
                          default:
                            action_status_ = "File installed.";
                            break;
                        }
                      }
                      action_popup_mode_ = ActionPopupMode::kInfo;
                      show_action_status_ = true;
                    }
                  });
                }

                if (draw_game_context_option("Install Plugin")) {
                  std::filesystem::path plugins_root = storage_root / "plugins";
                  std::string title_id =
                      UWP::GetTitleIdFromPath(selected_game_path_);
                  if (!title_id.empty()) {
                    std::filesystem::path title_plugins_dir =
                        plugins_root / title_id;
                    UWP::DownloadPluginsForGame(
                        title_id, title_plugins_dir.string(),
                        [this, open_manual_prompt,
                         ensure_plugin_layout_for_title, title_id,
                         title_plugins_dir](bool ok, std::string msg) {
                          if (ok) {
                            std::string layout_error;
                            const bool layout_ok =
                                ensure_plugin_layout_for_title(
                                    title_plugins_dir, title_id,
                                    selected_game_name_, &layout_error);
                            if (layout_ok) {
                              action_status_ = fmt::format(
                                  "Downloaded and installed plugins for {}.",
                                  title_id);
                            } else {
                              action_status_ = fmt::format(
                                  "Downloaded plugin files for {}, but install "
                                  "layout is incomplete.\n{}",
                                  title_id, layout_error);
                            }
                            action_popup_mode_ = ActionPopupMode::kInfo;
                            pending_manual_kind_ = ManualInstallKind::kPlugin;
                            pending_manual_destination_ = title_plugins_dir;
                            pending_manual_title_id_ = title_id;
                            action_popup_should_close_ = true;
                            show_action_status_ = true;
                          } else {
                            open_manual_prompt(
                                title_plugins_dir, ManualInstallKind::kPlugin,
                                title_id,
                                "No matching plugins were found online.\n"
                                "Do you want to select a plugin manually?");
                          }
                        });
                  } else {
                    action_status_ =
                        "Could not resolve title ID for plugin download.";
                    action_popup_mode_ = ActionPopupMode::kInfo;
                    show_action_status_ = true;
                  }
                }

                if (draw_game_context_option("Manage DLC")) {
                  std::string title_id =
                      UWP::GetTitleIdFromPath(selected_game_path_);
                  if (!title_id.empty()) {
                    emulator_window_.InstallContentForTitle(
                        title_id, storage_root,
                        EmulatorWindow::DlcInstallMode::kManual);
                  } else {
                    emulator_window_.InstallContent();
                  }
                }

                if (draw_game_context_option("Manage Title Updates")) {
                  if (!title_update_dialog_) {
                    title_update_dialog_ = std::make_unique<GameTitleUpdateDialog>();
                  }
                  const auto game =
                      UWP::GetTitleUpdateGameInfo(selected_game_path_);
                  title_update_dialog_->Open(game.title_id, game.media_id,
                                            game.base_version, storage_root);
                }

                if (draw_game_context_option("Installed Content")) {
                  const std::string title_id =
                      UWP::GetTitleIdFromPath(selected_game_path_);
                  uint32_t title_filter = 0;
                  if (!title_id.empty()) {
                    title_filter = static_cast<uint32_t>(
                        std::strtoul(title_id.c_str(), nullptr, 16));
                  }
                  emulator_window_.ToggleContentListDialogForTitle(
                      title_filter);
                }

                ImGui::EndChild();

                const float footer_text_size = 13.5f * uy;
                const float footer_icon_size = 15.6f * uy;
                const float footer_spacing_y = 17.0f * uy;
                const float footer_select_y = overlay_max.y - (32.0f * uy);
                const float footer_back_y = footer_select_y - footer_spacing_y;
                const float footer_base_x = overlay_min.x + panel_size.x * 0.9f;
                const float footer_back_text_x = footer_base_x - (70.0f * ux);
                const float footer_back_icon_offset = 35.0f * ux;
                const float footer_select_text_x = footer_base_x - (60.0f * ux);
                const float footer_select_icon_offset = 42.0f * ux;
                xe::app::DrawFooterPrompt(
                    overlay_draw_list, GetOrCreateButtonTexture('B'),
                    footer_text_size, footer_icon_size, "Back", footer_back_y,
                    footer_back_text_x, footer_back_icon_offset);
                xe::app::DrawFooterPrompt(
                    overlay_draw_list, GetOrCreateButtonTexture('A'),
                    footer_text_size, footer_icon_size, "Select",
                    footer_select_y, footer_select_text_x,
                    footer_select_icon_offset);
              }
            }

            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);
            if (close_context_menu) {
              show_game_context_menu_ = false;
              game_context_menu_focus_requested_ = false;
              gamelist_window_focus_requested = true;
              gamelist_focus_selected_request = true;
            }
          }

          if (patch_editor_ && patch_editor_->is_open()) show_action_status_ = false;
          const bool close_action_popup =
              std::exchange(action_popup_should_close_, false) &&
              !show_action_status_;
          if (show_action_status_) {
            ImGui::OpenPopup("Action Result");
            show_action_status_ = false;
            action_popup_focus_requested_ = true;
          }

          const ImGuiViewport* action_viewport = ImGui::GetMainViewport();
          ImGui::SetNextWindowPos(action_viewport->Pos);
          ImGui::SetNextWindowSize(action_viewport->Size);
          const ImGuiWindowFlags action_popup_flags =
              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
          if (action_popup_focus_requested_) {
            ImGui::SetNextWindowFocus();
          }
          if (ImGui::BeginPopupModal("Action Result", nullptr,
                                     action_popup_flags)) {
            if (close_action_popup) {
              ImGui::CloseCurrentPopup();
            }
            ImDrawList* action_draw_list = ImGui::GetWindowDrawList();
            const ImVec2 screen_min = action_viewport->Pos;
            const ImVec2 screen_max =
                ImVec2(screen_min.x + action_viewport->Size.x,
                       screen_min.y + action_viewport->Size.y);
            action_draw_list->AddRectFilled(
                screen_min, screen_max,
                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f)));

            std::string guide_asset_path =
                ResolveConfiguredGuideBackgroundAssetPath(false);
            auto action_panel_tex = GetOrCreateImageTexture(guide_asset_path);
            const float panel_height = io.DisplaySize.y;
            const float panel_width =
                std::min(action_viewport->Size.x,
                         GetGuidePanelSize(action_panel_tex, panel_height).x);
            const ImVec2 panel_size(panel_width, panel_height);
            const ImVec2 panel_padding(32.0f * display_scale,
                                       34.0f * display_scale);
            const float ux = io.DisplaySize.x / 1024.0f;
            const float uy = io.DisplaySize.y / 576.0f;
            const ImVec2 overlay_min = action_viewport->Pos;
            const ImVec2 overlay_max(overlay_min.x + panel_size.x,
                                     overlay_min.y + panel_size.y);
            DrawGuidePanelBackground(action_draw_list, action_panel_tex,
                                     overlay_min, overlay_max);

            auto get_action_popup_title = [&]() -> const char* {
              switch (action_popup_mode_) {
                case ActionPopupMode::kInfo:
                  return "Action Result";
                case ActionPopupMode::kManualPatchPrompt:
                  return "Manual Patch Install";
                case ActionPopupMode::kManualPluginPrompt:
                  return "Manual Plugin Install";
                case ActionPopupMode::kManualConfigPrompt:
                  return "Manual Config Install";
                case ActionPopupMode::kExistingDownloadPrompt:
                  return "Download Override";
                case ActionPopupMode::kPerGameConfigPrompt:
                  return "Per Game Config";
                default:
                  return "Action Result";
              }
            };

            const OverlayHeaderLayout header_layout = DrawOverlayHeader(
                action_draw_list, overlay_min, panel_size, panel_padding, ux,
                uy, get_action_popup_title());
            const float content_start_y = header_layout.position.y +
                                          header_layout.font_size +
                                          (65.0f * display_scale);
            const float content_x = overlay_min.x + panel_padding.x;
            const float field_width = 320.0f * ux;
            const float button_gap = 8.0f * ux;
            const ImVec2 action_text_pos(content_x, content_start_y);
            DrawTextWithConfiguredEffect(action_draw_list, ImGui::GetFont(),
                                         ImGui::GetFontSize(), action_text_pos,
                                         ImGui::GetColorU32(ImGuiCol_Text),
                                         action_status_.c_str());
            const ImVec2 action_text_size = ImGui::CalcTextSize(
                action_status_.c_str(), nullptr, false, field_width);
            ImGui::SetCursorScreenPos(
                ImVec2(content_x,
                       content_start_y + action_text_size.y + (24.0f * uy)));

            auto focus_first_action = [&]() {
              if (action_popup_focus_requested_) {
                ImGui::SetKeyboardFocusHere();
                action_popup_focus_requested_ = false;
              }
            };
            auto draw_action_button = [&](const char* label,
                                          const ImVec2& size =
                                              ImVec2(0.0f, 0.0f)) {
              return DrawTextEffectButton(label, size);
            };

            if (action_popup_mode_ == ActionPopupMode::kManualPatchPrompt ||
                action_popup_mode_ == ActionPopupMode::kManualPluginPrompt ||
                action_popup_mode_ == ActionPopupMode::kManualConfigPrompt) {
              const ImVec2 button_size((field_width - button_gap) * 0.5f, 0.0f);
              focus_first_action();
              if (draw_action_button("Browse", button_size)) {
                imgui_drawer()->SetIgnoreInput(true);
                UWP::SelectFile([this, parse_title_id_from_filename,
                                 apply_per_game_config,
                                 ensure_plugin_layout_for_title](
                                    std::string file_path) {
                  imgui_drawer()->SetIgnoreInput(false);
                  if (file_path.empty()) {
                    return;
                  }
                  auto dest = pending_manual_destination_;
                  if (!dest.empty()) {
                    std::filesystem::create_directories(dest);
                    std::filesystem::path source_path = file_path;
                    std::filesystem::path destination_path =
                        dest / source_path.filename();
                    std::string title_id = pending_manual_title_id_;
                    if (title_id.empty()) {
                      title_id = parse_title_id_from_filename(
                          destination_path.filename().string());
                    }
                    if (pending_manual_kind_ == ManualInstallKind::kConfig &&
                        !title_id.empty()) {
                      destination_path =
                          dest / fmt::format("{}.config.toml", title_id);
                    }
                    std::error_code copy_ec;
                    std::filesystem::copy_file(
                        source_path, destination_path,
                        std::filesystem::copy_options::overwrite_existing,
                        copy_ec);
                    if (copy_ec) {
                      action_status_ = "Failed to copy file. Please try again.";
                    } else {
                      if (pending_manual_kind_ == ManualInstallKind::kPatch) {
                          OpenPatchEditor(dest, title_id);
                        }
                      bool config_applied = true;
                      bool plugin_layout_ok = true;
                      std::string plugin_layout_error;
                      if (pending_manual_kind_ == ManualInstallKind::kConfig &&
                          !title_id.empty()) {
                        config_applied =
                            apply_per_game_config(title_id, destination_path);
                      } else if (pending_manual_kind_ ==
                                     ManualInstallKind::kPlugin &&
                                 !title_id.empty()) {
                        plugin_layout_ok = ensure_plugin_layout_for_title(
                            dest, title_id, selected_game_name_,
                            &plugin_layout_error);
                      }
                      switch (pending_manual_kind_) {
                        case ManualInstallKind::kPatch:
                          action_status_ =
                              "Patch installed. Choose its options in Edit Patches.";
                          break;
                        case ManualInstallKind::kPlugin:
                          action_status_ = plugin_layout_ok
                                               ? "Plugin installed manually."
                                               : fmt::format(
                                                     "Plugin copied manually, "
                                                     "but plugins.toml layout "
                                                     "is incomplete.\n{}",
                                                     plugin_layout_error);
                          break;
                        case ManualInstallKind::kConfig:
                          action_status_ = config_applied
                                               ? "Optimized settings installed "
                                                 "and applied manually."
                                               : "Optimized settings copied "
                                                 "manually, but apply failed.";
                          break;
                        default:
                          action_status_ = "File installed.";
                          break;
                      }
                    }
                    action_popup_mode_ = ActionPopupMode::kInfo;
                    show_action_status_ = true;
                  }
                });
              }
              ImGui::SameLine(0.0f, button_gap);
              if (draw_action_button("Cancel", button_size)) {
                ImGui::CloseCurrentPopup();
                action_popup_mode_ = ActionPopupMode::kInfo;
                pending_manual_kind_ = ManualInstallKind::kNone;
              }
            } else if (action_popup_mode_ ==
                       ActionPopupMode::kPerGameConfigPrompt) {
              const ImVec2 button_size(field_width, 0.0f);
              focus_first_action();
              if (draw_action_button("Create Per Game Config", button_size)) {
                std::string initial =
                    "# Per-game config overrides\n"
                    "# Add only the options you want to override for this "
                    "title.\n"
                    "\n[General]\n";
                if (std::filesystem::exists(pending_per_game_config_path_)) {
                  std::ifstream ifs(pending_per_game_config_path_,
                                    std::ios::binary);
                  initial.assign((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());
                }
                open_per_game_config_editor(
                    pending_per_game_config_title_id_,
                    pending_per_game_config_path_, initial,
                    pending_per_game_config_title_display_);
                ImGui::CloseCurrentPopup();
                action_popup_mode_ = ActionPopupMode::kInfo;
              }
              ImGui::SetCursorScreenPos(ImVec2(
                  content_x, ImGui::GetCursorScreenPos().y + (8.0f * uy)));
              if (draw_action_button("Browse and Edit", button_size)) {
                imgui_drawer()->SetIgnoreInput(true);
                UWP::SelectFile([this, open_per_game_config_editor](
                                    std::string file_path) {
                  imgui_drawer()->SetIgnoreInput(false);
                  if (file_path.empty()) {
                    return;
                  }
                  std::ifstream ifs(file_path, std::ios::binary);
                  std::string contents((std::istreambuf_iterator<char>(ifs)),
                                       std::istreambuf_iterator<char>());
                  open_per_game_config_editor(
                      pending_per_game_config_title_id_,
                      pending_per_game_config_path_, contents,
                      pending_per_game_config_title_display_);
                });
                ImGui::CloseCurrentPopup();
                action_popup_mode_ = ActionPopupMode::kInfo;
              }
              ImGui::SetCursorScreenPos(ImVec2(
                  content_x, ImGui::GetCursorScreenPos().y + (8.0f * uy)));
              if (draw_action_button("Cancel", button_size)) {
                ImGui::CloseCurrentPopup();
                action_popup_mode_ = ActionPopupMode::kInfo;
              }
            } else if (action_popup_mode_ ==
                       ActionPopupMode::kExistingDownloadPrompt) {
              const ImVec2 button_size(field_width, 0.0f);
              focus_first_action();
              if (draw_action_button("Re-download and Override", button_size)) {
                const auto kind = pending_existing_download_kind_;
                const auto dest = pending_existing_download_destination_;
                const auto title_id = pending_existing_download_title_id_;
                pending_existing_download_kind_ = ManualInstallKind::kNone;
                pending_existing_download_destination_.clear();
                pending_existing_download_title_id_.clear();

                ImGui::CloseCurrentPopup();
                action_popup_mode_ = ActionPopupMode::kInfo;

                if (kind == ManualInstallKind::kPatch) {
                  begin_patch_download(title_id, dest);
                } else if (kind == ManualInstallKind::kConfig) {
                  begin_optimized_settings_download(title_id, dest);
                }
              }
              ImGui::SetCursorScreenPos(ImVec2(
                  content_x, ImGui::GetCursorScreenPos().y + (8.0f * uy)));
              if (draw_action_button("Cancel", button_size)) {
                pending_existing_download_kind_ = ManualInstallKind::kNone;
                pending_existing_download_destination_.clear();
                pending_existing_download_title_id_.clear();
                ImGui::CloseCurrentPopup();
                action_popup_mode_ = ActionPopupMode::kInfo;
              }
            } else {
              focus_first_action();
              if (draw_action_button("OK", ImVec2(field_width, 0.0f))) {
                ImGui::CloseCurrentPopup();
              }
            }

            const float footer_text_size = 13.5f * uy;
            const float footer_icon_size = 15.6f * uy;
            const float footer_spacing_y = 17.0f * uy;
            const float footer_select_y = overlay_max.y - (32.0f * uy);
            const float footer_back_y = footer_select_y - footer_spacing_y;
            const float footer_base_x = overlay_min.x + panel_size.x * 0.9f;
            const float footer_back_text_x = footer_base_x - (70.0f * ux);
            const float footer_back_icon_offset = 35.0f * ux;
            const float footer_select_text_x = footer_base_x - (60.0f * ux);
            const float footer_select_icon_offset = 42.0f * ux;
            xe::app::DrawFooterPrompt(
                action_draw_list, GetOrCreateButtonTexture('B'),
                footer_text_size, footer_icon_size, "Back", footer_back_y,
                footer_back_text_x, footer_back_icon_offset);
            xe::app::DrawFooterPrompt(
                action_draw_list, GetOrCreateButtonTexture('A'),
                footer_text_size, footer_icon_size, "Select", footer_select_y,
                footer_select_text_x, footer_select_icon_offset);

            ImGui::EndPopup();
          }

          if (show_per_game_config_editor_) {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const std::string pergame_header_title =
                pending_per_game_config_title_display_.empty()
                    ? std::string("Game Config")
                    : fmt::format("{} - Game Config",
                                  pending_per_game_config_title_display_);
            std::string guide_asset_path =
                ResolveConfiguredGuideBackgroundAssetPath(true);

            auto pergame_panel_tex = GetOrCreateImageTexture(guide_asset_path);
            const float panel_height = io.DisplaySize.y;
            const float panel_width =
                std::min(viewport->Size.x,
                         GetGuidePanelSize(pergame_panel_tex, panel_height).x);
            const ImVec2 panel_size(panel_width, panel_height);
            const ImVec2 panel_padding(38.0f * display_scale,
                                       34.0f * display_scale);

            ImGui::SetNextWindowPos(viewport->Pos);
            ImGui::SetNextWindowSize(viewport->Size);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            const ImGuiWindowFlags dim_flags =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoCollapse;
            if (ImGui::Begin("##pergame_config_dim", nullptr, dim_flags)) {
              ImDrawList* dim_draw_list = ImGui::GetWindowDrawList();
              const ImVec2 dim_min = ImGui::GetWindowPos();
              const ImVec2 dim_max =
                  ImVec2(dim_min.x + ImGui::GetWindowSize().x,
                         dim_min.y + ImGui::GetWindowSize().y);
              dim_draw_list->AddRectFilled(
                  dim_min, dim_max,
                  ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f)));
            }
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);

            if (per_game_config_popup_focus_requested_) {
              ImGui::SetNextWindowFocus();
            }
            ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(panel_size, ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            const ImGuiWindowFlags pergame_overlay_flags =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove;
            bool close_per_game_config_editor = false;
            if (ImGui::Begin("##pergame_config_overlay", nullptr,
                             pergame_overlay_flags)) {
              const ImGuiStyle& pergame_style = ImGui::GetStyle();
              ImDrawList* pergame_draw_list = ImGui::GetWindowDrawList();
              const ImVec2 pergame_window_pos = ImGui::GetWindowPos();
              const ImVec2 pergame_window_size = ImGui::GetWindowSize();
              const ImVec2 pergame_window_max(
                  pergame_window_pos.x + pergame_window_size.x,
                  pergame_window_pos.y + pergame_window_size.y);
              DrawGuidePanelBackground(pergame_draw_list, pergame_panel_tex,
                                       pergame_window_pos, pergame_window_max);
              if (key_pressed(ImGuiKey_GamepadFaceRight, false) ||
                  key_pressed(ImGuiKey_Escape, false)) {
                close_per_game_config_editor = true;
              }
              const float ux = io.DisplaySize.x / 1024.0f;
              const float uy = io.DisplaySize.y / 576.0f;
              const OverlayHeaderLayout pergame_header_layout =
                  DrawOverlayHeader(pergame_draw_list, pergame_window_pos,
                                    panel_size, panel_padding, ux, uy,
                                    pergame_header_title.c_str());
              const float pergame_content_left = pergame_window_pos.x +
                                                 panel_padding.x +
                                                 (12.0f * display_scale);
              const float pergame_content_top =
                  pergame_header_layout.position.y +
                  pergame_header_layout.font_size + (36.0f * display_scale) +
                  (30.0f * display_scale);

              const std::string pergame_help_text =
                  "Controller: + add, - remove mode, select option to "
                  "edit/apply.";
              const ImVec2 pergame_help_pos(
                  pergame_content_left,
                  pergame_content_top + (0.0f * display_scale));
              DrawTextWithConfiguredEffect(pergame_draw_list, ImGui::GetFont(),
                                           14.0f * uy, pergame_help_pos,
                                           ImGui::GetColorU32(ImGuiCol_Text),
                                           pergame_help_text.c_str());
              ImGui::SetCursorScreenPos(
                  ImVec2(pergame_content_left,
                         pergame_content_top + (28.0f * display_scale)));

              const float footer_reserved_height =
                  std::max(92.0f * display_scale,
                           ImGui::GetFrameHeightWithSpacing() +
                               pergame_style.ItemSpacing.y * 2.0f);
              const float reduced_footer_height =
                  footer_reserved_height * 2.55f;
              const float footer_back_icon_right_x =
                  pergame_window_pos.x + panel_size.x * 0.9f - (70.0f * ux) +
                  (35.0f * ux) + ((15.6f * uy) * 0.5f);
              std::vector<const PerGameConfigOption*> addable_options;
              addable_options.reserve(per_game_config_options_.size());
              for (const auto& option : per_game_config_options_) {
                if (per_game_config_entries_.find(option.name) ==
                    per_game_config_entries_.end()) {
                  addable_options.push_back(&option);
                }
              }

              if (per_game_config_add_candidate_.empty() ||
                  std::none_of(addable_options.begin(), addable_options.end(),
                               [this](const PerGameConfigOption* option) {
                                 return option->name ==
                                        per_game_config_add_candidate_;
                               })) {
                if (!addable_options.empty()) {
                  per_game_config_add_candidate_ =
                      addable_options.front()->name;
                } else {
                  per_game_config_add_candidate_.clear();
                }
              }
              const bool preview_option_available = std::any_of(
                  addable_options.begin(), addable_options.end(),
                  [this](const PerGameConfigOption* option) {
                    return option->name == per_game_config_add_preview_option_;
                  });
              if (!preview_option_available) {
                per_game_config_add_preview_option_ =
                    per_game_config_add_candidate_;
              }

              const std::string previous_per_game_focus_control =
                  per_game_config_last_focused_control_id_;
              PerGameConfigFocusSide per_game_focus_side =
                  PerGameConfigFocusSide::kNone;
              std::string per_game_focus_control_id;
              bool per_game_focus_has_left_neighbor = false;
              bool per_game_focus_has_right_neighbor = false;
              auto track_per_game_focus =
                  [&per_game_focus_side, &per_game_focus_control_id,
                   &per_game_focus_has_left_neighbor,
                   &per_game_focus_has_right_neighbor](
                      PerGameConfigFocusSide side,
                      const std::string& control_id, bool has_left_neighbor,
                      bool has_right_neighbor) {
                    if (!ImGui::IsItemFocused()) {
                      return;
                    }
                    per_game_focus_side = side;
                    per_game_focus_control_id = control_id;
                    per_game_focus_has_left_neighbor = has_left_neighbor;
                    per_game_focus_has_right_neighbor = has_right_neighbor;
                  };
              auto focus_if_requested = [this](const std::string& control_id) {
                if (per_game_config_control_jump_request_ == control_id) {
                  ImGui::SetKeyboardFocusHere();
                  per_game_config_control_jump_request_.clear();
                }
              };

              bool per_game_has_configured_options = false;
              std::string per_game_first_config_entry_control_id;
              std::string per_game_last_config_entry_control_id;
              const bool per_game_nav_right_pressed =
                  key_pressed(ImGuiKey_GamepadDpadRight, false) ||
                  key_pressed(ImGuiKey_RightArrow, false);
              const bool per_game_nav_left_pressed =
                  key_pressed(ImGuiKey_GamepadDpadLeft, false) ||
                  key_pressed(ImGuiKey_LeftArrow, false);
              const bool per_game_nav_down_pressed =
                  key_pressed(ImGuiKey_GamepadDpadDown, false) ||
                  key_pressed(ImGuiKey_DownArrow, false);
              const bool per_game_nav_up_pressed =
                  key_pressed(ImGuiKey_GamepadDpadUp, false) ||
                  key_pressed(ImGuiKey_UpArrow, false);

              if (ImGui::BeginChild(
                      "##pergame_editor_plane",
                      ImVec2(0.0f, -reduced_footer_height),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoNavFocus)) {
                if (ImGui::BeginTable("##pergame_layout", 3,
                                      ImGuiTableFlags_SizingStretchProp)) {
                  ImGui::TableSetupColumn("##pergame_left_col",
                                          ImGuiTableColumnFlags_WidthFixed,
                                          360.0f);
                  ImGui::TableSetupColumn("##pergame_gap_col",
                                          ImGuiTableColumnFlags_WidthFixed |
                                              ImGuiTableColumnFlags_NoResize,
                                          18.0f);
                  ImGui::TableSetupColumn("##pergame_right_col",
                                          ImGuiTableColumnFlags_WidthStretch);
                  ImGui::TableNextColumn();
                  if (per_game_config_popup_focus_requested_ ||
                      per_game_config_focus_jump_request_ ==
                          PerGameConfigFocusSide::kLeft) {
                    ImGui::SetKeyboardFocusHere();
                    if (per_game_config_focus_jump_request_ ==
                        PerGameConfigFocusSide::kLeft) {
                      per_game_config_focus_jump_request_ =
                          PerGameConfigFocusSide::kNone;
                    }
                    per_game_config_popup_focus_requested_ = false;
                  }
                  ImGui::PushStyleColor(ImGuiCol_Text,
                                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                  ImGui::PushStyleColor(ImGuiCol_Button,
                                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                  ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                  ImGui::PushStyleColor(ImGuiCol_NavCursor,
                                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                  focus_if_requested("left_add_button");
                  bool add_button_pressed =
                      ImGui::Button("+##pergame_add_btn", ImVec2(28.0f, 0.0f));
                  bool add_button_highlighted = ImGui::IsItemHovered() ||
                                                ImGui::IsItemActive() ||
                                                ImGui::IsItemFocused();
                  ImVec2 add_button_min = ImGui::GetItemRectMin();
                  ImVec2 add_button_max = ImGui::GetItemRectMax();
                  ImGui::PopStyleColor(5);
                  if (add_button_highlighted) {
                    const ImVec4 button_border =
                        GetConfiguredUIAccentColor(0.95f);
                    ImGui::GetWindowDrawList()->AddRect(
                        add_button_min, add_button_max,
                        ImGui::GetColorU32(button_border), 4.0f, 0,
                        std::max(1.5f, 2.0f * display_scale));
                  }
                  ImVec2 add_text_size = ImGui::CalcTextSize("+");
                  ImVec2 add_text_pos(
                      add_button_min.x + (add_button_max.x - add_button_min.x -
                                          add_text_size.x) *
                                             0.5f,
                      add_button_min.y + (add_button_max.y - add_button_min.y -
                                          add_text_size.y) *
                                             0.5f);
                  DrawTextWithConfiguredEffect(
                      ImGui::GetWindowDrawList(), ImGui::GetFont(),
                      ImGui::GetFontSize(), add_text_pos,
                      IM_COL32(255, 255, 255, 255), "+");
                  if (add_button_pressed &&
                      !per_game_config_add_candidate_.empty()) {
                    add_or_update_per_game_entry(per_game_config_add_candidate_,
                                                 "");
                    selected_per_game_config_option_ =
                        per_game_config_add_candidate_;
                    per_game_config_remove_mode_ = false;
                    per_game_config_add_preview_option_ =
                        per_game_config_add_candidate_;
                  }
                  track_per_game_focus(PerGameConfigFocusSide::kLeft,
                                       "left_add_button", false, true);

                  ImGui::SameLine();
                  std::string add_label = per_game_config_add_candidate_.empty()
                                              ? "No more options"
                                              : per_game_config_add_candidate_;
                  const float add_combo_width = ImGui::CalcItemWidth() * 1.3f;
                  ImGui::SetNextItemWidth(add_combo_width);
                  ImGui::PushStyleColor(ImGuiCol_Text,
                                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                  focus_if_requested("left_add_combo");
                  const bool add_combo_open =
                      ImGui::BeginCombo("##pergame_add", add_label.c_str());
                  if (add_combo_open) {
                    for (const auto* option : addable_options) {
                      bool selected =
                          per_game_config_add_candidate_ == option->name;
                      std::string option_label = fmt::format(
                          "[{}] {}", option->category, option->name);
                      ImGui::PushStyleColor(ImGuiCol_Header,
                                            ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                      ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                            ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                      ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                            ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                      bool item_clicked = ImGui::Selectable(
                          ("##" + option_label).c_str(), selected);
                      bool item_highlighted =
                          ImGui::IsItemHovered() || ImGui::IsItemActive();
                      ImVec2 item_min = ImGui::GetItemRectMin();
                      ImVec2 item_max = ImGui::GetItemRectMax();
                      ImGui::PopStyleColor(3);
                      if (item_highlighted || selected) {
                        const ImVec4 item_border = GetConfiguredUIAccentColor(
                            selected ? 0.95f : 0.75f);
                        ImGui::GetWindowDrawList()->AddRect(
                            item_min, item_max, ImGui::GetColorU32(item_border),
                            4.0f, 0, 1.0f);
                      }
                      DrawTextWithConfiguredEffect(
                          ImGui::GetWindowDrawList(), ImGui::GetFont(),
                          ImGui::GetFontSize(),
                          ImVec2(item_min.x + ImGui::GetStyle().FramePadding.x,
                                 item_min.y + ImGui::GetStyle().FramePadding.y),
                          IM_COL32(255, 255, 255, 255), option_label.c_str());
                      if (item_highlighted) {
                        per_game_config_add_preview_option_ = option->name;
                      }
                      if (item_clicked) {
                        per_game_config_add_candidate_ = option->name;
                        per_game_config_add_preview_option_ = option->name;
                      }
                      if (selected) {
                        ImGui::SetItemDefaultFocus();
                      }
                    }
                    ImGui::EndCombo();
                  }
                  ImGui::PopStyleColor();

                  const ImVec2 add_combo_min = ImGui::GetItemRectMin();
                  const ImVec2 add_combo_max = ImGui::GetItemRectMax();
                  const bool add_combo_highlighted =
                      ImGui::IsItemHovered() || ImGui::IsItemActive() ||
                      ImGui::IsItemFocused() || add_combo_open;
                  if (add_combo_highlighted) {
                    const ImVec4 combo_border =
                        GetConfiguredUIAccentColor(0.95f);
                    ImGui::GetWindowDrawList()->AddRect(
                        add_combo_min, add_combo_max,
                        ImGui::GetColorU32(combo_border),
                        ImGui::GetStyle().FrameRounding, 0,
                        std::max(1.5f, 2.0f * display_scale));
                  }
                  const ImVec2 add_combo_text_pos(
                      add_combo_min.x + ImGui::GetStyle().FramePadding.x,
                      add_combo_min.y + (add_combo_max.y - add_combo_min.y -
                                         ImGui::GetTextLineHeight()) *
                                            0.5f);
                  DrawTextWithConfiguredEffect(
                      ImGui::GetWindowDrawList(), ImGui::GetFont(),
                      ImGui::GetFontSize(), add_combo_text_pos,
                      ImGui::GetColorU32(ImGuiCol_Text), add_label.c_str());
                  const float add_combo_arrow_size = std::max(
                      5.0f * display_scale, ImGui::GetFontSize() * 0.45f);
                  const float add_combo_arrow_center_x =
                      add_combo_max.x - ImGui::GetStyle().FramePadding.x -
                      add_combo_arrow_size;
                  const float add_combo_arrow_center_y =
                      add_combo_min.y +
                      (add_combo_max.y - add_combo_min.y) * 0.5f +
                      (1.0f * display_scale);
                  const ImU32 add_combo_arrow_color =
                      UseDarkAccentComboText()
                          ? IM_COL32(0, 0, 0, 255)
                          : ImGui::GetColorU32(ImGuiCol_Text);
                  ImGui::GetWindowDrawList()->AddTriangleFilled(
                      ImVec2(add_combo_arrow_center_x - add_combo_arrow_size,
                             add_combo_arrow_center_y -
                                 add_combo_arrow_size * 0.5f),
                      ImVec2(add_combo_arrow_center_x + add_combo_arrow_size,
                             add_combo_arrow_center_y -
                                 add_combo_arrow_size * 0.5f),
                      ImVec2(add_combo_arrow_center_x,
                             add_combo_arrow_center_y +
                                 add_combo_arrow_size * 0.5f),
                      add_combo_arrow_color);
                  track_per_game_focus(PerGameConfigFocusSide::kLeft,
                                       "left_add_combo", true, true);

                  ImGui::SameLine();
                  if (per_game_config_remove_mode_) {
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(0.80f, 0.20f, 0.20f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImVec4(0.90f, 0.28f, 0.28f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                          ImVec4(0.95f, 0.35f, 0.35f, 1.00f));
                  }
                  ImGui::PushStyleColor(ImGuiCol_Text,
                                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                  ImGui::PushStyleColor(ImGuiCol_Button,
                                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                  ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                  ImGui::PushStyleColor(ImGuiCol_NavCursor,
                                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                  focus_if_requested("left_remove_mode_button");
                  bool remove_button_pressed = ImGui::Button(
                      "-##pergame_remove_btn", ImVec2(28.0f, 0.0f));
                  bool remove_button_highlighted = ImGui::IsItemHovered() ||
                                                   ImGui::IsItemActive() ||
                                                   ImGui::IsItemFocused();
                  ImVec2 remove_button_min = ImGui::GetItemRectMin();
                  ImVec2 remove_button_max = ImGui::GetItemRectMax();
                  ImGui::PopStyleColor(5);
                  if (remove_button_highlighted) {
                    const ImVec4 button_border =
                        GetConfiguredUIAccentColor(0.95f);
                    ImGui::GetWindowDrawList()->AddRect(
                        remove_button_min, remove_button_max,
                        ImGui::GetColorU32(button_border), 4.0f, 0,
                        std::max(1.5f, 2.0f * display_scale));
                  }
                  ImVec2 remove_text_size = ImGui::CalcTextSize("-");
                  ImVec2 remove_text_pos(
                      remove_button_min.x +
                          (remove_button_max.x - remove_button_min.x -
                           remove_text_size.x) *
                              0.5f,
                      remove_button_min.y +
                          (remove_button_max.y - remove_button_min.y -
                           remove_text_size.y) *
                              0.5f);
                  DrawTextWithConfiguredEffect(
                      ImGui::GetWindowDrawList(), ImGui::GetFont(),
                      ImGui::GetFontSize(), remove_text_pos,
                      IM_COL32(255, 255, 255, 255), "-");
                  if (remove_button_pressed) {
                    per_game_config_remove_mode_ =
                        !per_game_config_remove_mode_;
                  }
                  track_per_game_focus(PerGameConfigFocusSide::kLeft,
                                       "left_remove_mode_button", true, false);
                  if (per_game_config_remove_mode_) {
                    ImGui::PopStyleColor(3);
                  }

                  const std::string pergame_list_header = fmt::format(
                      "Configured options ({}){}",
                      per_game_config_entries_.size(),
                      per_game_config_remove_mode_ ? " - remove mode active"
                                                   : "");
                  const ImVec2 pergame_list_header_pos =
                      ImGui::GetCursorScreenPos();
                  DrawTextWithConfiguredEffect(
                      ImGui::GetWindowDrawList(), ImGui::GetFont(),
                      ImGui::GetFontSize(), pergame_list_header_pos,
                      ImGui::GetColorU32(ImGuiCol_Text),
                      pergame_list_header.c_str());
                  const ImVec2 pergame_list_header_size =
                      ImGui::CalcTextSize(pergame_list_header.c_str());
                  ImGui::Dummy(pergame_list_header_size);

                  std::vector<std::string> configured_option_names;
                  configured_option_names.reserve(
                      per_game_config_entries_.size());
                  for (const auto& entry : per_game_config_entries_) {
                    configured_option_names.push_back(entry.first);
                  }
                  std::sort(configured_option_names.begin(),
                            configured_option_names.end());
                  if (!configured_option_names.empty()) {
                    per_game_has_configured_options = true;
                    per_game_first_config_entry_control_id =
                        fmt::format("left_config_entry_{}",
                                    configured_option_names.front());
                    per_game_last_config_entry_control_id = fmt::format(
                        "left_config_entry_{}", configured_option_names.back());
                  }

                  for (const auto& option_name : configured_option_names) {
                    bool is_selected =
                        selected_per_game_config_option_ == option_name;
                    std::string entry_control_id =
                        fmt::format("left_config_entry_{}", option_name);
                    std::string label = fmt::format("{}##pergame_entry_{}",
                                                    option_name, option_name);
                    const ImGuiStyle& selectable_style = ImGui::GetStyle();
                    ImGui::PushStyleColor(ImGuiCol_Header,
                                          ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                          ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                          ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                    ImGui::PushStyleColor(ImGuiCol_NavCursor,
                                          ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                    focus_if_requested(entry_control_id);
                    bool clicked =
                        ImGui::Selectable(label.c_str(), is_selected);
                    bool highlighted = ImGui::IsItemHovered() ||
                                       ImGui::IsItemActive() ||
                                       ImGui::IsItemFocused();
                    ImVec2 item_min = ImGui::GetItemRectMin();
                    ImVec2 item_max = ImGui::GetItemRectMax();
                    ImGui::PopStyleColor(5);
                    const float border_thickness =
                        std::max(1.0f, 2.0f * display_scale);
                    if (highlighted || is_selected) {
                      const ImVec4 entry_border = GetConfiguredUIAccentColor(
                          is_selected ? 1.0f : 0.75f);
                      const float border_inset_x = std::max(
                          border_thickness, selectable_style.FramePadding.x);
                      const float border_inset_y = border_thickness * 0.5f;
                      ImGui::GetWindowDrawList()->AddRect(
                          ImVec2(item_min.x + border_inset_x,
                                 item_min.y + border_inset_y),
                          ImVec2(item_max.x - border_inset_x,
                                 item_max.y - border_inset_y),
                          ImGui::GetColorU32(entry_border), 4.0f, 0,
                          border_thickness);
                    }
                    const float line_height = ImGui::GetTextLineHeight();
                    const ImVec2 text_pos(
                        item_min.x + selectable_style.FramePadding.x,
                        item_min.y +
                            (item_max.y - item_min.y - line_height) * 0.5f);
                    DrawTextWithConfiguredEffect(
                        ImGui::GetWindowDrawList(), ImGui::GetFont(),
                        ImGui::GetFontSize(), text_pos,
                        ImGui::GetColorU32(ImGuiCol_Text), option_name.c_str());
                    bool removed_option = false;
                    if (clicked) {
                      if (per_game_config_remove_mode_) {
                        per_game_config_entries_.erase(option_name);
                        removed_option = true;
                        if (selected_per_game_config_option_ == option_name) {
                          selected_per_game_config_option_.clear();
                        }
                      } else {
                        selected_per_game_config_option_ = option_name;
                      }
                    }
                    if (!removed_option && ImGui::IsItemFocused() &&
                        selected_per_game_config_option_ != option_name) {
                      selected_per_game_config_option_ = option_name;
                      per_game_config_add_preview_option_.clear();
                    }
                    track_per_game_focus(PerGameConfigFocusSide::kLeft,
                                         entry_control_id, false, false);
                  }
                  ImGui::TableNextColumn();
                  ImGui::TableNextColumn();

                  const bool right_column_has_selection =
                      !per_game_config_add_preview_option_.empty() ||
                      (!selected_per_game_config_option_.empty() &&
                       per_game_config_entries_.find(
                           selected_per_game_config_option_) !=
                           per_game_config_entries_.end());
                  if (per_game_config_focus_jump_request_ ==
                          PerGameConfigFocusSide::kRight &&
                      right_column_has_selection) {
                    ImGui::SetKeyboardFocusHere();
                    per_game_config_focus_jump_request_ =
                        PerGameConfigFocusSide::kNone;
                  }

                  const bool has_selected_entry =
                      !selected_per_game_config_option_.empty() &&
                      per_game_config_entries_.find(
                          selected_per_game_config_option_) !=
                          per_game_config_entries_.end();
                  const PerGameConfigOption* add_preview_option =
                      per_game_config_add_preview_option_.empty()
                          ? nullptr
                          : find_per_game_option(
                                per_game_config_add_preview_option_);
                  const bool add_preview_has_priority =
                      add_preview_option &&
                      (add_combo_open ||
                       per_game_focus_control_id == "left_add_combo");
                  const PerGameConfigOption* active_pergame_option = nullptr;
                  std::string active_pergame_value;
                  if (add_preview_has_priority) {
                    active_pergame_option = add_preview_option;
                    active_pergame_value = add_preview_option->default_value;
                  } else if (has_selected_entry) {
                    active_pergame_option =
                        find_per_game_option(selected_per_game_config_option_);
                    active_pergame_value =
                        per_game_config_entries_
                            .find(selected_per_game_config_option_)
                            ->second.value;
                  }
                  auto apply_per_game_value_change =
                      [this, &active_pergame_option, &active_pergame_value,
                       &add_or_update_per_game_entry](
                          const std::string& next_value) {
                        if (!active_pergame_option) {
                          return;
                        }
                        add_or_update_per_game_entry(
                            active_pergame_option->name, next_value);
                        selected_per_game_config_option_ =
                            active_pergame_option->name;
                        active_pergame_value = next_value;
                      };

                  auto draw_wrapped_pergame_right_text =
                      [&footer_back_icon_right_x](const std::string& text) {
                        const ImVec2 text_pos = ImGui::GetCursorScreenPos();
                        const float wrap_width = std::max(
                            0.0f, footer_back_icon_right_x - text_pos.x);
                        DrawTextWithConfiguredEffect(
                            ImGui::GetWindowDrawList(), ImGui::GetFont(),
                            ImGui::GetFontSize(), text_pos,
                            ImGui::GetColorU32(ImGuiCol_Text), text.c_str(),
                            wrap_width);
                        const ImVec2 text_size = ImGui::CalcTextSize(
                            text.c_str(), nullptr, false, wrap_width);
                        ImGui::Dummy(text_size);
                      };

                  if (!active_pergame_option) {
                    const std::string pergame_placeholder_text =
                        "Select a configured option to edit its value.";
                    draw_wrapped_pergame_right_text(pergame_placeholder_text);
                  } else {
                    auto* option = active_pergame_option;

                    if (option) {
                      const std::string pergame_category_text =
                          fmt::format("Category: {}", option->category);
                      draw_wrapped_pergame_right_text(pergame_category_text);
                      if (!option->description.empty()) {
                        ImGui::Spacing();
                        draw_wrapped_pergame_right_text(option->description);
                      }
                    }

                    ImGui::Spacing();
                    const std::string pergame_value_text =
                        fmt::format("Current value: {}", active_pergame_value);
                    draw_wrapped_pergame_right_text(pergame_value_text);

                    if (option && !option->enum_values.empty()) {
                      ImGui::Spacing();
                      const std::string pergame_choose_text = "Choose value:";
                      draw_wrapped_pergame_right_text(pergame_choose_text);
                      for (const auto& enum_value_raw : option->enum_values) {
                        std::string normalized = canonicalize_per_game_value(
                            *option, enum_value_raw);
                        std::string display = enum_value_raw.empty()
                                                  ? "(empty/default)"
                                                  : enum_value_raw;
                        if (option->name == "user_language") {
                          if (normalized == "1") {
                            display = "English (1)";
                          } else if (normalized == "2") {
                            display = "Japanese (2)";
                          } else if (normalized == "3") {
                            display = "German (3)";
                          } else if (normalized == "4") {
                            display = "French (4)";
                          } else if (normalized == "5") {
                            display = "Spanish (5)";
                          } else if (normalized == "6") {
                            display = "Italian (6)";
                          } else if (normalized == "7") {
                            display = "Korean (7)";
                          } else if (normalized == "8") {
                            display = "Traditional Chinese (8)";
                          } else if (normalized == "9") {
                            display = "Portuguese (9)";
                          } else if (normalized == "10") {
                            display = "Simplified Chinese (10)";
                          } else if (normalized == "11") {
                            display = "Polish (11)";
                          } else if (normalized == "12") {
                            display = "Russian (12)";
                          }
                        }
                        bool selected = active_pergame_value == normalized;
                        std::string button_label =
                            fmt::format("{}##enum_{}_{}", display, option->name,
                                        normalized);
                        ImGui::PushStyleColor(ImGuiCol_Header,
                                              ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                              ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                              ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                        ImGui::PushStyleColor(ImGuiCol_NavCursor,
                                              ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                        bool clicked =
                            ImGui::Selectable(button_label.c_str(), selected);
                        bool highlighted = ImGui::IsItemHovered() ||
                                           ImGui::IsItemActive() ||
                                           ImGui::IsItemFocused();
                        ImVec2 item_min = ImGui::GetItemRectMin();
                        ImVec2 item_max = ImGui::GetItemRectMax();
                        ImGui::PopStyleColor(5);
                        const float entry_border_thickness =
                            std::max(1.0f, 2.0f * display_scale);
                        if (highlighted || selected) {
                          const ImVec4 entry_border =
                              GetConfiguredUIAccentColor(selected ? 1.0f
                                                                  : 0.75f);
                          ImGui::GetWindowDrawList()->AddRect(
                              item_min, item_max,
                              ImGui::GetColorU32(entry_border), 4.0f, 0,
                              entry_border_thickness);
                        }
                        const float line_height = ImGui::GetTextLineHeight();
                        const ImVec2 text_pos(
                            item_min.x + ImGui::GetStyle().FramePadding.x,
                            item_min.y +
                                (item_max.y - item_min.y - line_height) * 0.5f);
                        DrawTextWithConfiguredEffect(
                            ImGui::GetWindowDrawList(), ImGui::GetFont(),
                            ImGui::GetFontSize(), text_pos,
                            ImGui::GetColorU32(ImGuiCol_Text), display.c_str());
                        if (clicked) {
                          apply_per_game_value_change(normalized);
                        }
                        track_per_game_focus(
                            PerGameConfigFocusSide::kRight,
                            fmt::format("right_enum_{}_{}", option->name,
                                        normalized),
                            false, false);
                      }
                    } else if (option && option->value_type ==
                                             PerGameConfigValueType::kBool) {
                      bool bool_value = (active_pergame_value == "true");
                      if (ImGui::Button(bool_value ? "Set false"
                                                   : "Set true")) {
                        std::string next_value = bool_value ? "false" : "true";
                        apply_per_game_value_change(next_value);
                      }
                      track_per_game_focus(PerGameConfigFocusSide::kRight,
                                           "right_bool_toggle", false, false);
                    } else if (option &&
                               (option->value_type ==
                                    PerGameConfigValueType::kInt ||
                                option->value_type ==
                                    PerGameConfigValueType::kUInt ||
                                option->value_type ==
                                    PerGameConfigValueType::kUInt64)) {
                      int64_t current_value = 0;
                      try {
                        current_value = std::stoll(active_pergame_value);
                      } catch (...) {
                        current_value = 0;
                      }
                      auto apply_int_delta = [option, &current_value,
                                              &apply_per_game_value_change](
                                                 int delta) {
                        int64_t next = current_value + delta;
                        if ((option->value_type ==
                                 PerGameConfigValueType::kUInt ||
                             option->value_type ==
                                 PerGameConfigValueType::kUInt64) &&
                            next < 0) {
                          next = 0;
                        }
                        apply_per_game_value_change(fmt::format("{}", next));
                        current_value = next;
                      };

                      if (ImGui::Button("-10", ImVec2(58.0f, 0.0f))) {
                        apply_int_delta(-10);
                      }
                      track_per_game_focus(PerGameConfigFocusSide::kRight,
                                           fmt::format("right_int_delta_{}_{}",
                                                       option->name, -10),
                                           false, true);

                      ImGui::SameLine();
                      if (ImGui::Button("-1", ImVec2(58.0f, 0.0f))) {
                        apply_int_delta(-1);
                      }
                      track_per_game_focus(PerGameConfigFocusSide::kRight,
                                           fmt::format("right_int_delta_{}_{}",
                                                       option->name, -1),
                                           true, true);

                      ImGui::SameLine();
                      const std::string pergame_int_value_text = fmt::format(
                          "{}", static_cast<long long>(current_value));
                      const ImVec2 pergame_int_value_pos =
                          ImGui::GetCursorScreenPos();
                      DrawTextWithConfiguredEffect(
                          ImGui::GetWindowDrawList(), ImGui::GetFont(),
                          ImGui::GetFontSize(), pergame_int_value_pos,
                          ImGui::GetColorU32(ImGuiCol_Text),
                          pergame_int_value_text.c_str());
                      const ImVec2 pergame_int_value_size =
                          ImGui::CalcTextSize(pergame_int_value_text.c_str());
                      ImGui::Dummy(ImVec2(pergame_int_value_size.x, 0.0f));

                      ImGui::SameLine();
                      if (ImGui::Button("+1", ImVec2(58.0f, 0.0f))) {
                        apply_int_delta(1);
                      }
                      track_per_game_focus(
                          PerGameConfigFocusSide::kRight,
                          fmt::format("right_int_delta_{}_{}", option->name, 1),
                          true, true);

                      ImGui::SameLine();
                      if (ImGui::Button("+10", ImVec2(58.0f, 0.0f))) {
                        apply_int_delta(10);
                      }
                      track_per_game_focus(PerGameConfigFocusSide::kRight,
                                           fmt::format("right_int_delta_{}_{}",
                                                       option->name, 10),
                                           true, false);
                    } else if (option && option->value_type ==
                                             PerGameConfigValueType::kDouble) {
                      double current_value = 0.0;
                      try {
                        current_value = std::stod(active_pergame_value);
                      } catch (...) {
                        current_value = 0.0;
                      }
                      const double deltas[] = {-1.0, -0.1, 0.1, 1.0};
                      for (size_t i = 0; i < std::size(deltas); ++i) {
                        double delta = deltas[i];
                        std::string label = fmt::format("{:+.1f}", delta);
                        if (ImGui::Button(label.c_str(), ImVec2(58.0f, 0.0f))) {
                          double next = current_value + delta;
                          apply_per_game_value_change(fmt::format("{}", next));
                          current_value = next;
                        }
                        track_per_game_focus(
                            PerGameConfigFocusSide::kRight,
                            fmt::format("right_double_delta_{}_{}",
                                        option->name, i),
                            i != 0, false);
                        if (i + 1 != std::size(deltas)) {
                          ImGui::SameLine();
                        }
                      }
                    } else {
                      const std::string pergame_fallback_text =
                          "No controller preset editor for this type yet. Use "
                          "default/reset or known enums.";
                      draw_wrapped_pergame_right_text(pergame_fallback_text);
                      if (option &&
                          (option->value_type ==
                               PerGameConfigValueType::kString ||
                           option->value_type ==
                               PerGameConfigValueType::kPath) &&
                          ImGui::Button("Set Empty")) {
                        apply_per_game_value_change("\"\"");
                      }
                      track_per_game_focus(PerGameConfigFocusSide::kRight,
                                           "right_set_empty", false, false);
                    }

                    if (option) {
                      ImGui::Spacing();
                      if (ImGui::Button("Reset To Default")) {
                        apply_per_game_value_change(option->default_value);
                      }
                      track_per_game_focus(PerGameConfigFocusSide::kRight,
                                           "right_reset_default", false, false);
                    }
                  }

                  if (per_game_nav_right_pressed &&
                      per_game_focus_side == PerGameConfigFocusSide::kLeft &&
                      !per_game_focus_has_right_neighbor &&
                      !per_game_focus_control_id.empty() &&
                      per_game_focus_control_id ==
                          previous_per_game_focus_control &&
                      right_column_has_selection) {
                    per_game_config_focus_jump_request_ =
                        PerGameConfigFocusSide::kRight;
                  } else if (per_game_nav_left_pressed &&
                             per_game_focus_side ==
                                 PerGameConfigFocusSide::kRight &&
                             !per_game_focus_has_left_neighbor &&
                             !per_game_focus_control_id.empty() &&
                             per_game_focus_control_id ==
                                 previous_per_game_focus_control) {
                    per_game_config_focus_jump_request_ =
                        PerGameConfigFocusSide::kLeft;
                  }

                  const bool focus_on_top_controls =
                      per_game_focus_control_id == "left_add_button" ||
                      per_game_focus_control_id == "left_add_combo" ||
                      per_game_focus_control_id == "left_remove_mode_button";
                  const bool focus_on_first_config_entry =
                      per_game_has_configured_options &&
                      per_game_focus_control_id ==
                          per_game_first_config_entry_control_id;
                  const bool focus_on_last_config_entry =
                      per_game_has_configured_options &&
                      per_game_focus_control_id ==
                          per_game_last_config_entry_control_id;

                  if (per_game_nav_down_pressed) {
                    if (focus_on_top_controls) {
                      if (per_game_has_configured_options) {
                        per_game_config_control_jump_request_ =
                            per_game_first_config_entry_control_id;
                      } else {
                        per_game_config_control_jump_request_ =
                            "left_apply_button";
                      }
                    } else if (focus_on_last_config_entry) {
                      per_game_config_control_jump_request_ =
                          "left_apply_button";
                    }
                  }

                  if (per_game_nav_up_pressed && focus_on_first_config_entry) {
                    per_game_config_control_jump_request_ = "left_add_combo";
                  }

                  per_game_config_last_focused_control_id_ =
                      per_game_focus_control_id;
                  ImGui::EndTable();
                }

                ImGui::EndChild();
              }

              ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));

              ImGui::PushStyleColor(ImGuiCol_Text,
                                    ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
              ImGui::PushStyleColor(ImGuiCol_Button,
                                    ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
              ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                    ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
              ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                    ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
              ImGui::PushStyleColor(ImGuiCol_NavCursor,
                                    ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
              focus_if_requested("left_apply_button");
              bool apply_button_pressed = ImGui::Button("Apply##pergame_apply");
              bool apply_button_highlighted = ImGui::IsItemHovered() ||
                                              ImGui::IsItemActive() ||
                                              ImGui::IsItemFocused();
              ImVec2 apply_button_min = ImGui::GetItemRectMin();
              ImVec2 apply_button_max = ImGui::GetItemRectMax();
              ImGui::PopStyleColor(5);
              if (apply_button_highlighted) {
                const ImVec4 button_border = GetConfiguredUIAccentColor(0.95f);
                ImGui::GetWindowDrawList()->AddRect(
                    apply_button_min, apply_button_max,
                    ImGui::GetColorU32(button_border), 8.0f * display_scale, 0,
                    std::max(1.5f, 2.0f * display_scale));
              }
              ImVec2 apply_text_size = ImGui::CalcTextSize("Apply");
              ImVec2 apply_text_pos(apply_button_min.x + (apply_button_max.x -
                                                          apply_button_min.x -
                                                          apply_text_size.x) *
                                                             0.5f,
                                    apply_button_min.y + (apply_button_max.y -
                                                          apply_button_min.y -
                                                          apply_text_size.y) *
                                                             0.5f);
              DrawTextWithConfiguredEffect(
                  ImGui::GetWindowDrawList(), ImGui::GetFont(),
                  ImGui::GetFontSize(), apply_text_pos,
                  IM_COL32(255, 255, 255, 255), "Apply");
              track_per_game_focus(PerGameConfigFocusSide::kLeft,
                                   "left_apply_button", false, true);
              if (apply_button_pressed) {
                std::error_code mkdir_ec;
                std::filesystem::create_directories(
                    pending_per_game_config_path_.parent_path(), mkdir_ec);
                if (mkdir_ec) {
                  action_status_ =
                      fmt::format("Failed to create config directory: {}",
                                  mkdir_ec.message());
                  action_popup_mode_ = ActionPopupMode::kInfo;
                  show_action_status_ = true;
                } else {
                  std::ofstream ofs(pending_per_game_config_path_,
                                    std::ios::binary);
                  if (!ofs.is_open()) {
                    action_status_ =
                        "Failed to write per-game config file (open failed).";
                  } else {
                    std::string generated_config = build_per_game_config_text();
                    std::memset(per_game_config_buffer_, 0,
                                sizeof(per_game_config_buffer_));
                    std::strncpy(per_game_config_buffer_,
                                 generated_config.c_str(),
                                 sizeof(per_game_config_buffer_) - 1);
                    ofs.write(generated_config.data(), generated_config.size());
                    ofs.close();

                    if (apply_per_game_config(pending_per_game_config_title_id_,
                                              pending_per_game_config_path_)) {
                      action_status_ = fmt::format(
                          "Per-game config saved and applied for {}.",
                          pending_per_game_config_title_id_);
                    } else {
                      action_status_ =
                          "Per-game config saved, but apply failed (check "
                          "xenia.log).";
                    }
                  }
                  action_popup_mode_ = ActionPopupMode::kInfo;
                  show_action_status_ = true;
                }
                close_per_game_config_editor = true;
              }

              ImGui::SameLine();
              ImGui::PushStyleColor(ImGuiCol_Text,
                                    ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
              ImGui::PushStyleColor(ImGuiCol_Button,
                                    ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
              ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                    ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
              ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                    ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
              ImGui::PushStyleColor(ImGuiCol_NavCursor,
                                    ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
              focus_if_requested("left_cancel_button");
              bool cancel_button_pressed =
                  ImGui::Button("Cancel##pergame_cancel");
              bool cancel_button_highlighted = ImGui::IsItemHovered() ||
                                               ImGui::IsItemActive() ||
                                               ImGui::IsItemFocused();
              ImVec2 cancel_button_min = ImGui::GetItemRectMin();
              ImVec2 cancel_button_max = ImGui::GetItemRectMax();
              ImGui::PopStyleColor(5);
              if (cancel_button_highlighted) {
                const ImVec4 button_border = GetConfiguredUIAccentColor(0.95f);
                ImGui::GetWindowDrawList()->AddRect(
                    cancel_button_min, cancel_button_max,
                    ImGui::GetColorU32(button_border), 8.0f * display_scale, 0,
                    std::max(1.5f, 2.0f * display_scale));
              }
              ImVec2 cancel_text_size = ImGui::CalcTextSize("Cancel");
              ImVec2 cancel_text_pos(
                  cancel_button_min.x +
                      (cancel_button_max.x - cancel_button_min.x -
                       cancel_text_size.x) *
                          0.5f,
                  cancel_button_min.y +
                      (cancel_button_max.y - cancel_button_min.y -
                       cancel_text_size.y) *
                          0.5f);
              DrawTextWithConfiguredEffect(
                  ImGui::GetWindowDrawList(), ImGui::GetFont(),
                  ImGui::GetFontSize(), cancel_text_pos,
                  IM_COL32(255, 255, 255, 255), "Cancel");
              track_per_game_focus(PerGameConfigFocusSide::kLeft,
                                   "left_cancel_button", true, false);
              if (cancel_button_pressed) {
                close_per_game_config_editor = true;
              }

              const bool focus_on_footer_controls =
                  per_game_focus_control_id == "left_apply_button" ||
                  per_game_focus_control_id == "left_cancel_button";
              if (per_game_nav_up_pressed && focus_on_footer_controls) {
                if (per_game_has_configured_options) {
                  per_game_config_control_jump_request_ =
                      per_game_last_config_entry_control_id;
                } else {
                  per_game_config_control_jump_request_ = "left_add_combo";
                }
              } else if (per_game_nav_down_pressed &&
                         focus_on_footer_controls) {
                per_game_config_control_jump_request_ =
                    per_game_focus_control_id;
              }

              const float footer_text_size = 13.5f * uy;
              const float footer_icon_size = 15.6f * uy;
              const float footer_spacing_y = 17.0f * uy;
              const float footer_select_y = pergame_window_max.y - (32.0f * uy);
              const float footer_back_y = footer_select_y - footer_spacing_y;
              const float footer_base_x =
                  pergame_window_pos.x + panel_size.x * 0.9f;
              const float footer_back_text_x = footer_base_x - (70.0f * ux);
              const float footer_back_icon_offset = 35.0f * ux;
              const float footer_select_text_x = footer_base_x - (60.0f * ux);
              const float footer_select_icon_offset = 42.0f * ux;
              xe::app::DrawFooterPrompt(
                  pergame_draw_list, GetOrCreateButtonTexture('B'),
                  footer_text_size, footer_icon_size, "Back", footer_back_y,
                  footer_back_text_x, footer_back_icon_offset);
              xe::app::DrawFooterPrompt(
                  pergame_draw_list, GetOrCreateButtonTexture('A'),
                  footer_text_size, footer_icon_size, "Select", footer_select_y,
                  footer_select_text_x, footer_select_icon_offset);
            }

            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);
            if (close_per_game_config_editor) {
              show_per_game_config_editor_ = false;
              per_game_config_popup_focus_requested_ = false;
              per_game_config_focus_jump_request_ =
                  PerGameConfigFocusSide::kNone;
              per_game_config_control_jump_request_.clear();
              gamelist_window_focus_requested = true;
              gamelist_focus_selected_request = true;
            }
          }

          if (request_launch_selected_game && !launch_game_path.empty()) {
            launch_after_draw = true;
            launch_path_after_draw = std::filesystem::u8path(launch_game_path);
          }

          gamelist_x_was_down = controller_x_down;
          gamelist_b_was_down = controller_b_down;
        }
        if (!game_list_tab_open) {
          gamelist_x_was_down = false;
          gamelist_b_was_down = false;
          show_search_panel_ = false;
          search_panel_focus_input_requested_ = false;
          search_panel_list_focus_requested_ = false;
          search_gamepad_y_was_down_ = false;
        }
        game_list_tab_was_open = game_list_tab_open;
        settings_tab_was_open = settings_tab_open;
        paths_tab_was_open = paths_tab_open;
        about_tab_was_open = about_tab_open;

        if (active_frontend_page_ == FrontendPage::kSettings) {
          StopVideoBackground();
          std::string tooltip =
              "Note: Xenia-Canary is a WIP emulator.\nMost settings should be "
              "considered hacks, and may cause (or fix) crashes and other "
              "problems. Edit these settings at your own risk.";

          static const char* k_settings_sections[] = {
              "General", "D3D12",        "Display", "GPU", "Audio",
              "Memory",  "Storage",      "UI",      "x64", "Video",
              "HID",     "Command Line", "Netplay", "DSU / Streaming",
              "ScreenScraper",
          };

          if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
              key_pressed(ImGuiKey_GamepadFaceRight, false)) {
            settings_focus_requested = true;
          }

          const bool settings_nav_left_pressed =
              key_pressed(ImGuiKey_GamepadDpadLeft, false) ||
              key_pressed(ImGuiKey_LeftArrow, false);
          if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
              settings_focus_on_right && settings_nav_left_pressed &&
              !ImGui::IsAnyItemActive()) {  // Don't switch left if any item
                                            // (like slider) is active
            settings_focus_requested = true;
            settings_focus_right_requested = false;
            settings_focus_on_right = false;
          }

          const float settings_plane_margin = 40.0f;
          ImVec2 settings_plane_origin = ImGui::GetCursorPos();
          ImVec2 settings_plane_avail = ImGui::GetContentRegionAvail();
          ImVec2 settings_plane_size(
              std::max(0.0f,
                       settings_plane_avail.x - (settings_plane_margin * 2.0f)),
              std::max(0.0f, settings_plane_avail.y -
                                 (settings_plane_margin * 2.0f)));
          ImGui::SetCursorPos(
              ImVec2(settings_plane_origin.x + settings_plane_margin,
                     settings_plane_origin.y + settings_plane_margin));
          ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
          if (ImGui::BeginChild("##settings_editor_plane", settings_plane_size,
                                false)) {
            const float settings_content_offset_x = 15.0f * display_scale;
            ImGui::Indent(settings_content_offset_x);
            if (settings_focus_requested) {
              ImGui::SetWindowFocus();
            }
            if (ImGui::BeginTable("##settings_layout", 3,
                                  ImGuiTableFlags_SizingStretchProp)) {
              ImGui::TableSetupColumn("##settings_left_col",
                                      ImGuiTableColumnFlags_WidthFixed, 370.0f);
              ImGui::TableSetupColumn("##settings_gap_col",
                                      ImGuiTableColumnFlags_WidthFixed |
                                          ImGuiTableColumnFlags_NoResize,
                                      86.0f);
              ImGui::TableSetupColumn("##settings_right_col",
                                      ImGuiTableColumnFlags_WidthStretch, 1.0f);

              ImGui::TableNextColumn();
              ImGui::TextWrapped("Available settings");
              bool settings_list_has_focus = false;
              bool settings_left_focus_detected = false;
              const bool disable_left_nav = settings_focus_on_right;
              if (disable_left_nav) {
                ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
              }
              const float settings_list_item_height = 40.0f * display_scale;
              const float settings_list_text_padding = 16.0f * display_scale;
              const float settings_list_border_margin = 6.0f * display_scale;
              const ImVec4 settings_border_selected =
                  GetConfiguredUIAccentColor(1.0f);
              for (int i = 0;
                   i < static_cast<int>(std::size(k_settings_sections)); ++i) {
                if (settings_focus_requested &&
                    i == settings_selected_section) {
                  ImGui::SetKeyboardFocusHere();
                }
                const bool settings_item_selected =
                    settings_selected_section == i;
                ImGui::PushID(i);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     settings_list_border_margin);
                const float item_width =
                    std::max(0.0f, ImGui::GetContentRegionAvail().x -
                                       settings_list_border_margin);
                const ImVec2 item_size(item_width, settings_list_item_height);
                ImGui::PushStyleColor(ImGuiCol_Header,
                                      ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_NavCursor,
                                      ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::Selectable("##settings_section_item",
                                  settings_item_selected,
                                  ImGuiSelectableFlags_None, item_size);
                ImGui::PopStyleColor(4);
                if (ImGui::IsItemClicked()) {
                  settings_selected_section = i;
                }
                ImDrawList* settings_draw_list = ImGui::GetWindowDrawList();
                const ImVec2 item_min = ImGui::GetItemRectMin();
                const ImVec2 item_max = ImGui::GetItemRectMax();
                if (settings_item_selected) {
                  const float border_thickness =
                      std::max(1.0f, 2.0f * display_scale);
                  const ImVec2 border_inset(border_thickness * 0.5f,
                                            border_thickness * 0.5f);
                  MarkConfiguredTextBegin(settings_draw_list);
                  settings_draw_list->AddRect(
                      ImVec2(item_min.x + border_inset.x,
                             item_min.y + border_inset.y),
                      ImVec2(item_max.x - border_inset.x,
                             item_max.y - border_inset.y),
                      ImGui::GetColorU32(settings_border_selected),
                      8.0f * display_scale, 0, border_thickness);
                  MarkConfiguredTextEnd(settings_draw_list);
                }
                const float text_y = item_min.y + (settings_list_item_height -
                                                   ImGui::GetTextLineHeight()) *
                                                      0.5f;
                const ImVec2 text_pos(item_min.x + settings_list_text_padding,
                                      text_y);
                DrawTextWithConfiguredEffect(
                    settings_draw_list, ImGui::GetFont(), ImGui::GetFontSize(),
                    text_pos, ImGui::GetColorU32(ImGuiCol_Text),
                    k_settings_sections[i]);
                const bool entry_focused = ImGui::IsItemFocused();
                ImGui::PopID();
                ImGui::Dummy(ImVec2(0.0f, 6.0f * display_scale));
                if (entry_focused) {
                  settings_selected_section = i;
                  settings_focus_requested = false;
                  settings_focus_on_right = false;
                  settings_list_has_focus = true;
                  settings_left_focus_detected = true;
                }
              }
              if (disable_left_nav) {
                ImGui::PopItemFlag();
              }

              if (!settings_focus_on_right && !settings_left_focus_detected &&
                  !settings_focus_requested) {
                settings_focus_requested = true;
              }

              if (settings_list_has_focus &&
                  (key_pressed(ImGuiKey_RightArrow, false) ||
                   key_pressed(ImGuiKey_GamepadDpadRight, false))) {
                settings_focus_right_requested = true;
                settings_focus_requested = false;
                settings_focus_on_right = true;
              }

              ImGui::TableNextColumn();
              ImGui::TableNextColumn();

              const bool disable_right_nav = !settings_focus_on_right;
              if (disable_right_nav) {
                ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
              }
              ImGui::PushStyleColor(ImGuiCol_NavCursor,
                                    ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

              if (settings_focus_right_requested) {
                ImGui::SetKeyboardFocusHere();
                settings_focus_right_requested = false;
                settings_focus_on_right = true;
              }

              const float settings_control_border_thickness =
                  std::max(1.0f, 2.0f * display_scale);
              const float settings_control_rounding = 8.0f * display_scale;
              ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,
                                  settings_control_border_thickness);
              ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                                  settings_control_rounding);
              const float settings_wrap_pos_x =
                  ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
              ImGui::PushTextWrapPos(settings_wrap_pos_x);
              const float settings_item_width =
                  std::max(220.0f, ImGui::GetContentRegionAvail().x * 0.34f);
              ImGui::PushItemWidth(settings_item_width);

              if (settings_selected_section == 0) {
                auto c_apply_patches = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("apply_patches")->second);
                if (ImGui::Checkbox("Apply Patches",
                                    c_apply_patches->current_value())) {
                  c_apply_patches->SetConfigValue(
                      !c_apply_patches->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_apply_patches->description();
                }

                auto c_controller_hotkeys =
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars->find("controller_hotkeys")->second);
                if (ImGui::Checkbox("Controller Hotkeys",
                                    c_controller_hotkeys->current_value())) {
                  c_controller_hotkeys->SetConfigValue(
                      !c_controller_hotkeys->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_controller_hotkeys->description();
                }

                auto* xconfig =
                    emulator_window_.emulator()->kernel_state()->xconfig();
                auto console_config = *xconfig->GetXConfig();
                int32_t user_language = console_config.user.language;
                ImGui::BeginDisabled(
                    emulator_window_.emulator()->is_title_open());
                struct LanguageOption {
                  int32_t value;
                  const char* label;
                };
                static constexpr LanguageOption kLanguageOptions[] = {
                    {1, "English"},    {2, "Japanese"},
                    {3, "German"},     {4, "French"},
                    {5, "Spanish"},    {6, "Italian"},
                    {7, "Korean"},     {8, "Traditional Chinese"},
                    {9, "Portuguese"}, {10, "Simplified Chinese"},
                    {11, "Polish"},    {12, "Russian"},
                };
                const char* user_language_preview = "English";
                for (const auto& option : kLanguageOptions) {
                  if (option.value == user_language) {
                    user_language_preview = option.label;
                    break;
                  }
                }
                {
                  ScopedAccentComboStyle accent_combo_style;
                  if (ImGui::BeginCombo("Language", user_language_preview)) {
                    for (const auto& option : kLanguageOptions) {
                      const bool is_selected = user_language == option.value;
                      if (ImGui::Selectable(option.label, is_selected)) {
                        console_config.user.language = option.value;
                        xconfig->WriteXConfig(&console_config);
                        user_language = option.value;
                      }
                      if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                      }
                    }
                    ImGui::EndCombo();
                  }
                }

                if (ImGui::IsItemFocused()) {
                  tooltip =
                      "Console language used by games. Change before launching "
                      "a title.";
                }
                ImGui::EndDisabled();

                ImGui::Spacing();
                if (DrawTextEffectButton("Open Profile Menu")) {
                  emulator_window_.ToggleProfilesConfigDialog();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip =
                      "Open the profile management dialog to create, edit, or "
                      "delete profiles.";
                }

                if (c_controller_hotkeys->GetTypedConfigValue()) {
                  ImGui::Spacing();
                  ImGui::PushStyleColor(ImGuiCol_Text,
                                        ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                  ImGui::TextUnformatted(
                      "Select + A        Cycle readback resolve mode.");
                  ImGui::TextUnformatted(
                      "Select + B        Toggle logging between configured and "
                      "Disabled.");
                  ImGui::TextUnformatted(
                      "Select + Y        Toggle fullscreen.");
                  ImGui::TextUnformatted(
                      "Select + X        Toggle Clear Memory Page State.");
                  ImGui::TextUnformatted("Select + RB       Clear GPU cache.");
                  ImGui::TextUnformatted(
                      "Select + LB       Toggle controller vibration.");
                  ImGui::TextUnformatted(
                      "Select + Up       Double CPU time scalar.");
                  ImGui::TextUnformatted(
                      "Select + Down     Half CPU time scalar.");
                  ImGui::TextUnformatted(
                      "Select + Right    Reset CPU time scalar.");
                  ImGui::PopStyleColor();
                }
              }

              if (settings_selected_section == 1) {
                auto c_allow_vrr_tearing = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars
                        ->find("d3d12_allow_variable_refresh_rate_and_tearing")
                        ->second);
                if (ImGui::Checkbox("Allow Variable Refresh Rate/Tearing",
                                    c_allow_vrr_tearing->current_value())) {
                  c_allow_vrr_tearing->SetConfigValue(
                      !c_allow_vrr_tearing->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_allow_vrr_tearing->description();
                }

                auto c_clear_memory_page = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("clear_memory_page_state")->second);
                if (ImGui::Checkbox("Clear Memory Page State",
                                    c_clear_memory_page->current_value())) {
                  c_clear_memory_page->SetConfigValue(
                      !c_clear_memory_page->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_clear_memory_page->description();
                }

                auto c_readback_resolve =
                    dynamic_cast<cvar::ConfigVar<std::string>*>(
                        cvar::ConfigVars->find("readback_resolve")->second);
                std::string c_readback_resolve_value =
                    c_readback_resolve->GetTypedConfigValue();
                {
                  ScopedAccentComboStyle accent_combo_style;
                  if (ImGui::BeginCombo("Readback Resolve",
                                        c_readback_resolve_value.c_str())) {
                    if (ImGui::Selectable("None",
                                          c_readback_resolve_value == "none")) {
                      c_readback_resolve->SetConfigValue("none");
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable("Fast",
                                          c_readback_resolve_value == "fast")) {
                      c_readback_resolve->SetConfigValue("fast");
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable("Full",
                                          c_readback_resolve_value == "full")) {
                      c_readback_resolve->SetConfigValue("full");
                      config::SaveConfig();
                    }

                    ImGui::EndCombo();
                  }
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_readback_resolve->description();
                }

                auto c_d3d12_bindless = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("d3d12_bindless")->second);
                if (ImGui::Checkbox("Enable D3D12 Bindless",
                                    c_d3d12_bindless->current_value())) {
                  c_d3d12_bindless->SetConfigValue(
                      !c_d3d12_bindless->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_d3d12_bindless->description();
                }

                auto c_d3d12_submit_on_primary_buffer_end =
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars
                            ->find("d3d12_submit_on_primary_buffer_end")
                            ->second);
                if (ImGui::Checkbox("Submit On Primary Buffer End",
                                    c_d3d12_submit_on_primary_buffer_end
                                        ->current_value())) {
                  c_d3d12_submit_on_primary_buffer_end->SetConfigValue(
                      !c_d3d12_submit_on_primary_buffer_end
                           ->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_d3d12_submit_on_primary_buffer_end->description();
                }

                auto c_d3d12_enable_tiled_shared_memory =
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars
                            ->find("d3d12_enable_tiled_shared_memory")
                            ->second);
                if (ImGui::Checkbox(
                        "Enable D3D12 Tiled Shared Memory (On Xbox keep\nOFF "
                        "otherwise games will crash)",
                        c_d3d12_enable_tiled_shared_memory->current_value())) {
                  c_d3d12_enable_tiled_shared_memory->SetConfigValue(
                      !c_d3d12_enable_tiled_shared_memory
                           ->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_d3d12_enable_tiled_shared_memory->description();
                }
              }

              if (settings_selected_section == 2) {
                auto c_post_scaling =
                    dynamic_cast<cvar::ConfigVar<std::string>*>(
                        cvar::ConfigVars
                            ->find("postprocess_scaling_and_sharpening")
                            ->second);
                std::string c_post_scaling_value =
                    c_post_scaling->GetTypedConfigValue();
                {
                  ScopedAccentComboStyle accent_combo_style;
                  if (ImGui::BeginCombo("Scaling & Sharpening Effect",
                                        (c_post_scaling_value == ""
                                             ? "None"
                                             : c_post_scaling_value.c_str()))) {
                    if (ImGui::Selectable("None", c_post_scaling_value == "")) {
                      c_post_scaling->SetConfigValue("");
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable("Bilinear",
                                          c_post_scaling_value == "bilinear")) {
                      c_post_scaling->SetConfigValue("bilinear");
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable("CAS",
                                          c_post_scaling_value == "cas")) {
                      c_post_scaling->SetConfigValue("cas");
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable("FSR",
                                          c_post_scaling_value == "fsr")) {
                      c_post_scaling->SetConfigValue("fsr");
                      config::SaveConfig();
                    }

                    ImGui::EndCombo();
                  }
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_post_scaling->description();
                }

                auto c_post_aa = dynamic_cast<cvar::ConfigVar<std::string>*>(
                    cvar::ConfigVars->find("postprocess_antialiasing")->second);
                std::string postaa_value = c_post_aa->GetTypedConfigValue();
                {
                  ScopedAccentComboStyle accent_combo_style;
                  if (ImGui::BeginCombo(
                          "Anti-Aliasing",
                          (postaa_value == "" ? "None"
                                              : postaa_value.c_str()))) {
                    if (ImGui::Selectable("None", postaa_value == "")) {
                      c_post_aa->SetConfigValue("");
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable("FXAA", postaa_value == "fxaa")) {
                      c_post_aa->SetConfigValue("fxaa");
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable("FXAA Extreme",
                                          postaa_value == "fxaa_extreme")) {
                      c_post_aa->SetConfigValue("fxaa_extreme");
                      config::SaveConfig();
                    }

                    ImGui::EndCombo();
                  }
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_post_aa->description();
                }

                auto c_cas_sharpness = dynamic_cast<cvar::ConfigVar<double>*>(
                    cvar::ConfigVars
                        ->find("postprocess_ffx_cas_additional_sharpness")
                        ->second);
                float cas_additional =
                    (float)c_cas_sharpness->GetTypedConfigValue();
                bool cas_additional_changed = false;
                if (ImGui::SliderFloat("CAS Additional Sharpness",
                                       &cas_additional, 0.0f, 1.0f, "%.3f")) {
                  cas_additional_changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Button(
                        "Reset##ResetCASAdditionalSharpnessSettings")) {
                  cas_additional = ui::Presenter::GuestOutputPaintConfig::
                      kCasAdditionalSharpnessDefault;
                  cas_additional_changed = true;
                }
                if (cas_additional_changed) {
                  c_cas_sharpness->SetConfigValue((double)cas_additional);
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_cas_sharpness->description();
                }

                auto c_fsr_max_upsampling =
                    dynamic_cast<cvar::ConfigVar<uint32_t>*>(
                        cvar::ConfigVars
                            ->find("postprocess_ffx_fsr_max_upsampling_passes")
                            ->second);
                uint32_t fsr_max_upsampling_value =
                    c_fsr_max_upsampling->GetTypedConfigValue();
                char value_label[32];
                snprintf(value_label, 32, "%dx", fsr_max_upsampling_value);
                {
                  ScopedAccentComboStyle accent_combo_style;
                  if (ImGui::BeginCombo("FSR Max Upsampling Passes",
                                        value_label)) {
                    if (ImGui::Selectable("1x",
                                          fsr_max_upsampling_value == 1)) {
                      c_fsr_max_upsampling->SetConfigValue(1);
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable("2x",
                                          fsr_max_upsampling_value == 2)) {
                      c_fsr_max_upsampling->SetConfigValue(2);
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable("3x",
                                          fsr_max_upsampling_value == 3)) {
                      c_fsr_max_upsampling->SetConfigValue(3);
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable("4x",
                                          fsr_max_upsampling_value == 4)) {
                      c_fsr_max_upsampling->SetConfigValue(4);
                      config::SaveConfig();
                    }

                    ImGui::EndCombo();
                  }
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_fsr_max_upsampling->description();
                }

                auto c_fsr_sharpness_reduction =
                    dynamic_cast<cvar::ConfigVar<double>*>(
                        cvar::ConfigVars
                            ->find("postprocess_ffx_fsr_sharpness_reduction")
                            ->second);
                float fsr_sharpness_reduction =
                    (float)c_fsr_sharpness_reduction->GetTypedConfigValue();
                const auto fsr_label =
                    fmt::format("{:.3f} stops", fsr_sharpness_reduction);
                float fsr_slider_value = sqrt(2.0f * fsr_sharpness_reduction);
                bool fsr_sharpness_changed = false;
                if (ImGui::SliderFloat(
                        "FSR Sharpness Reduction", &fsr_slider_value,
                        ui::Presenter::GuestOutputPaintConfig::
                            kFsrSharpnessReductionMin,
                        ui::Presenter::GuestOutputPaintConfig::
                            kFsrSharpnessReductionMax,
                        fsr_label.c_str(), ImGuiSliderFlags_NoInput)) {
                  fsr_sharpness_changed = true;
                }
                fsr_sharpness_reduction =
                    0.5f * fsr_slider_value * fsr_slider_value;
                ImGui::SameLine();
                if (ImGui::Button(
                        "Reset##ResetFSRSharpnessReductionSettings")) {
                  fsr_sharpness_reduction = ui::Presenter::
                      GuestOutputPaintConfig::kFsrSharpnessReductionDefault;
                  fsr_sharpness_changed = true;
                }
                if (fsr_sharpness_changed) {
                  c_fsr_sharpness_reduction->SetConfigValue(
                      (double)fsr_sharpness_reduction);
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_fsr_sharpness_reduction->description();
                }

                auto c_present_letterbox = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("present_letterbox")->second);
                if (ImGui::Checkbox("Letterbox Presentation",
                                    c_present_letterbox->current_value())) {
                  c_present_letterbox->SetConfigValue(
                      !c_present_letterbox->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_present_letterbox->description();
                }

                auto c_present_safe_area_x =
                    dynamic_cast<cvar::ConfigVar<int32_t>*>(
                        cvar::ConfigVars->find("present_safe_area_x")->second);
                int present_safe_area_x =
                    c_present_safe_area_x->GetTypedConfigValue();
                if (ImGui::SliderInt("Presentation Safe Area Width",
                                     &present_safe_area_x, 1, 100, "%d%%")) {
                  c_present_safe_area_x->SetConfigValue(present_safe_area_x);
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_present_safe_area_x->description();
                }

                auto c_present_safe_area_y =
                    dynamic_cast<cvar::ConfigVar<int32_t>*>(
                        cvar::ConfigVars->find("present_safe_area_y")->second);
                int present_safe_area_y =
                    c_present_safe_area_y->GetTypedConfigValue();
                if (ImGui::SliderInt("Presentation Safe Area Height",
                                     &present_safe_area_y, 1, 100, "%d%%")) {
                  c_present_safe_area_y->SetConfigValue(present_safe_area_y);
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_present_safe_area_y->description();
                }

                auto c_present_render_pass_clear =
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars->find("present_render_pass_clear")
                            ->second);
                if (ImGui::Checkbox(
                        "Use Presentation Render-Pass Clear",
                        c_present_render_pass_clear->current_value())) {
                  c_present_render_pass_clear->SetConfigValue(
                      !c_present_render_pass_clear->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_present_render_pass_clear->description();
                }

                auto c_host_present_from_non_ui_thread =
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars
                            ->find("host_present_from_non_ui_thread")
                            ->second);
                if (ImGui::Checkbox(
                        "Host Present From Non-UI Thread",
                        c_host_present_from_non_ui_thread->current_value())) {
                  c_host_present_from_non_ui_thread->SetConfigValue(
                      !c_host_present_from_non_ui_thread
                           ->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_host_present_from_non_ui_thread->description();
                }

                auto c_post_dither = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("postprocess_dither")->second);
                if (ImGui::Checkbox("Postprocess Dither",
                                    c_post_dither->current_value())) {
                  c_post_dither->SetConfigValue(
                      !c_post_dither->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_post_dither->description();
                }
              }

              if (settings_selected_section == 3) {
                static bool gpu_settings_debug_logged = false;
                if (!gpu_settings_debug_logged) {
                  gpu_settings_debug_logged = true;
                }

                auto c_allow_invalid = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("gpu_allow_invalid_fetch_constants")
                        ->second);
                if (ImGui::Checkbox("Allow Invalid Fetch Constants",
                                    c_allow_invalid->current_value())) {
                  c_allow_invalid->SetConfigValue(
                      !c_allow_invalid->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_allow_invalid->description();
                }

                auto c_dxbc_switch = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("dxbc_switch")->second);
                if (ImGui::Checkbox("DXBC Switch",
                                    c_dxbc_switch->current_value())) {
                  c_dxbc_switch->SetConfigValue(
                      !c_dxbc_switch->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_dxbc_switch->description();
                }

                auto c_vsync = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("vsync")->second);
                if (ImGui::Checkbox("V-Sync", c_vsync->current_value())) {
                  c_vsync->SetConfigValue(!c_vsync->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_vsync->description();
                }

                auto c_framerate_limit =
                    dynamic_cast<cvar::ConfigVar<uint64_t>*>(
                        cvar::ConfigVars->find("framerate_limit")->second);
                int framerate_limit_value =
                    static_cast<int>(c_framerate_limit->GetTypedConfigValue());
                if (ImGui::SliderInt("Framerate Limit", &framerate_limit_value,
                                     0, 240)) {
                  c_framerate_limit->SetConfigValue(
                      static_cast<uint64_t>(framerate_limit_value));
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_framerate_limit->description();
                }

                auto c_draw_resolution_scaled_texture_offsets =
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars
                            ->find("draw_resolution_scaled_texture_offsets")
                            ->second);
                if (ImGui::Checkbox("Draw Resolution Scaled Texture Offsets",
                                    c_draw_resolution_scaled_texture_offsets
                                        ->current_value())) {
                  c_draw_resolution_scaled_texture_offsets->SetConfigValue(
                      !c_draw_resolution_scaled_texture_offsets
                           ->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip =
                      c_draw_resolution_scaled_texture_offsets->description();
                }

                auto c_render_target_path =
                    dynamic_cast<cvar::ConfigVar<std::string>*>(
                        cvar::ConfigVars->find("render_target_path_d3d12")
                            ->second);
                std::string c_render_target_path_value =
                    c_render_target_path->GetTypedConfigValue();
                {
                  ScopedAccentComboStyle accent_combo_style;
                  if (ImGui::BeginCombo(
                          "Render Target Path",
                          (c_render_target_path_value == ""
                               ? "Any"
                               : c_render_target_path_value.c_str()))) {
                    if (ImGui::Selectable("Any",
                                          c_render_target_path_value == "")) {
                      c_render_target_path->SetConfigValue("");
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable(
                            "ROV", c_render_target_path_value == "rov")) {
                      c_render_target_path->SetConfigValue("rov");
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable(
                            "RTV", c_render_target_path_value == "rtv")) {
                      c_render_target_path->SetConfigValue("rtv");
                      config::SaveConfig();
                    }

                    ImGui::EndCombo();
                  }
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_render_target_path->description();
                }

                auto c_gamma_rt = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("gamma_render_target_as_unorm16")
                        ->second);
                if (ImGui::Checkbox("Gamma Render Target As UNORM16",
                                    c_gamma_rt->current_value())) {
                  c_gamma_rt->SetConfigValue(
                      !c_gamma_rt->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_gamma_rt->description();
                }

                auto c_fuzzy_alpha = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("use_fuzzy_alpha_epsilon")->second);
                if (ImGui::Checkbox("Use Fuzzy Alpha Epsilon",
                                    c_fuzzy_alpha->current_value())) {
                  c_fuzzy_alpha->SetConfigValue(
                      !c_fuzzy_alpha->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_fuzzy_alpha->description();
                }

                auto c_native_stencil_value_output =
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars->find("native_stencil_value_output")
                            ->second);
                if (ImGui::Checkbox(
                        "Native Stencil Value Output",
                        c_native_stencil_value_output->current_value())) {
                  c_native_stencil_value_output->SetConfigValue(
                      !c_native_stencil_value_output->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_native_stencil_value_output->description();
                }

                auto c_snorm16_render_target_full_range =
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars
                            ->find("snorm16_render_target_full_range")
                            ->second);
                if (ImGui::Checkbox(
                        "SNORM16 Render Target Full Range",
                        c_snorm16_render_target_full_range->current_value())) {
                  c_snorm16_render_target_full_range->SetConfigValue(
                      !c_snorm16_render_target_full_range
                           ->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_snorm16_render_target_full_range->description();
                }

                auto c_mrt_edram_used_range_clamp_to_min =
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars
                            ->find("mrt_edram_used_range_clamp_to_min")
                            ->second);
                if (ImGui::Checkbox(
                        "MRT EDRAM Used Range Clamp To Min",
                        c_mrt_edram_used_range_clamp_to_min->current_value())) {
                  c_mrt_edram_used_range_clamp_to_min->SetConfigValue(
                      !c_mrt_edram_used_range_clamp_to_min
                           ->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_mrt_edram_used_range_clamp_to_min->description();
                }

                auto c_store_shaders = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("store_shaders")->second);
                if (ImGui::Checkbox("Store Shaders",
                                    c_store_shaders->current_value())) {
                  c_store_shaders->SetConfigValue(
                      !c_store_shaders->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_store_shaders->description();
                }

                auto c_scale_x = dynamic_cast<cvar::ConfigVar<int>*>(
                    cvar::ConfigVars->find("draw_resolution_scale_x")->second);
                auto c_scale_y = dynamic_cast<cvar::ConfigVar<int>*>(
                    cvar::ConfigVars->find("draw_resolution_scale_y")->second);
                int scale_value = c_scale_x->GetTypedConfigValue();
                char scale_value_label[32];
                snprintf(scale_value_label, 32, "%dx", scale_value);
                {
                  ScopedAccentComboStyle accent_combo_style;
                  if (ImGui::BeginCombo("Draw Resolution Scale",
                                        scale_value_label)) {
                    if (ImGui::Selectable("1x", scale_value == 1)) {
                      c_scale_x->SetConfigValue(1);
                      c_scale_y->SetConfigValue(1);
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable("2x", scale_value == 2)) {
                      c_scale_x->SetConfigValue(2);
                      c_scale_y->SetConfigValue(2);
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable("3x", scale_value == 3)) {
                      c_scale_x->SetConfigValue(3);
                      c_scale_y->SetConfigValue(3);
                      config::SaveConfig();
                    }

                    ImGui::EndCombo();
                  }
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_scale_x->description();
                }

                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                                   "(Scaled rendering commits extra memory per "
                                   "resolved region; 3x needs the most)");
              }

              if (settings_selected_section == 4) {
                auto c_apu = dynamic_cast<cvar::ConfigVar<std::string>*>(
                    cvar::ConfigVars->find("apu")->second);
                std::string apu_value = c_apu->GetTypedConfigValue();
                const char* apu_preview =
                    apu_value.empty() ? "Any" : apu_value.c_str();
                {
                  ScopedAccentComboStyle accent_combo_style;
                  if (ImGui::BeginCombo("Audio System", apu_preview)) {
                    if (ImGui::Selectable("Any", apu_value == "any")) {
                      c_apu->SetConfigValue("any");
                      config::SaveConfig();
                    }

                    if (ImGui::Selectable("Nop", apu_value == "nop")) {
                      c_apu->SetConfigValue("nop");
                      config::SaveConfig();
                    }

#if XE_PLATFORM_WIN32
                    if (ImGui::Selectable("XAudio2", apu_value == "xaudio2")) {
                      c_apu->SetConfigValue("xaudio2");
                      config::SaveConfig();
                    }
#endif

#if XE_PLATFORM_LINUX
                    if (ImGui::Selectable("ALSA", apu_value == "alsa")) {
                      c_apu->SetConfigValue("alsa");
                      config::SaveConfig();
                    }
#endif

#if !XE_PLATFORM_ANDROID && !XE_PLATFORM_WINRT
                    if (ImGui::Selectable("SDL", apu_value == "sdl")) {
                      c_apu->SetConfigValue("sdl");
                      config::SaveConfig();
                    }
#endif

                    ImGui::EndCombo();
                  }
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_apu->description();
                }

                auto c_mute = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("mute")->second);
                if (ImGui::Checkbox("Mute Audio", c_mute->current_value())) {
                  c_mute->SetConfigValue(!c_mute->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_mute->description();
                }
              }

              if (settings_selected_section == 5) {
                auto c_ignore_offset = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars
                        ->find("ignore_offset_for_ranged_allocations")
                        ->second);
                if (ImGui::Checkbox("Ignore Offset",
                                    c_ignore_offset->current_value())) {
                  c_ignore_offset->SetConfigValue(
                      !c_ignore_offset->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_ignore_offset->description();
                }

                auto c_protect_on_release =
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars->find("protect_on_release")->second);
                if (ImGui::Checkbox("Protect On Release",
                                    c_protect_on_release->current_value())) {
                  c_protect_on_release->SetConfigValue(
                      !c_protect_on_release->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_protect_on_release->description();
                }

                auto c_protect_zero = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("protect_zero")->second);
                if (ImGui::Checkbox("Protect Zero",
                                    c_protect_zero->current_value())) {
                  c_protect_zero->SetConfigValue(
                      !c_protect_zero->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_protect_zero->description();
                }

                auto c_scribble_heap = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("scribble_heap")->second);
                if (ImGui::Checkbox("Scribble Heap",
                                    c_scribble_heap->current_value())) {
                  c_scribble_heap->SetConfigValue(
                      !c_scribble_heap->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_scribble_heap->description();
                }
              }

              if (settings_selected_section == 6) {
                auto c_mount_cache = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("mount_cache")->second);
                if (ImGui::Checkbox("Mount Cache",
                                    c_mount_cache->current_value())) {
                  c_mount_cache->SetConfigValue(
                      !c_mount_cache->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_mount_cache->description();
                }

                auto c_mount_scratch = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("mount_scratch")->second);
                if (ImGui::Checkbox("Mount Scratch",
                                    c_mount_scratch->current_value())) {
                  c_mount_scratch->SetConfigValue(
                      !c_mount_scratch->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_mount_scratch->description();
                }

                auto c_mount_memory_unit = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("mount_memory_unit")->second);
                if (ImGui::Checkbox("Mount Memory Unit",
                                    c_mount_memory_unit->current_value())) {
                  c_mount_memory_unit->SetConfigValue(
                      !c_mount_memory_unit->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_mount_memory_unit->description();
                }
              }

              if (settings_selected_section == 7) {
                scraper.DrawUISettings();
                auto bool_toggle =
                    [&](const char* label, const char* true_label,
                        const char* false_label, cvar::ConfigVar<bool>* cvar,
                        const char* desc) {
                      bool current = cvar->GetTypedConfigValue();
                      const char* preview = current ? true_label : false_label;
                      {
                        ScopedAccentComboStyle accent_combo_style;
                        if (ImGui::BeginCombo(label, preview)) {
                          if (ImGui::Selectable(true_label, current)) {
                            if (!current) {
                              cvar->SetConfigValue(true);
                              config::SaveConfig();
                            }
                          }

                          if (ImGui::Selectable(false_label, !current)) {
                            if (current) {
                              cvar->SetConfigValue(false);
                              config::SaveConfig();
                            }
                          }
                          ImGui::EndCombo();
                        }
                      }
                      if (ImGui::IsItemFocused()) {
                        tooltip = desc;
                      }
                    };

                bool_toggle(
                    "Achievement Notifications", "Show Notifications",
                    "Hide Notifications",
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars->find("show_achievement_notification")
                            ->second),
                    "Choose whether achievement pop-ups are shown.");

                bool_toggle(
                    "Storage Selection Dialog", "Enable Dialog",
                    "Disable Dialog",
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars->find("storage_selection_dialog")
                            ->second),
                    "Enable or disable the storage selection dialog.");

                bool_toggle(
                    "Genre Metadata", "Show Genre", "Hide Genre",
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars->find("ui_show_genre_metadata")
                            ->second),
                    "Toggle genre metadata in the Games tab detail pane.");

                bool_toggle(
                    "Developer Metadata", "Show Developer", "Hide Developer",
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars->find("ui_show_developer_metadata")
                            ->second),
                    "Toggle developer metadata in the Games tab detail pane.");

                bool_toggle(
                    "Title Text", "Show Game Title", "Hide Game Title",
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars->find("ui_show_title_metadata")
                            ->second),
                    "Toggle the selected game's title in the metadata panel.");

                bool_toggle(
                    "Title ID Metadata", "Show Title ID", "Hide Title ID",
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars->find("ui_show_title_id_metadata")
                            ->second),
                    "Toggle Title ID visibility in the Games tab detail pane.");

                bool_toggle(
                    "Release Date Metadata", "Show Release Date",
                    "Hide Release Date",
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars->find("ui_show_release_date_metadata")
                            ->second),
                    "Toggle release dates in the Games tab detail pane.");

                bool_toggle(
                    "User Rating Metadata", "Show User Rating",
                    "Hide User Rating",
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars->find("ui_show_user_rating_metadata")
                            ->second),
                    "Toggle user ratings in the Games tab detail pane.");

                auto set_color = [](const char* name,
                                    const std::string& value) {
                  dynamic_cast<cvar::ConfigVar<std::string>*>(
                      cvar::ConfigVars->at(name))
                      ->SetConfigValue(value);
                };
                auto color_combo = [&](const char* label, const char* name,
                                       bool theme = false) {
                  auto* setting = dynamic_cast<cvar::ConfigVar<std::string>*>(
                      cvar::ConfigVars->at(name));
                  const auto current =
                      NormalizeThemeColor(setting->GetTypedConfigValue());
                  const char* preview = "Green";
                  for (const auto& color : kThemeColors) {
                    if (current == color.value) {
                      preview = color.label;
                    }
                  }
                  ScopedAccentComboStyle accent_combo_style;
                  if (ImGui::BeginCombo(label, preview)) {
                    for (const auto& color : kThemeColors) {
                      const bool selected = current == color.value;
                      if (ImGui::Selectable(color.label, selected)) {
                        if (theme) {
                          ApplyColorTheme(color.value, set_color);
                        } else {
                          set_color(name, color.value);
                        }
                        config::SaveConfig();
                      }
                      if (selected) {
                        ImGui::SetItemDefaultFocus();
                      }
                    }
                    ImGui::EndCombo();
                  }
                };
                color_combo("Color Theme", "ui_color_theme", true);
                color_combo("Interface Overlay Color",
                            "ui_interface_overlay_color");
                color_combo("Guide Panel Color", "ui_guide_color");
                color_combo("Background Color", "ui_background_color");

                // Hide tabs text toggle
                auto c_hide_tabs = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("ui_hide_tabs_text")->second);
                if (ImGui::Checkbox("Hide Tabs Text",
                                    c_hide_tabs->current_value())) {
                  c_hide_tabs->SetConfigValue(
                      !c_hide_tabs->GetTypedConfigValue());
                  config::SaveConfig();
                }
                if (ImGui::IsItemFocused()) {
                  tooltip =
                      "Hide or show the text labels for tabs (games, settings, "
                      "paths, about) in the frontend.";
                }

                auto c_border_color =
                    dynamic_cast<cvar::ConfigVar<std::string>*>(
                        cvar::ConfigVars->find("ui_border_color")->second);
                std::string current_border_color =
                    c_border_color ? c_border_color->GetTypedConfigValue()
                                   : "green";
                if (current_border_color == "grey") current_border_color = "silver";
                const char* border_color_values[] = {
                    "green",  "purple", "red",   "orange", "blue",
                    "yellow", "silver", "black", "white"};
                const char* border_color_labels[] = {
                    "Green (#61C032)", "Purple", "Red",   "Orange", "Blue",
                    "Yellow",          "Silver", "Black", "White"};
                int current_border_color_index = 0;
                for (int i = 0;
                     i < static_cast<int>(sizeof(border_color_values) /
                                          sizeof(border_color_values[0]));
                     ++i) {
                  if (current_border_color == border_color_values[i]) {
                    current_border_color_index = i;
                    break;
                  }
                }
                {
                  ScopedAccentComboStyle accent_combo_style;
                  if (ImGui::BeginCombo(
                          "UI Accent Color",
                          border_color_labels[current_border_color_index])) {
                    for (int i = 0;
                         i < static_cast<int>(sizeof(border_color_values) /
                                              sizeof(border_color_values[0]));
                         ++i) {
                      bool is_selected = current_border_color_index == i;
                      if (ImGui::Selectable(border_color_labels[i],
                                            is_selected)) {
                        current_border_color_index = i;
                        c_border_color->SetConfigValue(border_color_values[i]);
                        config::SaveConfig();
                      }
                      if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                      }
                    }
                    ImGui::EndCombo();
                  }
                }
                if (ImGui::IsItemFocused()) {
                  tooltip =
                      "Choose the frontend accent color used for borders, "
                      "checkboxes, combo accents, and header backgrounds.";
                }

                // Text effect combo box
                auto c_text_effect =
                    dynamic_cast<cvar::ConfigVar<std::string>*>(
                        cvar::ConfigVars->find("ui_text_effect")->second);
                std::string current_effect =
                    c_text_effect ? c_text_effect->GetTypedConfigValue()
                                  : "none";

                const char* text_effects[] = {
                    "shadow",  "lift",          "stroke_fill", "glow",
                    "outline", "double_shadow", "bold",        "none"};
                int current_effect_index = 0;
                for (int i = 0; i < static_cast<int>(sizeof(text_effects) /
                                                     sizeof(text_effects[0]));
                     ++i) {
                  if (current_effect == text_effects[i]) {
                    current_effect_index = i;
                    break;
                  }
                }

                const char* preview = text_effects[current_effect_index];
                {
                  ScopedAccentComboStyle accent_combo_style;
                  if (ImGui::BeginCombo("Text Effect", preview)) {
                    for (int i = 0;
                         i < static_cast<int>(sizeof(text_effects) /
                                              sizeof(text_effects[0]));
                         ++i) {
                      bool is_selected = (current_effect_index == i);
                      if (ImGui::Selectable(text_effects[i], is_selected)) {
                        current_effect_index = i;
                        c_text_effect->SetConfigValue(text_effects[i]);
                        config::SaveConfig();
                      }
                      if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                      }
                    }
                    ImGui::EndCombo();
                  }
                }
                if (ImGui::IsItemFocused()) {
                  tooltip =
                      "Apply a frontend text effect. Options include shadow, "
                      "lift, stroke + fill, glow, outline, double shadow, and "
                      "bold.";
                }

                auto c_text_color = dynamic_cast<cvar::ConfigVar<std::string>*>(
                    cvar::ConfigVars->find("ui_text_color")->second);
                std::string current_text_color =
                    c_text_color ? c_text_color->GetTypedConfigValue()
                                 : "white";
                const char* text_colors[] = {"white", "black"};
                int current_text_color_index =
                    current_text_color == "black" ? 1 : 0;
                {
                  ScopedAccentComboStyle accent_combo_style;
                  if (ImGui::BeginCombo(
                          "Text Color",
                          text_colors[current_text_color_index])) {
                    for (int i = 0; i < 2; ++i) {
                      bool is_selected = (current_text_color_index == i);
                      if (ImGui::Selectable(text_colors[i], is_selected)) {
                        current_text_color_index = i;
                        c_text_color->SetConfigValue(text_colors[i]);
                        config::SaveConfig();
                      }
                      if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                      }
                    }
                    ImGui::EndCombo();
                  }
                }
                if (ImGui::IsItemFocused()) {
                  tooltip =
                      "Choose whether monochrome frontend text renders as "
                      "white or black. White is the default.";
                }

                auto c_text_brightness = dynamic_cast<cvar::ConfigVar<double>*>(
                    cvar::ConfigVars->find("ui_text_brightness")->second);
                float text_brightness =
                    c_text_brightness
                        ? static_cast<float>(
                              c_text_brightness->GetTypedConfigValue())
                        : 1.0f;
                if (ImGui::SliderFloat("Text Brightness", &text_brightness,
                                       0.25f, 2.0f, "%.2f")) {
                  c_text_brightness->SetConfigValue(
                      static_cast<double>(text_brightness));
                  config::SaveConfig();
                }
                if (ImGui::IsItemFocused()) {
                  tooltip =
                      "Adjust the overall brightness applied to frontend text.";
                }

                auto c_text_contrast = dynamic_cast<cvar::ConfigVar<double>*>(
                    cvar::ConfigVars->find("ui_text_contrast")->second);
                float text_contrast =
                    c_text_contrast
                        ? static_cast<float>(
                              c_text_contrast->GetTypedConfigValue())
                        : 1.0f;
                if (ImGui::SliderFloat("Text Contrast", &text_contrast, 0.25f,
                                       2.0f, "%.2f")) {
                  c_text_contrast->SetConfigValue(
                      static_cast<double>(text_contrast));
                  config::SaveConfig();
                }
                if (ImGui::IsItemFocused()) {
                  tooltip =
                      "Adjust the overall contrast applied to frontend text.";
                }
              }

              if (settings_selected_section == 8) {
                auto c_host_guest_stacksync =
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars
                            ->find("enable_host_guest_stack_synchronization")
                            ->second);
                if (ImGui::Checkbox("Enable Host Guest Stack Synchronization",
                                    c_host_guest_stacksync->current_value())) {
                  c_host_guest_stacksync->SetConfigValue(
                      !c_host_guest_stacksync->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_host_guest_stacksync->description();
                }

                auto c_disable_context_promotion =
                    dynamic_cast<cvar::ConfigVar<bool>*>(
                        cvar::ConfigVars->find("disable_context_promotion")
                            ->second);
                if (ImGui::Checkbox(
                        "Disable Context Promotion",
                        c_disable_context_promotion->current_value())) {
                  c_disable_context_promotion->SetConfigValue(
                      !c_disable_context_promotion->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  // Guest code is translated when a game loads.
                  tooltip = c_disable_context_promotion->description() +
                            " Applies the next time a game starts.";
                }
              }

              if (settings_selected_section == 9) {
                auto* xconfig =
                    emulator_window_.emulator()->kernel_state()->xconfig();
                auto console_config = *xconfig->GetXConfig();
                const auto resolution = kernel::Resolution(
                    console_config.user.av_pack_hdmi_sz.get());
                ImGui::BeginDisabled(
                    emulator_window_.emulator()->is_title_open());
                {
                  ScopedAccentComboStyle accent_combo_style;
                  if (ImGui::BeginCombo("Internal Display Resolution",
                                        resolution.name_.c_str())) {
                    for (const auto& option : kernel::XVGAResolution) {
                      if (ImGui::Selectable(
                              option.name_.c_str(),
                              option.to_host() == resolution.to_host())) {
                        console_config.user.av_pack_hdmi_sz = option.to_host();
                        auto video_flags =
                            console_config.user.video_flags.get();
                        video_flags &= ~kernel::X_VIDEO_FLAGS::Widescreen;
                        if (option.is_widescreen()) {
                          video_flags |= kernel::X_VIDEO_FLAGS::Widescreen;
                        }
                        console_config.user.video_flags = video_flags;
                        xconfig->WriteXConfig(&console_config);
                      }
                    }
                    ImGui::EndCombo();
                  }
                }
                if (ImGui::IsItemFocused()) {
                  tooltip =
                      "Console display resolution reported to games. Change "
                      "before launching a title.";
                }
                bool widescreen = (console_config.user.video_flags.get() &
                                   kernel::X_VIDEO_FLAGS::Widescreen) != 0;
                ImGui::BeginDisabled(resolution.is_widescreen());
                if (ImGui::Checkbox("Widescreen", &widescreen)) {
                  auto video_flags = console_config.user.video_flags.get();
                  if (widescreen) {
                    video_flags |= kernel::X_VIDEO_FLAGS::Widescreen;
                  } else {
                    video_flags &= ~kernel::X_VIDEO_FLAGS::Widescreen;
                  }
                  console_config.user.video_flags = video_flags;
                  xconfig->WriteXConfig(&console_config);
                }
                ImGui::EndDisabled();
                ImGui::EndDisabled();
                auto c_interlaced = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("interlaced")->second);
                if (ImGui::Checkbox("Interlaced Video Mode",
                                    c_interlaced->current_value())) {
                  c_interlaced->SetConfigValue(
                      !c_interlaced->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_interlaced->description();
                }
              }

              if (settings_selected_section == 10) {
                auto c_left_deadzone = dynamic_cast<cvar::ConfigVar<double>*>(
                    cvar::ConfigVars->find("left_stick_deadzone_percentage")
                        ->second);
                auto c_right_deadzone = dynamic_cast<cvar::ConfigVar<double>*>(
                    cvar::ConfigVars->find("right_stick_deadzone_percentage")
                        ->second);
                auto c_left_trigger_deadzone =
                    dynamic_cast<cvar::ConfigVar<double>*>(
                        cvar::ConfigVars
                            ->find("left_trigger_deadzone_percentage")
                            ->second);
                auto c_right_trigger_deadzone =
                    dynamic_cast<cvar::ConfigVar<double>*>(
                        cvar::ConfigVars
                            ->find("right_trigger_deadzone_percentage")
                            ->second);
                auto c_vibration = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("vibration")->second);

                float left_deadzone =
                    static_cast<float>(c_left_deadzone->GetTypedConfigValue());
                if (ImGui::SliderFloat("Left Stick Deadzone", &left_deadzone,
                                       0.0f, 1.0f, "%.2f")) {
                  c_left_deadzone->SetConfigValue(
                      static_cast<double>(left_deadzone));
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_left_deadzone->description();
                }

                float right_deadzone =
                    static_cast<float>(c_right_deadzone->GetTypedConfigValue());
                if (ImGui::SliderFloat("Right Stick Deadzone", &right_deadzone,
                                       0.0f, 1.0f, "%.2f")) {
                  c_right_deadzone->SetConfigValue(
                      static_cast<double>(right_deadzone));
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_right_deadzone->description();
                }

                float left_trigger_deadzone = static_cast<float>(
                    c_left_trigger_deadzone->GetTypedConfigValue());
                if (ImGui::SliderFloat("Left Trigger Deadzone",
                                       &left_trigger_deadzone, 0.0f, 1.0f,
                                       "%.2f")) {
                  c_left_trigger_deadzone->SetConfigValue(
                      static_cast<double>(left_trigger_deadzone));
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_left_trigger_deadzone->description();
                }

                float right_trigger_deadzone = static_cast<float>(
                    c_right_trigger_deadzone->GetTypedConfigValue());
                if (ImGui::SliderFloat("Right Trigger Deadzone",
                                       &right_trigger_deadzone, 0.0f, 1.0f,
                                       "%.2f")) {
                  c_right_trigger_deadzone->SetConfigValue(
                      static_cast<double>(right_trigger_deadzone));
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_right_trigger_deadzone->description();
                }

                if (ImGui::Checkbox("Controller Vibration",
                                    c_vibration->current_value())) {
                  c_vibration->SetConfigValue(
                      !c_vibration->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_vibration->description();
                }

                auto c_guide_button = dynamic_cast<cvar::ConfigVar<bool>*>(
                    cvar::ConfigVars->find("guide_button")->second);
                if (ImGui::Checkbox("Forward Guide Button",
                                    c_guide_button->current_value())) {
                  c_guide_button->SetConfigValue(
                      !c_guide_button->GetTypedConfigValue());
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = c_guide_button->description();
                }
              }

              if (settings_selected_section == 11) {
                auto cl = dynamic_cast<cvar::ConfigVar<std::string>*>(
                    cvar::ConfigVars->find("cl")->second);
                std::string cl_text = (std::string)cl->GetTypedConfigValue();
                if (cl_text != cl_buffer_ && cl_buffer_[0] == '\0') {
                  std::strncpy(cl_buffer_, cl_text.c_str(),
                               sizeof(cl_buffer_) - 1);
                  cl_buffer_[sizeof(cl_buffer_) - 1] = '\0';
                }

                if (ImGui::Button("Set CL")) {
                  UWP::ShowKeyboard();
                  ImGui::SetKeyboardFocusHere();
                }

                ImGui::SameLine();
                if (ImGui::InputText("##cl-text", cl_buffer_, 128)) {
                  cl->SetConfigValue(cl_buffer_);
                  config::SaveConfig();
                }

                if (ImGui::IsItemFocused()) {
                  tooltip = cl->description();
                }
              }

              if (settings_selected_section == 13 && emulator_window_.dsu_) {
                emulator_window_.dsu_->DrawSettings();
                tooltip.clear();
              }

              if (settings_selected_section == 14 && emulator_window_.screenscraper_) {
                emulator_window_.screenscraper_->DrawSettings(
                    MakeScreenScraperTarget(std::filesystem::u8path(selected_game_path_), selected_game_name_),
                    [this]() { PickStorageFolder(StorageFolderTarget::kMedia); },
                    bool(storage_folder_result_));
                if (!storage_path_status_.empty()) ImGui::TextWrapped("%s", storage_path_status_.c_str());
                tooltip.clear();
              }

              if (settings_selected_section == 12) {
                if (DrawTextEffectButton("Network Settings")) {
                  emulator_window_.ToggleNetplaySettingsDialog();
                }
                if (DrawTextEffectButton("Network Status")) {
                  emulator_window_.ToggleNetplayStatusDialog();
                }
                if (DrawTextEffectButton("Friends and Sessions")) {
                  emulator_window_.ToggleFriendsDialog();
                }
                if (DrawTextEffectButton("Manage Profiles")) {
                  emulator_window_.ToggleProfilesConfigDialog();
                }
                tooltip =
                    "Configure netplay, inspect connectivity, and manage "
                    "friends and Xbox Live-enabled profiles.";
              }

              ImGui::PopItemWidth();
              ImGui::PopTextWrapPos();
              ImGui::PopStyleVar(2);
              ImGui::PopStyleColor();
              if (disable_right_nav) {
                ImGui::PopItemFlag();
              }

              ImGui::EndTable();
            }
            ImGui::Unindent(settings_content_offset_x);
            ImGui::EndChild();
          }
          ImGui::PopStyleVar();
        }

        if (active_frontend_page_ == FrontendPage::kPaths) {
          StopVideoBackground();
          const float margin = 40.0f;
          const ImVec2 origin = ImGui::GetCursorPos();
          const ImVec2 available = ImGui::GetContentRegionAvail();
          ImGui::SetCursorPos(ImVec2(origin.x + margin, origin.y + margin));
          ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
          if (ImGui::BeginChild(
                  "##paths_editor_plane",
                  ImVec2(std::max(1.0f, available.x - margin * 2.0f),
                         std::max(1.0f, available.y - margin * 2.0f)),
                  false, ImGuiWindowFlags_NoScrollbar)) {
            if (paths_focus_requested) {
              InitializeGuideNavigation();
              paths_focus_requested = false;
            }
            ImGui::PushTextWrapPos(0.0f);
            const auto draw_storage_selector =
                [&](const char* label, const char* id,
                    const std::string& current_path, const char* development,
                    const char* usb, StorageFolderTarget target,
                    bool additional = false) {
                  const char* preview =
                      additional ? "Choose location"
                      : UWP::SameStoragePath(current_path, development)
                          ? "Development files"
                      : UWP::SameStoragePath(current_path, usb) ? "USB path"
                                                             : "Custom";
                  ImGui::TextUnformatted(label);
                  ImGui::SetNextItemWidth(std::min(
                      ImGui::GetContentRegionAvail().x, 580.0f * display_scale));
                  if (ImGui::BeginCombo(id, preview)) {
                    const char* choices[] = {"Development files", "USB path",
                                             "Custom"};
                    for (int index = 0; index < 3; ++index) {
                      const bool selected = !additional &&
                                            std::strcmp(preview, choices[index]) == 0;
                      if (ImGui::Selectable(choices[index], selected)) {
                        if (index == 2) {
                          PickStorageFolder(target);
                        } else {
                          ApplyStorageFolder(target,
                                             index == 0 ? development : usb);
                        }
                      }
                      if (selected) {
                        ImGui::SetItemDefaultFocus();
                      }
                    }
                    ImGui::EndCombo();
                  }
                  if (!additional && !current_path.empty()) {
                    ImGui::TextWrapped("%s", current_path.c_str());
                  }
                  ImGui::Spacing();
                };

            auto paths = UWP::GetPaths();
            draw_storage_selector(
                "Games directory", "##primary_games_location",
                paths.empty() ? UWP::kDefaultGameDirectory : paths.front(),
                UWP::kDevelopmentGameDirectory, UWP::kDefaultGameDirectory,
                StorageFolderTarget::kPrimaryGames);
            draw_storage_selector(
                "Add another game folder", "##additional_games_location", {},
                UWP::kDevelopmentGameDirectory, UWP::kDefaultGameDirectory,
                StorageFolderTarget::kAdditionalGames, true);
            if (draw_text_button("##refresh_paths", "Refresh List")) {
              UWP::RefreshPaths();
              cached_game_title_ids_.clear();
              storage_path_status_ = "Game list refreshed.";
            }
            paths = UWP::GetPaths();
            for (size_t index = 1; index < paths.size(); ++index) {
              ImGui::PushID(static_cast<int>(index));
              ImGui::TextWrapped("%s", paths[index].c_str());
              if (draw_text_button("##remove_game_folder", "Remove Folder")) {
                paths.erase(paths.begin() + index);
                UWP::SetGamePaths(paths);
                cached_game_title_ids_.clear();
                ImGui::PopID();
                break;
              }
              ImGui::PopID();
            }

            ImGui::Spacing();
            auto* storage_variable =
                dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(
                    cvar::ConfigVars->find("storage_root")->second);
            auto config_folder = storage_variable->GetTypedConfigValue();
            if (config_folder.empty()) {
              config_folder = emulator_window_.emulator()->storage_root();
            } else if (config_folder.filename() == "storage") {
              config_folder = config_folder.parent_path();
            }
            ImGui::BeginDisabled(emulator_window_.emulator()->is_title_open());
            draw_storage_selector(
                "Config folders", "##config_folders_location",
                xe::path_to_utf8(config_folder), "D:/DevelopmentFiles/Xenia",
                "E:/Xenia", StorageFolderTarget::kConfig);
            ImGui::EndDisabled();
            if (!storage_path_status_.empty()) {
              ImGui::Spacing();
              ImGui::TextWrapped("%s", storage_path_status_.c_str());
            }
            ImGui::PopTextWrapPos();
          }
          ImGui::EndChild();
          ImGui::PopStyleVar();
        }

        if (active_frontend_page_ == FrontendPage::kAbout) {
          StopVideoBackground();
          if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
              key_pressed(ImGuiKey_GamepadFaceRight, false)) {
            about_focus_requested = true;
          }

          const float about_plane_margin = 40.0f;
          ImVec2 about_plane_origin = ImGui::GetCursorPos();
          ImVec2 about_plane_avail = ImGui::GetContentRegionAvail();
          ImVec2 about_plane_size(
              std::max(0.0f, about_plane_avail.x - (about_plane_margin * 2.0f)),
              std::max(0.0f,
                       about_plane_avail.y - (about_plane_margin * 2.0f)));
          ImGui::SetCursorPos(
              ImVec2(about_plane_origin.x + about_plane_margin,
                     about_plane_origin.y + about_plane_margin));
          ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
          if (ImGui::BeginChild("##about_editor_plane", about_plane_size,
                                false)) {
            if (about_focus_requested) {
              ImGui::SetWindowFocus();
            }
            const float about_content_offset_x = 15.0f * display_scale;
            ImGui::Indent(about_content_offset_x);

            const std::string about_text = fmt::format(
                "Xenia Canary Netplay UWP {}\n"
                "A Unofficial fork of Xenia focusing on Xbox support and a "
                "blades style frontend.\n",
                UWP::GetPackageVersion());
            ImGui::TextWrapped("%s", about_text.c_str());

            ImGui::Spacing();

            if (about_focus_requested) {
              ImGui::SetKeyboardFocusHere();
            }
            if (draw_text_button("##upstream_donations_button",
                                 "Upstream Donations")) {
              LaunchWebBrowser("https://www.patreon.com/xenia_project");
            }
            if (ImGui::IsItemFocused()) {
              about_focus_requested = false;
            }
            ImGui::Spacing();
            if (draw_text_button("##xbox_emulation_hub_button",
                                 "Xbox Emulation Hub")) {
              LaunchWebBrowser("https://discord.gg/WCmxvvxHqu");
            }
            ImGui::Spacing();
            if (draw_text_button("##emulation_revival_button",
                                 "Emulation Revival")) {
              LaunchWebBrowser("https://discord.gg/Rr6vKuNjam");
            }
            ImGui::Spacing();
            if (draw_text_button("##get_source_code_button",
                                 "Get the Source Code")) {
              LaunchWebBrowser(
                  "https://github.com/danprice142/xenia-canary-uwp");
            }

            ImGui::Spacing();
            static int dev_store_preview_index = -1;
            const char* dev_store_options[] = {"XB Dev Store",
                                               "Revival Dev Store"};
            const char* dev_store_urls[] = {
                "https://xbdev.store/",
                "https://emulationrevival.github.io/index.html",
            };
            const char* dev_store_preview =
                dev_store_preview_index >= 0
                    ? dev_store_options[dev_store_preview_index]
                    : "Our App Stores";
            ImGui::PushItemWidth(220.0f * display_scale);
            {
              ScopedAccentComboStyle accent_combo_style;
              if (ImGui::BeginCombo("##app_stores_combo", dev_store_preview)) {
                for (int i = 0; i < 2; ++i) {
                  bool selected = dev_store_preview_index == i;
                  if (ImGui::Selectable(dev_store_options[i], selected)) {
                    dev_store_preview_index = i;
                    LaunchWebBrowser(dev_store_urls[i]);
                  }
                  if (selected) {
                    ImGui::SetItemDefaultFocus();
                  }
                }
                ImGui::EndCombo();
              }
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemFocused()) {
              about_focus_requested = false;
            }

            ImGui::Spacing();
            ImGui::TextWrapped(
                "This UWP Xbox Port was originally made by SirMangler and the "
                "hardwork of all the Xenia Project contributers over the "
                "years\n");

            ImGui::EndChild();
            ImGui::Unindent(about_content_offset_x);
          }
          ImGui::PopStyleVar();
        }
      }
      ImGui::EndChild();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor();

      ImGui::TableNextColumn();
      if (ImGui::BeginChild("##right_rail", ImVec2(0, 0), false,
                            ImGuiWindowFlags_NoScrollbar)) {
        draw_nav_button("##paths_tab", FrontendPage::kPaths);
        draw_nav_button("##about_tab", FrontendPage::kAbout);
        ImGui::Dummy(ImVec2(0.0f, 24.0f * display_scale));
      }
      ImGui::EndChild();

      ImGui::EndTable();
    }

    frontend_splitter.Merge(frontend_draw_list);

    const float ux = io.DisplaySize.x / 1024.0f;
    const float uy = io.DisplaySize.y / 576.0f;

    auto draw_rotated_text = [frontend_draw_list](const char* text,
                                                  ImVec2 anchor,
                                                  float font_size, ImU32 color,
                                                  float rotation_radians) {
      DrawRotatedTextWithConfiguredEffect(frontend_draw_list, ImGui::GetFont(),
                                          font_size, anchor, color, text,
                                          rotation_radians);
    };

    auto draw_button_icon = [frontend_draw_list, this](
                                char letter, ImVec2 center, float size) {
      auto tex = GetOrCreateButtonTexture(letter);
      if (!tex) {
        return;
      }
      const ImVec2 half(size * 0.5f, size * 0.5f);
      frontend_draw_list->AddImage(
          reinterpret_cast<ImTextureID>(tex.get()),
          ImVec2(center.x - half.x, center.y - half.y),
          ImVec2(center.x + half.x, center.y + half.y));
    };

    const char* overlay_header_text = "Select a Game";
    switch (active_frontend_page_) {
      case FrontendPage::kGameList:
        overlay_header_text = "Select a Game";
        break;
      case FrontendPage::kSettings:
        overlay_header_text = "Make some Tweaks";
        break;
      case FrontendPage::kPaths:
        overlay_header_text = "Choose your Path";
        break;
      case FrontendPage::kAbout:
        overlay_header_text = "Get the Info";
        break;
      default:
        overlay_header_text = "Select a Game";
        break;
    }

    frontend_draw_list->PushClipRect(ImVec2(0.0f, 0.0f), io.DisplaySize, false);

    DrawTextWithConfiguredEffect(frontend_draw_list, ImGui::GetFont(),
                                 26.0f * uy, ImVec2(150.0f * ux, 18.0f * uy),
                                 IM_COL32(228, 228, 228, 255),
                                 overlay_header_text);
    constexpr float kRotateClockwise90 = -3.14159265f * 0.5f;
    constexpr float kRotateCounterClockwise90 = 3.14159265f * 0.5f;

    struct RailSlot {
      ImVec2 pos;
      float rotation;
      float font_size;
    };
    const RailSlot slots[4] = {{ImVec2(125.0f * ux, 115.0f * uy),
                                kRotateCounterClockwise90, 26.0f * uy},
                               {ImVec2(81.0f * ux, 129.0f * uy),
                                kRotateCounterClockwise90, 22.0f * uy},
                               {ImVec2(926.0f * ux, 137.0f * uy),
                                kRotateCounterClockwise90, 26.0f * uy},
                               {ImVec2(965.0f * ux, 147.0f * uy),
                                kRotateCounterClockwise90, 22.0f * uy}};

    const char* tab_labels[4] = {"games", "settings", "paths", "about"};
    int active_index = static_cast<int>(active_frontend_page_);

    // Check if tabs text should be hidden
    auto c_hide_tabs = dynamic_cast<cvar::ConfigVar<bool>*>(
        cvar::ConfigVars->find("ui_hide_tabs_text")->second);
    bool hide_tabs_text =
        c_hide_tabs ? c_hide_tabs->GetTypedConfigValue() : false;

    if (!hide_tabs_text) {
      for (int i = 0; i < 4; ++i) {
        int slot_index = (active_index + i) % 4;
        draw_rotated_text(tab_labels[slot_index], slots[i].pos,
                          slots[i].font_size, IM_COL32(0, 0, 0, 255),
                          slots[i].rotation);
      }
    }
    const float footer_text_size = 13.5f * uy;
    const float footer_icon_size = 15.6f * uy;
    xe::app::DrawFooterPrompt(frontend_draw_list, GetOrCreateButtonTexture('Y'),
                              footer_text_size, footer_icon_size, "Search",
                              526.0f * uy, 178.0f * ux, -13.0f * ux);
    xe::app::DrawFooterPrompt(frontend_draw_list, GetOrCreateButtonTexture('X'),
                              footer_text_size, footer_icon_size, "Game Menu",
                              543.0f * uy, 165.0f * ux, -17.0f * ux);
    xe::app::DrawFooterPrompt(frontend_draw_list, GetOrCreateButtonTexture('B'),
                              footer_text_size, footer_icon_size, "Back",
                              526.0f * uy, 816.0f * ux, 35.0f * ux);
    xe::app::DrawFooterPrompt(frontend_draw_list, GetOrCreateButtonTexture('A'),
                              footer_text_size, footer_icon_size, "Select",
                              543.0f * uy, 825.0f * ux, 42.0f * ux);
    if (auto logo = GetOrCreateXeniaLogo()) {
      const float logo_aspect = logo->height > 0
                                    ? static_cast<float>(logo->width) /
                                          static_cast<float>(logo->height)
                                    : 1.0f;
      const float target_height = 42.0f * uy;
      const float target_width = target_height * logo_aspect;
      const ImVec2 logo_center(826.0f * ux, 34.0f * uy);
      const ImVec2 logo_half_size(target_width * 0.5f, target_height * 0.5f);
      const ImVec2 logo_min(logo_center.x - logo_half_size.x,
                            logo_center.y - logo_half_size.y);
      const ImVec2 logo_max(logo_center.x + logo_half_size.x,
                            logo_center.y + logo_half_size.y);
      frontend_draw_list->AddImage(reinterpret_cast<ImTextureID>(logo.get()),
                                   logo_min, logo_max);
    }

    DrawNoProfilePrompt(io);

    frontend_draw_list->PopClipRect();
  }
  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
  ImGui::EndDisabled();

  bool editor_closed = false;
  if (scraper.MetadataEditorOpen()) {
    editor_closed = !scraper.DrawMetadataEditor(imgui_drawer());
  } else if (patch_editor_ && patch_editor_->is_open()) {
    if (patch_editor_->Draw(imgui_drawer())) {
      emulator_window_.emulator()->patcher()->ReloadPatches();
    }
    editor_closed = !patch_editor_->is_open();
  } else if (title_update_dialog_ && title_update_dialog_->is_open()) {
    editor_closed = !title_update_dialog_->Draw(emulator_window_.emulator(), imgui_drawer());
  }
  if (editor_closed) {
    show_game_context_menu_ = true;
    game_context_menu_focus_requested_ = true;
  }

  if (launch_after_draw && !game_editor_open && !launch_path_after_draw.empty()) {
    emulator_window_.RunTitle(launch_path_after_draw);
    if (!emulator_window_.emulator()->is_title_open()) {
      imgui_drawer()->AddDialog(this);
    }
    return;
  }
}

std::shared_ptr<ui::ImmediateTexture>
EmulatorWindow::WinRTFrontendDialog::GetOrCreateThemeTexture(
    const std::string& path, std::string& loaded_path,
    std::shared_ptr<ui::ImmediateTexture>& texture) {
  if (!texture || loaded_path != path) {
    imgui_drawer()->RetainTextureForFrame(texture);
    texture = LoadOverlayTexture(imgui_drawer(), path.c_str(), false);
    loaded_path = path;
  }
  imgui_drawer()->RetainTextureForFrame(texture);
  return texture;
}

std::shared_ptr<ui::ImmediateTexture>
EmulatorWindow::WinRTFrontendDialog::GetOrCreateBackground() {
  return GetOrCreateThemeTexture(
      ResolveConfiguredThemeAssetPath(ThemeAsset::kOverlay,
                                      "ui_interface_overlay_color"),
      background_texture_path_, background_tex_);
}

std::shared_ptr<ui::ImmediateTexture>
EmulatorWindow::WinRTFrontendDialog::GetOrCreateBackgroundFallback() {
  return GetOrCreateThemeTexture(
      ResolveConfiguredThemeAssetPath(ThemeAsset::kBackground,
                                      "ui_background_color"),
      background_fallback_texture_path_, background_fallback_tex_);
}

std::shared_ptr<ui::ImmediateTexture>
EmulatorWindow::WinRTFrontendDialog::GetOrCreateImageTexture(
    const std::string& image_path) {
  std::error_code file_error;
  if (image_path.empty() || emulator_window_.immediate_drawer_ == nullptr ||
      !std::filesystem::exists(std::filesystem::u8path(image_path), file_error)) {
    return nullptr;
  }

  auto cache_it = game_image_textures_.find(image_path);
  if (cache_it != game_image_textures_.end() && cache_it->second != nullptr) {
    imgui_drawer()->RetainTextureForFrame(cache_it->second);
    return cache_it->second;
  }

  int width = 0, height = 0, comp = 0;
  auto data = stbi_load(image_path.c_str(), &width, &height, &comp, 4);
  if (!data || width <= 0 || height <= 0) {
    stbi_image_free(data);
    return nullptr;
  }

  auto tex = emulator_window_.immediate_drawer_->CreateTexture(
      static_cast<uint32_t>(width), static_cast<uint32_t>(height),
      xe::ui::ImmediateTextureFilter::kLinear, false, data);
  stbi_image_free(data);
  if (tex == nullptr) {
    return nullptr;
  }

  game_image_textures_[image_path] = std::move(tex);
  imgui_drawer()->RetainTextureForFrame(game_image_textures_[image_path]);
  return game_image_textures_[image_path];
}

std::shared_ptr<ui::ImmediateTexture>
EmulatorWindow::WinRTFrontendDialog::GetOrCreateXeniaLogo() {
  return GetOrCreateThemeTexture(
      ResolveConfiguredThemeAssetPath(ThemeAsset::kLogo,
                                      "ui_interface_overlay_color"),
      xenia_logo_texture_path_, xenia_logo_tex_);
}

std::shared_ptr<ui::ImmediateTexture>
EmulatorWindow::WinRTFrontendDialog::GetOrCreateButtonTexture(char button) {
  std::shared_ptr<ui::ImmediateTexture>* cached_tex = nullptr;
  const char* asset_file = nullptr;
  switch (button) {
    case 'A':
      cached_tex = &button_a_tex_;
      asset_file = "A.png";
      break;
    case 'B':
      cached_tex = &button_b_tex_;
      asset_file = "B.png";
      break;
    case 'X':
      cached_tex = &button_x_tex_;
      asset_file = "X.png";
      break;
    case 'Y':
      cached_tex = &button_y_tex_;
      asset_file = "Y.png";
      break;
    default:
      return nullptr;
  }

  if (*cached_tex != nullptr) {
    return *cached_tex;
  }

  if (emulator_window_.immediate_drawer_ == nullptr) {
    return nullptr;
  }

  const std::string path = std::string("Assets/") + asset_file;
  if (!std::filesystem::exists(path)) {
    return nullptr;
  }

  int width = 0, height = 0, comp = 0;
  auto data = stbi_load(path.c_str(), &width, &height, &comp, 4);
  if (!data || width <= 0 || height <= 0) {
    stbi_image_free(data);
    return nullptr;
  }

  auto tex = emulator_window_.immediate_drawer_->CreateTexture(
      static_cast<uint32_t>(width), static_cast<uint32_t>(height),
      xe::ui::ImmediateTextureFilter::kLinear, false, data);
  stbi_image_free(data);
  *cached_tex = std::move(tex);

  return *cached_tex;
}
#endif  // XE_PLATFORM_WINRT

}  // namespace app
}  // namespace xe
