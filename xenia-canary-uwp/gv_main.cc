// Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
// PolyForm Noncommercial License 1.0.0: not for sale, and this notice stays in every copy.
//
// The Xbox app's start-up on Xenia Edge. Edge's own front end is a wxWidgets desktop window, which the Xbox has no
// use for (and can't run): Game Vault picks the game, so this app only sets up the emulator in the app's CoreWindow,
// starts the game Game Vault sent (gamevault-xenia: link, see App.cpp) and starts the next one when Game Vault sends
// another while a game runs. The steps follow Edge's xenia_main.cc: Setup, SetupSubsystems, the presenter on the
// window, LaunchPath; RelaunchTitle for the next game.

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <set>
#include <vector>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/platform.h"
#include "xenia/base/platform_win.h"

#include <psapi.h>

#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/apu/xaudio2/xaudio2_audio_system.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/memory.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/shader_storage.h"
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#include "xenia/hid/input_system.h"
#include "xenia/hid/xinput/xinput_hid.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/content_manager.h"
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
DEFINE_int32(gv_profile_ms, 0,
             "Game Vault testing: every this many ms, note what each emulated "
             "CPU is running, and log the top spots every 30 s (game "
             "functions, or Xenia's own code as an offset in the exe). 0 = "
             "off.",
             "Logging");

// (the in-game menu's resolution scale; Xenia's own settings)
DECLARE_int32(draw_resolution_scale_x);
DECLARE_int32(draw_resolution_scale_y);

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
namespace gpu {
namespace d3d12 {
void GameVaultTakeSubmitStats(uint64_t out[9]);
void GameVaultTakeGpuTiming(double secs, std::vector<std::string>* lines);
}  // namespace d3d12
}  // namespace gpu
namespace app {

// The frame rate and profile lines also go to gv_live.log in Xenia's own
// LocalState, which Device Portal can read while the game runs (the main log
// is in the shared folder, which only Game Vault can copy out). New each run,
// 2 MB at most.
static void GvLiveLine(const std::string& line) {
  static std::mutex mutex;
  static bool first = true;
  std::lock_guard<std::mutex> lock(mutex);
  const auto path =
      std::filesystem::path(xe::to_path(UWP::GetLocalState())) / "gv_live.log";
  std::error_code ec;
  const bool fresh =
      first || std::filesystem::file_size(path, ec) > (2u << 20);
  first = false;
  std::ofstream(path, fresh ? std::ios::trunc : std::ios::app) << line << '\n';
}
#define GV_LIVE(...)                                      \
  do {                                                    \
    const std::string gv_live = fmt::format(__VA_ARGS__); \
    XELOGI("{}", gv_live);                                \
    GvLiveLine(gv_live);                                  \
  } while (0)

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

// CPU and GPU load for the frame rate counter and the log, since the last
// Sample(). CPU: the whole app's CPU time over all the cores it can use. GPU:
// how busy the thread that runs the 360's GPU (the command processor) is; near
// 100% it never waits for the game, so the GPU side holds the game back.
// Memory: the app's private bytes.
class LoadMeter {
 public:
  explicit LoadMeter(Emulator* emulator) : emulator_(emulator) {}
  // Each emulated CPU's load (the guest scheduler's dispatch threads), in %,
  // "-" for one not running; from the last Sample().
  std::string PerCpu() const { return per_cpu_; }
  void Sample(double* cpu, double* gpu, double* memory_mb) {
    const uint64_t now = Now();
    FILETIME c, e, k, u;
    uint64_t process = 0, thread = 0;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
      process = Ticks(k) + Ticks(u);
    }
    HANDLE gpu_thread = GpuThread();
    if (gpu_thread && GetThreadTimes(gpu_thread, &c, &e, &k, &u)) {
      thread = Ticks(k) + Ticks(u);
    }
    const double wall = double(now - wall_);
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    *cpu = wall_ && wall > 0
               ? 100.0 * double(process - process_) / (wall * cores) : 0;
    *gpu = wall_ && wall > 0 && thread >= thread_
               ? std::min(100.0, 100.0 * double(thread - thread_) / wall) : 0;
    PROCESS_MEMORY_COUNTERS_EX memory = {sizeof(memory)};
    *memory_mb = K32GetProcessMemoryInfo(
                     GetCurrentProcess(),
                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                     sizeof(memory))
                     ? memory.PrivateUsage / (1024.0 * 1024.0) : 0;
    SamplePerCpu(double(now - wall_));
    wall_ = now, process_ = process, thread_ = thread;
  }

