// Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
// PolyForm Noncommercial License 1.0.0: not for sale, and this notice stays in every copy.
//
// The Xbox app's start-up on Xenia Edge. Edge's own front end is a wxWidgets desktop window, which the Xbox has no
// use for (and can't run): Game Vault picks the game, so this app only sets up the emulator in the app's CoreWindow,
// starts the game Game Vault sent (gamevault-xenia: link, see App.cpp) and starts the next one when Game Vault sends
// another while a game runs. The steps follow Edge's xenia_main.cc: Setup, SetupSubsystems, the presenter on the
// window, LaunchPath; RelaunchTitle for the next game.

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/platform.h"

#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/apu/xaudio2/xaudio2_audio_system.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/emulator.h"
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#include "xenia/hid/input_system.h"
#include "xenia/hid/xinput/xinput_hid.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_module.h"
#include "third_party/imgui/imgui.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context.h"

#include "UWPUtil.h"
#include "XeniaUWP.h"

DEFINE_string(apu, "any", "Audio system. Use: [any, nop, xaudio2]", "APU");
DEFINE_string(gpu, "any", "Graphics system. Use: [any, d3d12]", "GPU");
DEFINE_string(hid, "any", "Input system. Use: [any, nop, xinput]", "HID");

DEFINE_path(storage_root, "",
            "Root path for persistent internal data storage (config, etc.), or "
            "empty for the app's LocalState folder.",
            "Storage");
DEFINE_path(content_root, "",
            "Root path for guest content storage (saves, etc.), or empty to use "
            "the content folder under the storage root.",
            "Storage");
DEFINE_path(cache_root, "",
            "Root path for files used to speed up the emulator, or empty for "
            "the cache_host folder under the storage root.",
            "Storage");
DEFINE_transient_path(target, "",
                      "Specifies the target .xex or .iso to execute.",
                      "General");

namespace xe {
namespace app {

namespace {
// Draw order on the window: the emulator's own UI above the game.
constexpr size_t kZOrderImGui = 0;
constexpr size_t kZOrderHidInput = 1;

// Picks a backend by name, or the first one that comes up (as xenia_main.cc).
template <typename T, typename... Args>
class Factory {
 public:
  template <typename DT>
  void Add(const std::string_view name) {
    creators_.push_back({std::string(name), [](Args... args) -> std::unique_ptr<T> {
                           if (!DT::IsAvailable()) return nullptr;
                           return std::make_unique<DT>(std::forward<Args>(args)...);
                         }});
  }
  void Add(const std::string_view name,
           std::function<std::unique_ptr<T>(Args...)> instantiate) {
    creators_.push_back({std::string(name), std::move(instantiate)});
  }
  std::unique_ptr<T> Create(const std::string_view name, Args... args) {
    for (const auto& c : creators_) {
      if (name == c.name) {
        if (auto instance = c.instantiate(args...)) return instance;
      }
    }
    for (const auto& c : creators_) {
      if (auto instance = c.instantiate(args...)) return instance;
    }
    return nullptr;
  }
  std::vector<std::unique_ptr<T>> CreateAll(const std::string_view name,
                                            Args... args) {
    std::vector<std::unique_ptr<T>> instances;
    for (const auto& c : creators_) {
      if (name == "any" || name == c.name) {
        if (auto instance = c.instantiate(args...)) {
          instances.emplace_back(std::move(instance));
        }
      }
    }
    // A name this app doesn't have (Edge's desktop default hid = "sdl" ended
    // up in the config): all of the ones it has, rather than none.
    if (instances.empty() && name != "nop") {
      for (const auto& c : creators_) {
        if (auto instance = c.instantiate(args...)) {
          instances.emplace_back(std::move(instance));
        }
      }
    }
    return instances;
  }