 private:
  static uint64_t Ticks(const FILETIME& t) {  // 100 ns
    return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  }
  static uint64_t Now() {
    FILETIME t;
    GetSystemTimeAsFileTime(&t);
    return Ticks(t);
  }
  HANDLE GpuThread() const {
    auto* graphics = emulator_ ? emulator_->graphics_system() : nullptr;
    auto* processor = graphics ? graphics->command_processor() : nullptr;
    auto* worker = processor ? processor->worker_thread() : nullptr;
    auto* thread = worker ? worker->thread() : nullptr;
    return thread ? HANDLE(thread->native_handle()) : nullptr;
  }
  void SamplePerCpu(double wall) {
    auto* kernel = emulator_ ? emulator_->kernel_state() : nullptr;
    auto* scheduler = kernel ? kernel->guest_scheduler() : nullptr;
    per_cpu_.clear();
    for (int i = 0; i < kernel::GuestScheduler::cpu_count(); ++i) {
      auto* thread = scheduler ? scheduler->cpu_host_thread(i) : nullptr;
      FILETIME c, e, k, u;
      uint64_t now = 0;
      bool have = thread && GetThreadTimes(HANDLE(thread->native_handle()),
                                           &c, &e, &k, &u);
      if (have) now = Ticks(k) + Ticks(u);
      if (!per_cpu_.empty()) per_cpu_ += "/";
      if (have && wall_ && wall > 0 && now >= cpu_threads_[i]) {
        per_cpu_ += fmt::format(
            "{:.0f}",
            std::min(100.0, 100.0 * double(now - cpu_threads_[i]) / wall));
      } else {
        per_cpu_ += "-";
      }
      cpu_threads_[i] = now;
    }
  }
  Emulator* emulator_;
  uint64_t wall_ = 0, process_ = 0, thread_ = 0;
  uint64_t cpu_threads_[kernel::GuestScheduler::cpu_count()] = {};
  std::string per_cpu_;
};