 private:
  struct Creator {
    std::string name;
    std::function<std::unique_ptr<T>(Args...)> instantiate;
  };
  std::vector<Creator> creators_;
};
}  // namespace

// Game Vault's "Show frame rate" (fps=1 in the link that sent the game): the
// game's frames per second (frames it sent to the screen, as in xemu's
// counter), measured over half a second, small in the top left corner. Drawn
// on the foreground list: no window, so the controller stays with the game.
class FpsOverlay final : public ui::ImGuiDialog {
 public:
  FpsOverlay(ui::ImGuiDrawer* drawer, std::function<ui::Presenter*()> presenter)
      : ui::ImGuiDialog(drawer), presenter_(std::move(presenter)) {}
  void set_shown(bool shown) { shown_ = shown; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    ui::Presenter* presenter = presenter_();
    if (!shown_ || !presenter) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const uint64_t frames = presenter->guest_output_refresh_count();
    if (start_ == std::chrono::steady_clock::time_point()) {
      start_ = now, frames_ = frames;
    } else if (now - start_ >= std::chrono::milliseconds(500)) {
      const double seconds =
          std::chrono::duration<double>(now - start_).count();
      snprintf(text_, sizeof(text_), "%.0f FPS",
               double(frames - frames_) / seconds);
      start_ = now, frames_ = frames;
    }
    if (!text_[0]) {
      return;
    }
    ImDrawList* list = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * 1.6f;
    const ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text_);
    const ImVec2 at(io.DisplaySize.x * 0.02f, io.DisplaySize.y * 0.03f);
    const float pad = size * 0.3f;
    list->AddRectFilled(ImVec2(at.x - pad, at.y - pad * 0.5f),
                        ImVec2(at.x + text.x + pad, at.y + text.y + pad * 0.5f),
                        IM_COL32(0, 0, 0, 150), pad);
    list->AddText(font, size, at, IM_COL32(255, 255, 255, 235), text_);
  }

 private:
  std::function<ui::Presenter*()> presenter_;
  bool shown_ = false;
  std::chrono::steady_clock::time_point start_;
  uint64_t frames_ = 0;
  char text_[16] = {};
};

// Game Vault: Xbox 360 title updates and DLC from the player's USB drive. Game Vault finds the packages (in "Game
// Vault\xbox 360 add-ons" on the drive), copies the new ones into the shared folder and lists them in "xbox 360
// add-ons.txt" there; each start installs the ones not installed yet, the way Xenia's own "Install content" does, and writes what happened
// to "xbox 360 add-ons done.txt" there ("<size>\t<installed|failed ...>\t<path>" per package) for Game Vault to show.
// A package that failed is tried again only when its size changes (a new copy), so a broken file costs one try.
static void InstallGameVaultAddOns(Emulator* emulator) {
  // what happened, also in Xenia's own LocalState (Device Portal can read that one; the shared folder's log it cannot)
  std::ofstream trace(std::filesystem::path(xe::to_path(UWP::GetLocalState())) / "gv_addons.txt", std::ios::trunc);
  const std::string games = UWP::GetPublisherGames();
  trace << "shared folder: " << games << "\n";
  if (games.empty()) return;
  const auto shared = std::filesystem::path(xe::to_path(games));
  const auto list = shared / "xbox 360 add-ons.txt", done = shared / "xbox 360 add-ons done.txt";
  std::ifstream in(list);
  trace << "list " << (in ? "read" : "not there") << "\n";
  if (!in) return;
  std::map<std::string, std::pair<uint64_t, std::string>> before;   // path -> size, result
  if (std::ifstream d(done); d) {
    std::string line;
    while (std::getline(d, line)) {
      const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
      if (b == std::string::npos) continue;
      before[line.substr(b + 1)] = {std::strtoull(line.substr(0, a).c_str(), nullptr, 10), line.substr(a + 1, b - a - 1)};
    }
  }
  std::map<std::string, std::pair<uint64_t, std::string>> after;
  // the results so far plus `extra`, written at once: a package that brings Xenia down is on file as failed before
  // it is tried, so the next start skips it instead of going down again
  auto write_done = [&](const std::string* extra, uint64_t extra_size) {
    std::map<std::string, std::pair<uint64_t, std::string>> all = after;
    for (auto& [path, v] : before) all.emplace(path, v);   // packages on a drive not plugged in now keep theirs
    if (extra) all[*extra] = {extra_size, "failed (Xenia stopped while installing it)"};
    std::ofstream out(done, std::ios::trunc);
    for (auto& [path, v] : all) out << v.first << '\t' << v.second << '\t' << path << '\n';
  };
  std::string line;
  int installed = 0, failed = 0;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) continue;
    // "<copy in the shared folder>\t<the file on the USB drive>\t<size>": Xenia cannot open the drive's file by its
    // path, so Game Vault copies it here first; results are kept under the drive's path
    const size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
    if (t2 == std::string::npos) continue;
    const std::string copy = line.substr(0, t1), original = line.substr(t1 + 1, t2 - t1 - 1);
    const uint64_t size = std::strtoull(line.substr(t2 + 1).c_str(), nullptr, 10);
    const auto path = std::filesystem::path(xe::to_path(copy));
    std::error_code ec;
    auto was = before.find(original);
    trace << "line [" << line << "]\n";
    if (was != before.end() && was->second.first == size) {
      trace << "  done before: " << was->second.second << "\n";
      after[original] = was->second;   // installed before, or failed with this very file
      std::filesystem::remove(path, ec);
      continue;
    }
    trace << "package " << original << " (copy " << copy << ", " << size << " bytes)\n";
    if (!std::filesystem::exists(path, ec)) {   // Game Vault has not copied it yet
      trace << "  copy not there\n";
      continue;
    }
    std::string result;
    write_done(&original, size);
    XELOGI("Game Vault: installing add-on {}", original);
    try {
      Emulator::ContentInstallEntry entry(path);
      const X_STATUS status = emulator->InstallContentPackage(path, entry);
      if (status == X_STATUS_SUCCESS) {
        result = "installed";
        installed++;
      } else {
        result = fmt::format("failed {:08X}{}{}", status, entry.installation_error_message_.empty() ? "" : " ",
                             entry.installation_error_message_);
        failed++;
      }
    } catch (const std::exception& e) {
      result = std::string("failed ") + e.what();
      failed++;
    } catch (...) {
      result = "failed";
      failed++;
    }
    XELOGI("Game Vault: add-on {}: {}", original, result);
    trace << "  " << result << "\n";
    after[original] = {size, result};
    std::filesystem::remove(path, ec);   // the copy has done its job (the installed content is in Xenia now)
  }
  write_done(nullptr, 0);
  if (installed || failed) XELOGI("Game Vault: add-ons: {} installed, {} failed", installed, failed);
}