// Game Vault testing (gv_profile_ms): where each emulated CPU spends its time.
// The CPU's dispatch thread is stopped for a moment, its instruction pointer
// noted, and it goes on; nothing is allocated while a thread is stopped (it may
// hold the heap's lock). Every 30 s the most frequent spots per CPU go to the
// log: a game function by its address, or Xenia's own code as an offset in
// xeniacanary.exe (looked up in the .pdb on the PC).
class CpuSampler {
 public:
  explicit CpuSampler(Emulator* emulator) : emulator_(emulator) {
    for (auto& samples : samples_) samples.reserve(kMaxSamples);
    HMODULE exe = GetModuleHandleW(nullptr);
    MODULEINFO info = {};
    if (exe && K32GetModuleInformation(GetCurrentProcess(), exe, &info,
                                       sizeof(info))) {
      exe_base_ = uint64_t(info.lpBaseOfDll);
      exe_size_ = info.SizeOfImage;
    }
  }
  void Run(int interval_ms) {
    XELOGI("Game Vault profile: on, every {} ms", interval_ms);
    auto next_report =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
      Sample();
      if (std::chrono::steady_clock::now() >= next_report) {
        Report();
        next_report += std::chrono::seconds(30);
      }
    }
  }

 private:
  static constexpr size_t kMaxSamples = 65536;
  // The 6 emulated CPUs, then the GPU thread (command processor).
  static constexpr int kThreads = kernel::GuestScheduler::cpu_count() + 1;
  struct Spot {
    uint64_t pc;
    uint64_t caller;  // outside Xenia and the game: the first Xenia frame
  };
  bool InExe(uint64_t pc) const {
    return exe_size_ && pc >= exe_base_ && pc < exe_base_ + exe_size_;
  }
  bool InJit(uint64_t pc) const {
    return jit_size_ && pc >= jit_base_ && pc < jit_base_ + jit_size_;
  }
  // While the thread is stopped: its pc and, when that is in Windows or the
  // driver (waiting, mostly), the first return address in Xenia's own code.
  Spot Walk(CONTEXT context) const {
    Spot spot = {context.Rip, 0};
    if (InExe(spot.pc) || InJit(spot.pc)) return spot;
    for (int frame = 0; frame < 32; ++frame) {
      DWORD64 image_base = 0;
      auto* entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
      if (!entry) {
        // a leaf function: the return address is on top of the stack
        context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
        context.Rsp += 8;
      } else {
        void* handler_data;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry,
                         &context, &handler_data, &establisher, nullptr);
      }
      if (!context.Rip) break;
      if (InExe(context.Rip)) {
        spot.caller = context.Rip;
        break;
      }
      if (InJit(context.Rip)) {
        spot.caller = 1;  // called from the game's code
        break;
      }
    }
    return spot;
  }
  void Sample() {
    auto* kernel = emulator_->kernel_state();
    auto* scheduler = kernel ? kernel->guest_scheduler() : nullptr;
    if (!scheduler || !emulator_->is_title_open()) return;
    if (!jit_size_ && emulator_->processor() &&
        emulator_->processor()->backend()) {
      auto* cache = emulator_->processor()->backend()->code_cache();
      jit_base_ = cache->execute_base_address();
      jit_size_ = cache->total_size();
    }
    for (int i = 0; i < kThreads; ++i) {
      xe::threading::Thread* thread = nullptr;
      if (i < kernel::GuestScheduler::cpu_count()) {
        thread = scheduler->cpu_host_thread(i);
      } else if (auto* graphics = emulator_->graphics_system()) {
        auto* processor = graphics->command_processor();
        auto* worker = processor ? processor->worker_thread() : nullptr;
        thread = worker ? worker->thread() : nullptr;
      }
      if (!thread || samples_[i].size() >= kMaxSamples) continue;
      HANDLE handle = HANDLE(thread->native_handle());
      if (SuspendThread(handle) == DWORD(-1)) continue;
      CONTEXT context = {};
      context.ContextFlags = CONTEXT_FULL;
      Spot spot = {0, 0};
      const bool got = GetThreadContext(handle, &context) != 0;
      if (got) spot = Walk(context);
      ResumeThread(handle);
      if (got) samples_[i].push_back(spot);
    }
  }
  std::string Xenia(uint64_t pc) const {
    // 16-byte steps: the line in Xenia's code, looked up in the .pdb on the PC
    return fmt::format("xenia +{:08X}", (pc - exe_base_) & ~uint64_t(15));
  }
  void Report() {
    auto* backend = emulator_->processor()
                        ? emulator_->processor()->backend()
                        : nullptr;
    auto* code_cache = backend ? backend->code_cache() : nullptr;
    for (int i = 0; i < kThreads; ++i) {
      auto& samples = samples_[i];
      if (samples.empty()) continue;
      std::map<std::string, uint32_t> counts;
      for (const Spot& spot : samples) {
        std::string key;
        if (InExe(spot.pc)) {
          key = Xenia(spot.pc);
        } else if (InJit(spot.pc)) {
          if (auto* function =
                  code_cache ? code_cache->LookupFunction(spot.pc) : nullptr) {
            key = fmt::format("game  {:08X} {}", function->address(),
                              function->name());
            hot_[function->address()]++;
          } else {
            key = "game  (helper code)";
          }
        } else if (spot.caller == 1) {
          key = "system, called by the game";
        } else if (spot.caller) {
          key = "system, called from " + Xenia(spot.caller);
        } else {
          key = "system (idle or unknown)";
        }
        ++counts[key];
      }
      std::vector<std::pair<uint32_t, std::string>> top;
      for (auto& [key, n] : counts) top.emplace_back(n, key);
      std::sort(top.rbegin(), top.rend());
      const std::string who =
          i < kernel::GuestScheduler::cpu_count() ? fmt::format("CPU {}", i)
                                                  : std::string("GPU thread");
      for (size_t j = 0; j < top.size() && j < 12; ++j) {
        GV_LIVE("Game Vault profile: {} {:5.1f}% {}", who,
               100.0 * top[j].first / samples.size(), top[j].second);
      }
      samples.clear();
    }
    // The game's hottest functions, once each: their code (big-endian PowerPC
    // words) for a look on the PC.
    for (auto& [address, n] : hot_) {
      if (n < 3000 || dumped_.count(address)) continue;
      dumped_.insert(address);
      const uint32_t* code =
          emulator_->memory()->TranslateVirtual<const uint32_t*>(address);
      std::string words;
      for (int w = 0; w < 160; ++w) {
        words += fmt::format("{:08X} ", xe::byte_swap(code[w]));
      }
      GV_LIVE("Game Vault profile code {:08X}: {}", address, words);
    }
    hot_.clear();
  }
  Emulator* emulator_;
  std::map<uint32_t, uint32_t> hot_;
  std::set<uint32_t> dumped_;
  std::vector<Spot> samples_[kThreads];
  uint64_t exe_base_ = 0, exe_size_ = 0, jit_base_ = 0, jit_size_ = 0;
};