// Xenia opened without a game (from the Xbox's app list): a line saying where
// games are started, rather than a black screen. Gone once a game runs.
class NoGameMessage final : public ui::ImGuiDialog {
 public:
  NoGameMessage(ui::ImGuiDrawer* drawer, Emulator* emulator)
      : ui::ImGuiDialog(drawer), emulator_(emulator) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (emulator_->is_title_open()) {
      Close();  // (deletes this dialog)
      return;
    }
    const char* lines[] = {"Start an Xbox 360 game from Game Vault.",
                           "Xenia opens with it."};
    ImDrawList* list = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float y = io.DisplaySize.y * 0.45f;
    for (int i = 0; i < 2; ++i) {
      const float size = ImGui::GetFontSize() * (i ? 1.4f : 2.0f);
      const ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0.0f, lines[i]);
      list->AddText(font, size,
                    ImVec2((io.DisplaySize.x - text.x) * 0.5f, y),
                    IM_COL32(255, 255, 255, i ? 180 : 235), lines[i]);
      y += text.y * 1.5f;
    }
  }

 private:
  Emulator* emulator_;
};

class GameVaultXeniaApp final : public ui::WindowedApp {
 public:
  static std::unique_ptr<ui::WindowedApp> Create(
      ui::WindowedAppContext& app_context) {
    return std::unique_ptr<ui::WindowedApp>(
        new GameVaultXeniaApp(app_context));
  }

  bool OnInitialize() override;

 protected:
  void OnDestroy() override;

 private:
  explicit GameVaultXeniaApp(ui::WindowedAppContext& app_context)
      : ui::WindowedApp(app_context, "xenia") {}

  static std::unique_ptr<apu::AudioSystem> CreateAudioSystem(
      cpu::Processor* processor);
  static std::unique_ptr<gpu::GraphicsSystem> CreateGraphicsSystem();
  static std::vector<std::unique_ptr<hid::InputDriver>> CreateInputDrivers(
      ui::Window* window);

  void EmulatorThread();
  // UI thread: the graphics system's frames go to the window (again after a
  // relaunch, which makes a new graphics system).
  void SetupPresenter();
  void ShutdownPresenter();
  // UI thread, every pass: Game Vault sent another game while one runs.
  void CompanionTick();
  void StartTitle(const std::filesystem::path& path);

  std::unique_ptr<Emulator> emulator_;
  std::unique_ptr<ui::Window> window_;
  std::unique_ptr<ui::ImGuiDrawer> imgui_drawer_;
  std::unique_ptr<ui::ImmediateDrawer> immediate_drawer_;
  ui::Presenter* presenter_painting_ = nullptr;
  FpsOverlay* fps_overlay_ = nullptr;  // owned by the ImGui drawer
  std::atomic<bool> emulator_ready_{false};
  std::atomic<bool> switching_{false};
  std::thread emulator_thread_;
};

std::unique_ptr<apu::AudioSystem> GameVaultXeniaApp::CreateAudioSystem(
    cpu::Processor* processor) {
  Factory<apu::AudioSystem, cpu::Processor*> factory;
  factory.Add<apu::xaudio2::XAudio2AudioSystem>("xaudio2");
  factory.Add<apu::nop::NopAudioSystem>("nop");
  return factory.Create(cvars::apu, processor);
}

std::unique_ptr<gpu::GraphicsSystem> GameVaultXeniaApp::CreateGraphicsSystem() {
  // Direct3D 12 is the only graphics API of the Xbox's app runtime.
  Factory<gpu::GraphicsSystem> factory;
  factory.Add<gpu::d3d12::D3D12GraphicsSystem>("d3d12");
  return factory.Create(cvars::gpu);
}

std::vector<std::unique_ptr<hid::InputDriver>>
GameVaultXeniaApp::CreateInputDrivers(ui::Window* window) {
  std::vector<std::unique_ptr<hid::InputDriver>> drivers;
  Factory<hid::InputDriver, ui::Window*, size_t> factory;
  factory.Add("xinput", xe::hid::xinput::Create);
  for (auto& driver :
       factory.CreateAll(cvars::hid, window, kZOrderHidInput)) {
    if (XSUCCEEDED(driver->Setup())) {
      drivers.emplace_back(std::move(driver));
    }
  }
  return drivers;
}

bool GameVaultXeniaApp::OnInitialize() {
  Profiler::Initialize();
  Profiler::ThreadEnter("Main");

  // The app's LocalState (filesystem_win.cc: GetUserFolder on the Xbox).
  std::filesystem::path storage_root = cvars::storage_root;
  if (storage_root.empty()) {
    storage_root = xe::filesystem::GetUserFolder();
  }
  storage_root = std::filesystem::absolute(storage_root);
  config::SetupConfig(storage_root);
  XELOGI("Storage root: {}", storage_root);

  std::filesystem::path content_root = cvars::content_root;
  if (content_root.empty()) {
    content_root = storage_root / "content";
  } else if (!content_root.is_absolute()) {
    content_root = storage_root / content_root;
  }
  XELOGI("Content root: {}", content_root);
  // Game Vault: saves come back after a reinstall, and are copied out while
  // Xenia runs
  UWP::SavesAtStart(content_root);

  std::filesystem::path cache_root = cvars::cache_root;
  if (cache_root.empty()) {
    cache_root = storage_root / "cache_host";
  } else if (!cache_root.is_absolute()) {
    cache_root = storage_root / cache_root;
  }
  XELOGI("Host cache root: {}", cache_root);
  XELOGI("Game Vault: Xenia Edge build {}", UWP::GetPackageVersion());

  emulator_ =
      std::make_unique<Emulator>("", storage_root, content_root, cache_root);

  window_ = ui::Window::Create(app_context(), "Xenia", 1920, 1080);
  if (!window_ || !window_->Open()) {
    XELOGE("Failed to open the window");
    return false;
  }
  UWP::RegisterXeniaWindow(window_.get());
  imgui_drawer_ =
      std::make_unique<ui::ImGuiDrawer>(window_.get(), kZOrderImGui);

  emulator_thread_ = std::thread(&GameVaultXeniaApp::EmulatorThread, this);
  return true;
}