// Game Vault's "Show frame rate" (fps=1 in the link that sent the game): the
// game's frames per second (frames it sent to the screen, as in xemu's
// counter) with the CPU and GPU load (LoadMeter), measured over half a
// second, small in the top left corner. Drawn
// on the foreground list: no window, so the controller stays with the game.
class FpsOverlay final : public ui::ImGuiDialog {
 public:
  FpsOverlay(ui::ImGuiDrawer* drawer, std::function<ui::Presenter*()> presenter,
             Emulator* emulator)
      : ui::ImGuiDialog(drawer),
        presenter_(std::move(presenter)),
        load_(emulator) {}
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
      double cpu, gpu, memory_mb;
      load_.Sample(&cpu, &gpu, &memory_mb);
    } else if (now - start_ >= std::chrono::milliseconds(500)) {
      const double seconds =
          std::chrono::duration<double>(now - start_).count();
      double cpu, gpu, memory_mb;
      load_.Sample(&cpu, &gpu, &memory_mb);
      snprintf(text_, sizeof(text_), "%.0f FPS  CPU %.0f%%  GPU %.0f%%",
               double(frames - frames_) / seconds, cpu, gpu);
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
  LoadMeter load_;
  char text_[48] = {};
};

// Game Vault: Xbox 360 title updates and DLC from the player's USB drive. Game Vault finds the packages (in "Game
// Vault\xbox 360 add-ons" on the drive), copies the new ones into the shared folder and lists them in "xbox 360
// add-ons.txt" there; each start installs the ones not installed yet, the way Xenia's own "Install content" does, and writes what happened
// to "xbox 360 add-ons done.txt" there ("<size>\t<installed|failed ...>\t<path>" per package) for Game Vault to show.
// A package that failed is tried again only when its size changes (a new copy), so a broken file costs one try.
// A title update just installed replaces the ones before it for the same disc (media ID) with the same or a lower
// version: a player who swaps in another update would otherwise keep every one side by side.
static void RemoveOlderTitleUpdates(Emulator* emulator, const Emulator::ContentInstallEntry& entry,
                                    std::ofstream& trace) {
  using xe::kernel::xam::ContentManager;
  const auto folder = emulator->content_root() / entry.data_installation_path_;
  std::error_code ec;
  // the game's patch: default.xexp, or one per disc of a multi-disc game
  auto patch_of = [&](const std::filesystem::path& dir, uint32_t& media, uint32_t& version) {
    if (ContentManager::ReadXexExecutionInfo(dir / "default.xexp", media, version)) return true;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec))
      if (e.path().extension() == ".xexp" && ContentManager::ReadXexExecutionInfo(e.path(), media, version))
        return true;
    return false;
  };
  uint32_t media = 0, version = 0;
  if (!patch_of(folder, media, version)) return;
  const auto type_folder = folder.parent_path();
  const auto headers = type_folder.parent_path() / "Headers" / type_folder.filename();
  std::vector<std::filesystem::path> older;
  for (const auto& e : std::filesystem::directory_iterator(type_folder, ec)) {
    uint32_t m = 0, v = 0;
    if (e.is_directory() && e.path().filename() != folder.filename() && patch_of(e.path(), m, v) && m == media &&
        v <= version)
      older.push_back(e.path());
  }
  for (const auto& p : older) {
    trace << "  older update " << xe::path_to_utf8(p.filename()) << " removed\n";
    XELOGI("Game Vault: older title update {} removed", xe::path_to_utf8(p.filename()));
    std::filesystem::remove_all(p, ec);
    std::filesystem::remove(headers / std::filesystem::path(p.filename()).concat(".header"), ec);
  }
}

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
  // Up to 1.1.8.122 the header was not read first, so every add-on was unpacked loose into the content folder (a
  // ".header" there gives it away) and still reported "installed". Those files go and those results are forgotten:
  // Game Vault copies the packages again and the next start installs them where games look.
  {
    const auto root = emulator->content_root();
    std::error_code ec;
    if (std::filesystem::exists(root / ".header", ec)) {
      for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        const std::string name = xe::path_to_utf8(e.path().filename());
        // a profile's content (or the shared 0000000000000000): where packages belong
        if (name.size() == 16 && name.find_first_not_of("0123456789ABCDEFabcdef") == std::string::npos) continue;
        std::filesystem::remove_all(e.path(), ec);
        trace << "loose " << name << " removed\n";
      }
      for (auto it = before.begin(); it != before.end();)
        it = it->second.second == "installed" ? before.erase(it) : std::next(it);
      XELOGI("Game Vault: add-ons: loose files from the old install removed; installed add-ons go in again");
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
      // where it goes (<profile>\<title ID>\<type>\<name>) comes from its header; without it the package was
      // unpacked loose into the content folder, where no game looks
      X_STATUS status = emulator->ProcessContentPackageHeader(path, entry);
      if (status == X_STATUS_SUCCESS) status = emulator->InstallContentPackage(path, entry);
      if (status == X_STATUS_SUCCESS) {
        result = "installed";
        installed++;
        trace << "  into " << xe::path_to_utf8(entry.data_installation_path_) << "\n";
        if (entry.content_type_ == XContentType::kInstaller) RemoveOlderTitleUpdates(emulator, entry, trace);
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

// View + Menu during a game: the game stops and a menu comes up over it. The
// controller then only moves through the menu; B goes back a page or resumes.
struct GameMenuActions {
  std::function<void()> fps_changed;    // the counter was turned on or off
  std::function<void()> restart;        // the game starts again
  std::function<void()> leave;          // back to Game Vault
  std::function<void()> clear_shaders;  // stored shaders go, then restart
  std::function<void(int)> set_scale;   // resolution scale, then restart
};

class GameMenu final : public ui::ImGuiDialog {
 public:
  GameMenu(ui::ImGuiDrawer* drawer, Emulator* emulator,
           GameMenuActions actions, GameMenu** open_menu)
      : ui::ImGuiDialog(drawer),
        emulator_(emulator),
        actions_(std::move(actions)),
        open_menu_(open_menu) {
    *open_menu_ = this;
    if (auto* input = emulator_->input_system()) {
      input->AddUIInputBlocker();
    }
    auto* scheduler = emulator_->kernel_state()
                          ? emulator_->kernel_state()->guest_scheduler()
                          : nullptr;
    if (scheduler && kernel::GuestScheduler::enabled()) {
      // Only the game's threads are held: with them stopped no new sound or
      // drawing comes, so both go quiet by themselves. (Xenia's own sound
      // pause waits for its worker to notice, which a worker running late,
      // as in GTA IV, never does: the menu froze the app.)
      scheduler_ = scheduler;
      scheduler_->SetPaused(true);
    } else {
      emulator_->Pause();
    }
    XELOGI("Game Vault: in-game menu open, game paused");
  }
  ~GameMenu() override {
    if (scheduler_) {
      scheduler_->SetPaused(false);
    } else {
      emulator_->Resume();
    }
    if (auto* input = emulator_->input_system()) {
      input->RemoveUIInputBlocker();
    }
    *open_menu_ = nullptr;
    XELOGI("Game Vault: in-game menu closed");
    if (after_close_) {
      after_close_();
    }
  }

 protected:
  enum class Page { kMain, kScale, kControllers };

  void OnDraw(ImGuiIO& io) override {
    ++frames_;
    const float scale = io.DisplaySize.y / 1080.0f;
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f * scale, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(28.0f * scale, 24.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(10.0f * scale, 10.0f * scale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 18, 32, 245));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(32, 44, 70, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 110, 220, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(60, 140, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(120, 180, 255, 255));
    ImGui::Begin("##gamevault_menu", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize);
    button_ = ImVec2(-FLT_MIN, 58.0f * scale);
    switch (page_) {
      case Page::kMain:
        DrawMain(scale);
        break;
      case Page::kScale:
        DrawScale(scale);
        break;
      case Page::kControllers:
        DrawControllers(scale);
        break;
    }
    ImGui::End();
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(4);
    // B: back a page, or resume (the View button that opened the menu is
    // still down for a moment, so the first frames don't count)
    if (!done_ && frames_ > 10 &&
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
      if (page_ == Page::kMain) {
        done_ = true;
      } else {
        Show(Page::kMain);
      }
    }
    if (done_) {
      Close();
    }
  }

 private:
  void Show(Page page) {
    page_ = page;
    page_frame_ = frames_ + 1;
  }
  void Title(const char* text, float scale) {
    ImGui::SetWindowFontScale(1.6f * scale);
    ImGui::TextUnformatted(text);
    ImGui::SetWindowFontScale(1.2f * scale);
    ImGui::Spacing();
    if (frames_ == page_frame_) {
      ImGui::SetKeyboardFocusHere();
    }
  }
  void Hint(const char* text, float scale) {
    ImGui::SetWindowFontScale(0.95f * scale);
    ImGui::TextDisabled("%s", text);
  }
  // Closes the menu (the game goes on) and then runs |action|.
  void CloseThen(std::function<void()> action) {
    after_close_ = std::move(action);
    done_ = true;
  }

  void DrawMain(float scale) {
    Title("Paused", scale);
    if (ImGui::Button("Resume", button_)) {
      done_ = true;
    }
    if (ImGui::Button("Restart game", button_)) {
      CloseThen(actions_.restart);
    }
    const int current = std::max(1, int(cvars::draw_resolution_scale_x));
    if (ImGui::Button(fmt::format("Resolution scale: {}x", current).c_str(),
                      button_)) {
      Show(Page::kScale);
    }
    if (ImGui::Button("Controllers", button_)) {
      Show(Page::kControllers);
    }
    const bool fps = UWP::GetShowFps();
    if (ImGui::Button(fps ? "Show frame rate: On" : "Show frame rate: Off",
                      button_)) {
      UWP::SetShowFps(!fps);
      if (actions_.fps_changed) actions_.fps_changed();
    }
    if (ImGui::Button("Clear shader cache", button_)) {
      CloseThen(actions_.clear_shaders);
    }
    if (ImGui::Button("Back to Game Vault", button_)) {
      CloseThen(actions_.leave);
    }
    Hint("B: resume.  Clearing the shader cache restarts the game.", scale);
  }

  void DrawScale(float scale) {
    Title("Resolution scale", scale);
    const int current = std::max(1, int(cvars::draw_resolution_scale_x));
    static const char* kNames[] = {"1x  (720p, fastest)", "2x  (1440p)",
                                   "3x  (4K, sharpest)"};
    for (int i = 1; i <= 3; ++i) {
      const std::string label =
          fmt::format("{}{}", kNames[i - 1], i == current ? "   (now)" : "");
      if (ImGui::Button(label.c_str(), button_) && i != current) {
        CloseThen([this, i]() { actions_.set_scale(i); });
      }
    }
    Hint("For every Xbox 360 game. The game restarts. B: back.", scale);
  }

  void DrawControllers(float scale) {
    Title("Controllers", scale);
    auto* input = emulator_->input_system();
    for (uint32_t slot = 0; slot < 4; ++slot) {
      hid::X_INPUT_CAPABILITIES caps = {};
      hid::X_INPUT_STATE state = {};
      const bool connected =
          input && input->GetCapabilities(slot, 1, &caps) == X_ERROR_SUCCESS;
      const bool pressed =
          connected &&
          input->GetStateForUI(slot, 1, &state) == X_ERROR_SUCCESS &&
          (state.gamepad.buttons || state.gamepad.left_trigger > 30 ||
           state.gamepad.right_trigger > 30);
      const std::string line =
          fmt::format("Player {}:  {}", slot + 1,
                      !connected ? "-"
                      : pressed  ? "connected  (pressing)"
                                 : "connected");
      if (pressed) {
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "%s",
                           line.c_str());
      } else if (connected) {
        ImGui::TextUnformatted(line.c_str());
      } else {
        ImGui::TextDisabled("%s", line.c_str());
      }
    }
    ImGui::Spacing();
    if (ImGui::Button("Back", button_)) {
      Show(Page::kMain);
    }
    Hint("Hold a button on a controller to see which player it is.", scale);
  }

  Emulator* emulator_;
  GameMenuActions actions_;
  GameMenu** open_menu_;
  kernel::GuestScheduler* scheduler_ = nullptr;
  Page page_ = Page::kMain;
  uint64_t frames_ = 0;
  uint64_t page_frame_ = 1;
  ImVec2 button_;
  bool done_ = false;
  std::function<void()> after_close_;
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
  // The running game starts again (in-game menu), on its own thread.
  void RestartTitle(const char* why);

  std::unique_ptr<Emulator> emulator_;
  std::unique_ptr<ui::Window> window_;
  std::unique_ptr<ui::ImGuiDrawer> imgui_drawer_;
  std::unique_ptr<ui::ImmediateDrawer> immediate_drawer_;
  ui::Presenter* presenter_painting_ = nullptr;
  FpsOverlay* fps_overlay_ = nullptr;  // owned by the ImGui drawer
  GameMenu* game_menu_ = nullptr;      // owned by the ImGui drawer, when open
  std::atomic<bool> emulator_ready_{false};
  std::atomic<bool> switching_{false};
  std::filesystem::path current_path_;  // the game started last
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
  // the 360 game settings Game Vault writes for a game the player changed (read at title load, config.cc)
  if (const std::string games = UWP::GetPublisherGames(); !games.empty()) {
    config::gamevault_settings_folder = std::filesystem::path(xe::to_path(games)) / "xenia-config";
  }

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
  current_path_ = path;
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
                                    [this]() { return presenter_painting_; },
                                    emulator_.get());
    }
    if (fps_overlay_) {
      fps_overlay_->set_shown(UWP::GetShowFps());
    }
  });
  XELOGI("Game Vault: starting {} (frame rate shown: {})", path,
         UWP::GetShowFps());
  // signed in, as on a real console (games otherwise stop at "You are not
  // signed in" and can't save)
  UWP::EnsureProfile(emulator_.get());
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
  // The hard drive's cache partitions and the other standard drives, as
  // Edge's xenia_main.cc does (GTA IV streams the city through cache:).
  emulator_->MountStandardDrives();

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
    // ImGui reads the controller for the in-game menu
    imgui_drawer_->LoadInputSystem(emulator_->input_system());
    // View + Menu held: the in-game menu (Game Vault when no game runs).
    UWP::SetHostGuideAction([this]() {
      if (game_menu_) {
        return;
      }
      if (!emulator_->is_title_open() || switching_) {
        UWP::ReturnToFrontend(imgui_drawer_.get());
        return;
      }
      GameMenuActions actions;
      actions.fps_changed = [this]() {
        if (!fps_overlay_ && UWP::GetShowFps()) {
          fps_overlay_ = new FpsOverlay(
              imgui_drawer_.get(), [this]() { return presenter_painting_; },
              emulator_.get());
        }
        if (fps_overlay_) {
          fps_overlay_->set_shown(UWP::GetShowFps());
        }
      };
      actions.restart = [this]() { RestartTitle("restart"); };
      actions.leave = [this]() { UWP::ReturnToFrontend(imgui_drawer_.get()); };
      actions.clear_shaders = [this]() {
        gpu::RequestShaderStorageClear(emulator_->game_config_title_id());
        RestartTitle("shader cache cleared");
      };
      actions.set_scale = [this](int scale) {
        cvars::draw_resolution_scale_x = scale;
        cvars::draw_resolution_scale_y = scale;
        config::SaveConfig();
        RestartTitle("resolution scale");
      };
      new GameMenu(imgui_drawer_.get(), emulator_.get(), std::move(actions),
                   &game_menu_);
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

  if (cvars::gv_profile_ms > 0) {
    std::thread([this]() {
      CpuSampler(emulator_.get()).Run(cvars::gv_profile_ms);
    }).detach();
  }

  // The game's frame rate, CPU and GPU load and memory in the log every 30 s
  // (test runs and player reports show the speed without a screenshot).
  std::thread([this]() {
    LoadMeter load(emulator_.get());
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
      double cpu, gpu, memory_mb;
      load.Sample(&cpu, &gpu, &memory_mb);
      if (last && frames >= last) {
        GV_LIVE("Game Vault: frame rate {:.1f}, CPU {:.0f}%, GPU {:.0f}%, "
               "memory {:.0f} MB, emulated CPUs {}%",
               double(frames - last) /
                   std::chrono::duration<double>(now - last_time).count(),
               cpu, gpu, memory_mb, load.PerCpu());
        uint64_t st[9];
        gpu::d3d12::GameVaultTakeSubmitStats(st);
        const double secs =
            std::chrono::duration<double>(now - last_time).count();
        const auto pct = [secs](uint64_t ns) {
          return 100.0 * double(ns) / (secs * 1e9);
        };
        GV_LIVE("Game Vault: GPU submits {:.0f}/s; time in replay {:.0f}%, "
                "close {:.0f}%, execute {:.0f}%, signal {:.0f}%; waits for the "
                "GPU {:.0f}/s, {:.0f}% of the time; occlusion query waits "
                "{:.0f}/s, {:.0f}%",
                st[0] / secs, pct(st[1]), pct(st[2]), pct(st[3]), pct(st[4]),
                st[5] / secs, pct(st[6]), st[7] / secs, pct(st[8]));
        std::vector<std::string> gpu_lines;
        gpu::d3d12::GameVaultTakeGpuTiming(secs, &gpu_lines);
        for (const std::string& gpu_line : gpu_lines) GV_LIVE("{}", gpu_line);
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
  current_path_ = path;
  switching_ = true;
  std::thread([this, path]() {
    emulator_->RelaunchTitle(xe::path_to_utf8(path), "", 0, {}, false);
    switching_ = false;
  }).detach();
}

void GameVaultXeniaApp::RestartTitle(const char* why) {
  if (switching_ || current_path_.empty()) {
    return;
  }
  XELOGI("Game Vault: in-game menu: the game starts again ({})", why);
  switching_ = true;
  const std::filesystem::path path = current_path_;
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