void GameVaultXeniaApp::SetupPresenter() {
  auto* graphics = emulator_ ? emulator_->graphics_system() : nullptr;
  ui::Presenter* presenter = graphics ? graphics->presenter() : nullptr;
  if (presenter && presenter_painting_ == presenter) {
    return;
  }
  ShutdownPresenter();
  if (!presenter) {
    return;
  }
  window_->SetPresenter(presenter);
  presenter_painting_ = presenter;
  immediate_drawer_ = graphics->provider()->CreateImmediateDrawer();
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(presenter);
    imgui_drawer_->SetPresenterAndImmediateDrawer(presenter,
                                                  immediate_drawer_.get());
  }
}

void GameVaultXeniaApp::ShutdownPresenter() {
  imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
  immediate_drawer_.reset();
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  presenter_painting_ = nullptr;
}

void GameVaultXeniaApp::StartTitle(const std::filesystem::path& path) {
  // The game Game Vault shared or a network game is there without a look at
  // the drive (a network game's stand-in path is on no drive at all).
  std::error_code ec;
  if (path.empty() || !(xe::filesystem::IsGameVaultFile(path) ||
                        std::filesystem::exists(path, ec))) {
    XELOGE("Game Vault: no game at {} (network game here: {}, shared or "
           "network file: {})",
           path, xe::filesystem::GetNetFile(path) != nullptr,
           xe::filesystem::IsGameVaultFile(path));
    app_context().CallInUIThread([this, path]() {
      ui::ImGuiDialog::ShowMessageBox(
          imgui_drawer_.get(), "Game not found",
          fmt::format("Xenia could not open {}", xe::path_to_utf8(path)));
    });
    return;
  }
  if (auto xam = emulator_->kernel_state()
                      ->GetKernelModule<kernel::xam::XamModule>("xam.xex")) {
    xam->loader_data().host_path = xe::path_to_utf8(path);
  }
  app_context().CallInUIThread([this]() {
    if (!fps_overlay_ && UWP::GetShowFps()) {
      fps_overlay_ = new FpsOverlay(imgui_drawer_.get(),
                                    [this]() { return presenter_painting_; });
    }
    if (fps_overlay_) {
      fps_overlay_->set_shown(UWP::GetShowFps());
    }
  });
  XELOGI("Game Vault: starting {} (frame rate shown: {})", path,
         UWP::GetShowFps());
  const X_STATUS result = emulator_->LaunchPath(path);
  if (XFAILED(result)) {
    XELOGE("Game Vault: starting {} failed: {:08X}", path, result);
  }
}

void GameVaultXeniaApp::EmulatorThread() {
  xe::threading::set_name("Emulator");
  Profiler::ThreadEnter("Emulator");

  X_STATUS result =
      emulator_->Setup(window_.get(), imgui_drawer_.get(), true,
                       CreateAudioSystem, CreateGraphicsSystem,
                       CreateInputDrivers);
  if (XSUCCEEDED(result)) {
    result = emulator_->SetupSubsystems();
  }
  if (XFAILED(result)) {
    XELOGE("Failed to set up the emulator: {:08X}", result);
    app_context().RequestDeferredQuit();
    return;
  }

  // A relaunch makes a new graphics system: its frames go to the window again.
  // Edge binds controllers to the game's player slots only when a driver
  // reports devices (SDL does on hotplug); XInput doesn't, so ask for it now
  // and after each relaunch (new input drivers). Without it the game asks to
  // reconnect the controller.
  emulator_->input_system()->NotifyDevicesChanged();
  {
    // What the controllers look like to the game (a game that sees none asks
    // to reconnect the controller).
    auto* input = emulator_->input_system();
    auto lock = input->lock();
    for (uint32_t slot = 0; slot < 4; ++slot) {
      hid::X_INPUT_CAPABILITIES caps = {};
      const X_RESULT r = input->GetCapabilities(slot, 1, &caps);
      XELOGI("Game Vault: controllers: {} driver(s), player {}: {} device(s) "
             "bound, capabilities {:08X}",
             input->driver_count(), slot + 1,
             input->GetSlotBindings(slot).size(), r);
    }
  }
  emulator_->on_launch.AddListener([this](auto, const auto&) {
    app_context().CallInUIThread([this]() { SetupPresenter(); });
    emulator_->input_system()->NotifyDevicesChanged();
  });
  emulator_->on_before_shutdown.AddListener([this]() {
    app_context().CallInUIThreadSynchronous([this]() { ShutdownPresenter(); });
  });
  // The game quit to the dashboard: back to Game Vault.
  emulator_->set_on_exit_to_dashboard([this]() {
    app_context().CallInUIThread([]() { UWP::RequestQuit(); });
    return false;
  });

  // Title updates and DLC Game Vault found on the USB drive
  InstallGameVaultAddOns(emulator_.get());

  app_context().CallInUIThreadSynchronous([this]() {
    SetupPresenter();
    UWP::RegisterInputSystem(emulator_->input_system());
    UWP::SetCompanionTick([this]() { CompanionTick(); });
    // View + Menu held: back to Game Vault.
    UWP::SetHostGuideAction([this]() {
      UWP::ReturnToFrontend(imgui_drawer_.get());
    });
  });
  emulator_ready_ = true;

  if (!cvars::target.empty()) {
    UWP::ConsumeGamePath();
    StartTitle(cvars::target);
  } else if (UWP::HasGamePath()) {
    StartTitle(std::filesystem::u8path(UWP::ConsumeGamePath()));
  } else {
    app_context().CallInUIThread([this]() {
      new NoGameMessage(imgui_drawer_.get(), emulator_.get());
    });
  }

  // The game's frame rate in the log every 30 s (test runs and player
  // reports show the speed without a screenshot).
  std::thread([this]() {
    uint64_t last = 0;
    auto last_time = std::chrono::steady_clock::now();
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(30));
      auto* presenter = presenter_painting_;
      if (!presenter || !emulator_->is_title_open()) {
        continue;
      }
      const uint64_t frames = presenter->guest_output_refresh_count();
      const auto now = std::chrono::steady_clock::now();
      if (last && frames >= last) {
        XELOGI("Game Vault: frame rate {:.1f}",
               double(frames - last) /
                   std::chrono::duration<double>(now - last_time).count());
      }
      last = frames, last_time = now;
    }
  }).detach();

  emulator_->WaitUntilExit();
}

void GameVaultXeniaApp::CompanionTick() {
  if (!emulator_ready_ || switching_ || !UWP::HasGamePath()) {
    return;
  }
  const auto path = std::filesystem::u8path(UWP::ConsumeGamePath());
  if (!emulator_->is_title_open()) {
    switching_ = true;
    std::thread([this, path]() {
      StartTitle(path);
      switching_ = false;
    }).detach();
    return;
  }
  // Game Vault: the player opened Game Vault from the guide while a game ran
  // here and picked another game. Edge restarts the emulator in this process
  // with the new one (on its own thread, as Edge's front end does).
  XELOGI("Game Vault: another game was sent while a game runs: {}", path);
  switching_ = true;
  std::thread([this, path]() {
    emulator_->RelaunchTitle(xe::path_to_utf8(path), "", 0, {}, false);
    switching_ = false;
  }).detach();
}

void GameVaultXeniaApp::OnDestroy() {
  UWP::SetCompanionTick({});
  UWP::SetHostGuideAction({});
  UWP::RegisterInputSystem(nullptr);
  config::SaveConfig();
  XELOGI("Game Vault: Xenia closing");
  xe::FlushLog();
  // Like Edge and Canary: no orderly teardown of a running title.
  std::quick_exit(EXIT_SUCCESS);
}

}  // namespace app
}  // namespace xe

XE_DEFINE_WINDOWED_APP(xenia, xe::app::GameVaultXeniaApp::Create);
