// Spartan Test trackers for Xenia (PSYCHOsmurf / Spartan Test).
// Released under the BSD license, like Xenia - see LICENSE in the root.
//
// The app-side trackers (the stall and freeze trackers are in kernel/guest_scheduler_spartan.cc):
// - Memory: the app's memory against the limit the Xbox kills it at (5 GB as a Game, 1 GB as an App), and the Xbox
//   GPU memory against its budget. A line every 60 s while a game runs; a warning the first time 80, 90 and 95% of
//   the limit (or the GPU budget) is passed, so a game closed by the Xbox for memory leaves a trace.
// - Audio: every 10 s with gaps, how often the sound ran dry (XAudio2 found nothing queued) or was one frame from it.
// - Crashes: an exception nothing handled is written with where it happened (xeniacanary.exe+offset, for the /MAP
//   file) and the guest thread that ran, to LocalState\spartan_crash.txt and the log (flushed), before Windows ends
//   the app. The filter that was there before is still called after.
// - Game files: every 10 s with a read or write of 100 ms or more, the window's reads and writes and the slowest.
// - Report card: every 5 minutes while a game runs, when the app goes to the background or is suspended (the Xbox
//   button: a quit from there ends the app without Xenia's own shutdown) and when Xenia closes, the session in a few
//   lines: freezes by cause, short stalls, shaders built while playing (and each builder's slowest), game files,
//   sound and memory peaks. Totals since Xenia started.

#include "spartan_tracker.h"

// First, so windows.h comes in with Xenia's settings (NOMINMAX, lean) before dxgi brings it in
#include "xenia/base/platform_win.h"

#include <algorithm>
#include <bit>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <tuple>
#include <utility>
#include <vector>
#include <string>
#include <thread>

#include <dxgi1_4.h>
#include <fileapifromapp.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>

#include "third_party/fmt/include/fmt/format.h"
#include "UWPUtil.h"
#include "version.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/spartan_trace.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

DECLARE_bool(log_safepoint_pc);
DECLARE_bool(use_dedicated_xma_thread);
DECLARE_string(xma_decoder);

namespace spartan {
namespace {

// The crash path runs on a thread that has just faulted, maybe inside the heap or a lock: it formats into stack
// buffers, writes LocalState\spartan_crash.txt with plain file calls first, and only then tries Xenia's log.
wchar_t g_crash_path[1024] = {};
// Declared here: the app SDK's headers leave the top-level filter type out with the call.
using TopLevelFilter = LONG(WINAPI*)(EXCEPTION_POINTERS*);
TopLevelFilter g_previous_filter = nullptr;

template <typename... Args>
size_t Append(char* buffer, size_t size, size_t used, fmt::format_string<Args...> format, Args&&... args) {
  if (used >= size) return used;
  const auto result = fmt::format_to_n(buffer + used, size - used, format, std::forward<Args>(args)...);
  return std::min(size, used + size_t(result.size));
}

size_t WhereInApp(char* buffer, size_t size, size_t used, uintptr_t address) {
  const auto base = reinterpret_cast<uintptr_t>(&__ImageBase);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
  if (address >= base && address < base + nt->OptionalHeader.SizeOfImage) {
    return Append(buffer, size, used, "xeniacanary.exe+{:X}", address - base);
  }
  return Append(buffer, size, used, "{:X} (outside xeniacanary.exe: JIT code or a system library)", address);
}

size_t GuestThreadText(char* buffer, size_t size, size_t used) {
  // IsInThread(nullptr) is true when this host thread has no guest thread (GetCurrentThread would assert there)
  auto* thread = xe::kernel::XThread::IsInThread(nullptr) ? nullptr : xe::kernel::XThread::GetCurrentThread();
  if (!thread) {
    return Append(buffer, size, used, "no guest thread");
  }
  used = Append(buffer, size, used, "guest thread tid {:08X}{}", thread->thread_id(),
                thread->main_thread() ? " (main)" : "");
  if (auto* state = thread->thread_state()) {
    if (auto* context = state->context()) {
      used = Append(buffer, size, used, " pc {:08X} lr {:08X}", context->last_safepoint_pc, uint32_t(context->lr));
      if (context->gv_extern_pc) used = Append(buffer, size, used, " in import {:08X}", context->gv_extern_pc);
    }
  }
  return used;
}

void Report(const char* text, size_t length) {
  if (g_crash_path[0]) {
    HANDLE file = CreateFileFromAppW(g_crash_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(file, text, DWORD(length), &written, nullptr);
      FlushFileBuffers(file);
      CloseHandle(file);
    }
  }
  // The log writer thread would wait on itself.
  if (!xe::IsLogWriterThread()) {
    XELOGE("{}", std::string_view(text, length));
    xe::FlushLog();
  }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
  static std::atomic<bool> once{false};
  if (info && info->ExceptionRecord && !once.exchange(true)) {
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    char text[768];
    size_t used = Append(text, sizeof(text), 0, "Spartan crash: exception {:08X} at ", uint32_t(record.ExceptionCode));
    used = WhereInApp(text, sizeof(text), used, reinterpret_cast<uintptr_t>(record.ExceptionAddress));
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
      const ULONG_PTR kind = record.ExceptionInformation[0];
      used = Append(text, sizeof(text), used, ", {} {:X}",
                    kind == 0 ? "reading" : kind == 1 ? "writing" : "executing", record.ExceptionInformation[1]);
    }
    used = Append(text, sizeof(text), used, "; host thread {}; ", GetCurrentThreadId());
    used = GuestThreadText(text, sizeof(text), used);
    Report(text, used);
  }
  return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

std::terminate_handler g_previous_terminate = nullptr;

[[noreturn]] void OnTerminate() {
  static std::atomic<bool> once{false};
  if (!once.exchange(true)) {
    char text[512];
    size_t used = Append(text, sizeof(text), 0,
                         "Spartan crash: an error nothing handled ended the app (std::terminate); host thread {}; ",
                         GetCurrentThreadId());
    used = GuestThreadText(text, sizeof(text), used);
    Report(text, used);
  }
  if (g_previous_terminate) g_previous_terminate();
  std::abort();
}

void InstallCrashHandlers() {
  const std::string local = UWP::GetLocalState();
  if (!local.empty()) {
    const std::wstring path = xe::to_path(local).wstring() + L"\\spartan_crash.txt";
    if (path.size() < std::size(g_crash_path)) wcscpy_s(g_crash_path, path.c_str());
  }
  // The app SDK leaves SetUnhandledExceptionFilter out (as it does the vectored handler calls Xenia binds from
  // ntdll); kernelbase exports it.
  using SetFilterFn = TopLevelFilter(WINAPI*)(TopLevelFilter);
  SetFilterFn set_filter = nullptr;
  for (const wchar_t* module : {L"kernelbase.dll", L"kernel32.dll"}) {
    if (HMODULE handle = GetModuleHandleW(module)) {
      set_filter = reinterpret_cast<SetFilterFn>(GetProcAddress(handle, "SetUnhandledExceptionFilter"));
      if (set_filter) break;
    }
  }
  if (set_filter) {
    g_previous_filter = set_filter(CrashFilter);
  } else {
    XELOGW("Spartan trackers: no crash handler on this system (SetUnhandledExceptionFilter not found)");
  }
  g_previous_terminate = std::set_terminate(OnTerminate);
}

// What only this file sees, for the report card (FinalReport runs on the window's thread).
std::atomic<bool> g_started{false};
std::atomic<uint64_t> g_first_frame_us{0}, g_first_frame{0};
std::atomic<uint64_t> g_audio_dry{0}, g_audio_one_left{0};
std::atomic<uint64_t> g_app_peak{0}, g_app_limit{0}, g_gpu_peak{0}, g_gpu_budget{0};
// When the last report card for leaving the app was written: background and suspend come together, one card does.
std::atomic<uint64_t> g_leave_report_us{0};
// Sound making, the session so far (the 10 s line adds each window).
struct SoundSession {
  std::atomic<uint64_t> callbacks{0}, callbacks_long{0}, callback_max_us{0}, pumps_late{0}, pump_late_max_us{0};
  std::atomic<uint64_t> decodes{0}, decode_us{0}, decode_in_callback_us{0}, decode_max{0}, refused{0};
};
SoundSession g_sound;

const char* XmaWhere() {
  return cvars::use_dedicated_xma_thread ? "on its own thread (use_dedicated_xma_thread on)"
                                         : "on the thread that asks (use_dedicated_xma_thread off)";
}

// Every 10 s while the game makes sound: how its sound code and the XMA decoding kept up.
void SoundLine() {
  namespace sp = xe::spartan;
  sp::SoundCounters& c = sp::sound;
  const uint64_t callbacks = c.callbacks.exchange(0), callback_us = c.callback_us.exchange(0),
                 callback_max = c.callback_max_us.exchange(0), longs = c.callbacks_long.exchange(0),
                 late = c.pumps_late.exchange(0), late_max = c.pump_late_max_us.exchange(0),
                 not_sent = c.frames_not_sent.exchange(0), decodes = c.decodes.exchange(0),
                 decode_us = c.decode_us.exchange(0), in_callback = c.decode_in_callback_us.exchange(0),
                 decode_max = c.decode_max.exchange(0), refused = c.refused.exchange(0);
  int streams = 0;
  for (auto& bits : c.streams_seen) {
    streams += int(std::popcount(bits.exchange(0)));
  }
  const int64_t open_now = std::max<int64_t>(0, c.streams_open.load());
  const uint64_t open_max = std::max<uint64_t>(c.streams_open_max.exchange(uint64_t(open_now)), uint64_t(open_now));
  g_sound.callbacks += callbacks;
  g_sound.callbacks_long += longs;
  g_sound.pumps_late += late;
  g_sound.decodes += decodes;
  g_sound.decode_us += decode_us;
  g_sound.decode_in_callback_us += in_callback;
  g_sound.refused += refused;
  sp::AtomicMax(g_sound.callback_max_us, callback_max);
  sp::AtomicMax(g_sound.pump_late_max_us, late_max);
  sp::AtomicMax(g_sound.decode_max, decode_max);
  if (!callbacks && !decodes && !refused) {
    return;
  }
  XELOGI(
      "Spartan sound: in 10 s the game's sound code ran {} times, {:.2f} ms on average, the slowest {:.1f} ms, {} "
      "longer than one sound frame (5.3 ms); started late {} times (the worst {:.1f} ms after its time); {} frames "
      "not made (output full); XMA decoding {}: {} decodes of {} streams, {} ms in all, {} ms of it inside the game's "
      "sound code, the slowest {:.1f} ms (stream {}); streams open {} (the most {} of {}), {} refused (none free)",
      callbacks, callbacks ? callback_us / 1000.0 / callbacks : 0.0, callback_max / 1000.0, longs, late,
      late_max / 1000.0, not_sent, XmaWhere(), decodes, streams, decode_us / 1000, in_callback / 1000,
      (decode_max >> 16) / 1000.0, decode_max & 0xFFFF, open_now, open_max, sp::kXmaStreams, refused);
}

void WriteReport(const char* when) {
  namespace sp = xe::spartan;
  const uint64_t frame = xe::logging::GetFrameNumber();
  const uint64_t first_us = g_first_frame_us.load(), first_frame = g_first_frame.load();
  if (!frame || !first_us) {
    return;
  }
  const double seconds = std::max(1.0, double(sp::NowUs() - first_us) / 1e6);
  XELOGI("Spartan report ({}): {:.0f} min since the first frame, frame {}, {:.1f} frames a second on average",
         when, seconds / 60, frame, double(frame > first_frame ? frame - first_frame : 0) / seconds);

  uint64_t freezes = 0, freeze_ms = 0;
  std::string by_cause;
  for (size_t i = 0; i < size_t(sp::Cause::kCount); ++i) {
    const uint64_t n = sp::freeze_count[i].load(std::memory_order_relaxed);
    const uint64_t ms = sp::freeze_ms[i].load(std::memory_order_relaxed);
    if (!n) continue;
    freezes += n;
    freeze_ms += ms;
    by_cause += fmt::format("{}{} {} ({:.1f} s)", by_cause.empty() ? "" : ", ", sp::CauseName(sp::Cause(i)), n,
                            ms / 1000.0);
  }
  std::string worst;
  uint64_t w[3];
  if (freezes && sp::worst_freeze.Read(w) && w[1] < uint64_t(sp::Cause::kCount)) {
    worst = fmt::format(", the longest {:.1f} s at frame {} ({})", w[0] / 1000.0, w[2],
                        sp::CauseName(sp::Cause(w[1])));
  }
  XELOGI("Spartan report: freezes (no new frame for 1 s or more): {}, {:.1f} s in all{}", freezes, freeze_ms / 1000.0,
         worst);
  if (freezes) {
    XELOGI("Spartan report:   by cause: {}", by_cause);
  }

  uint64_t stalls = 0;
  std::vector<std::pair<uint64_t, size_t>> phases;
  for (size_t i = 0; i < size_t(sp::GpuPhase::kCount); ++i) {
    const uint64_t n = sp::short_stalls_by_phase[i].load(std::memory_order_relaxed);
    if (!n) continue;
    stalls += n;
    phases.emplace_back(n, i);
  }
  std::sort(phases.begin(), phases.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::string by_phase;
  for (const auto& [n, i] : phases) {
    by_phase += fmt::format("{}{} {}", by_phase.empty() ? "" : ", ", sp::GpuPhaseName(sp::GpuPhase(i)), n);
  }
  XELOGI("Spartan report: short stalls (a frame held 80 ms or more): {}{}{}", stalls,
         stalls ? "; the GPU thread was: " : "", by_phase);

  const sp::PipelineCounters& c = sp::pipelines;
  const uint64_t built = c.now_built.load(), translate_us = c.now_translate_us.load(),
                 driver_us = c.now_driver_us.load();
  std::string slowest;
  uint64_t s3[3];
  if (built && c.now_slowest.Read(s3)) {
    slowest = fmt::format("; the slowest {} ms (VS {:016X} PS {:016X})", s3[0] / 1000, s3[1], s3[2]);
  }
  XELOGI(
      "Spartan report: shaders built while playing: the GPU thread built {} itself (translation {:.1f} s, Xbox "
      "driver {:.1f} s, {} ms each on average{}) and waited {} times for a builder ({:.1f} s); the builders built {} "
      "({:.1f} s between them); {} failed",
      built, translate_us / 1e6, driver_us / 1e6, built ? (translate_us + driver_us) / built / 1000 : 0, slowest,
      c.now_waited.load(), c.now_wait_us.load() / 1e6, c.bg_built.load(), c.bg_us.load() / 1e6, c.failed.load());
  // Each builder's slowest pipeline, the slowest first (8 at most).
  std::vector<std::tuple<uint64_t, int, uint64_t, uint64_t>> slow;
  for (int i = 0; i < sp::kBuilderSlots; ++i) {
    uint64_t b[3];
    if (sp::builder_slowest[i].Read(b) && b[0]) slow.emplace_back(b[0], i, b[1], b[2]);
  }
  std::sort(slow.begin(), slow.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
  if (!slow.empty()) {
    std::string text;
    for (size_t i = 0; i < slow.size() && i < 8; ++i) {
      text += fmt::format("{}builder {} {} ms (VS {:016X} PS {:016X})", i ? "; " : "", std::get<1>(slow[i]),
                          std::get<0>(slow[i]) / 1000, std::get<2>(slow[i]), std::get<3>(slow[i]));
    }
    XELOGI("Spartan report: each builder's slowest pipeline: {}", text);
  }

  const sp::IoStats io = sp::IoSession();
  std::string slowest_read, slowest_write;
  if (io.reads) {
    slowest_read = fmt::format(", the slowest {} ms: '{}' ({} KB, {})", io.slowest_read_us / 1000,
                               io.slowest_read_file, (io.slowest_read_bytes + 1023) / 1024, io.slowest_read_medium);
  }
  if (io.writes) {
    slowest_write = fmt::format(", the slowest {} ms: '{}'", io.slowest_write_us / 1000, io.slowest_write_file);
  }
  XELOGI(
      "Spartan report: game files: {} reads ({:.0f} MB, {:.1f} s), {} took 100 ms or more{}; {} writes ({:.1f} MB), "
      "{} took 100 ms or more{}; emulated drive speed waits {:.1f} s",
      io.reads, io.read_bytes / 1048576.0, io.read_us / 1e6, io.slow_reads, slowest_read, io.writes,
      io.write_bytes / 1048576.0, io.slow_writes, slowest_write, io.drive_wait_us / 1e6);

  const uint64_t decode_max = g_sound.decode_max.load();
  XELOGI(
      "Spartan report: sound making: the game's sound code ran {} times, {} longer than one sound frame, the slowest "
      "{:.1f} ms, started late {} times (the worst {:.1f} ms); XMA decoding ({}, decoder '{}'): {} decodes, {:.1f} s in "
      "all, {:.1f} s inside the game's sound code, the slowest {:.1f} ms (stream {}); {} streams refused (none free)",
      g_sound.callbacks.load(), g_sound.callbacks_long.load(), g_sound.callback_max_us.load() / 1000.0,
      g_sound.pumps_late.load(), g_sound.pump_late_max_us.load() / 1000.0, XmaWhere(), cvars::xma_decoder,
      g_sound.decodes.load(), g_sound.decode_us.load() / 1e6, g_sound.decode_in_callback_us.load() / 1e6,
      (decode_max >> 16) / 1000.0, decode_max & 0xFFFF, g_sound.refused.load());
  XELOGI(
      "Spartan report: sound ran dry {} times (a gap), down to one frame {} times; memory peaks: app {} of {} MB, "
      "Xbox GPU {} of a {} MB budget",
      g_audio_dry.load(), g_audio_one_left.load(), g_app_peak.load() >> 20, g_app_limit.load() >> 20,
      g_gpu_peak.load() >> 20, g_gpu_budget.load() >> 20);
}

// Warns once per level passed; a level is armed again after use falls 10 points below it.
struct Levels {
  bool passed[3] = {};
  template <typename F>
  void Check(double percent, F&& warn) {
    static constexpr int kLevels[3] = {80, 90, 95};
    for (int i = 0; i < 3; ++i) {
      if (!passed[i] && percent >= kLevels[i]) {
        passed[i] = true;
        warn(kLevels[i]);
      } else if (passed[i] && percent < kLevels[i] - 10) {
        passed[i] = false;
      }
    }
  }
};

// The Xbox GPU's adapter, for its memory budget. Taken once on the thread that set the emulator up (a relaunch
// replaces the graphics system, so the watch thread never reads it); the adapter outlives a new device.
IDXGIAdapter3* FindAdapter(xe::Emulator* emulator) {
  IDXGIAdapter3* adapter = nullptr;
  auto* graphics = emulator->graphics_system();
  auto* provider = graphics ? static_cast<xe::ui::d3d12::D3D12Provider*>(graphics->provider()) : nullptr;
  if (provider && provider->GetDevice() && provider->GetDXGIFactory()) {
    IDXGIFactory4* factory = nullptr;
    if (SUCCEEDED(provider->GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&factory)))) {
      if (FAILED(factory->EnumAdapterByLuid(provider->GetDevice()->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
        adapter = nullptr;
      }
      factory->Release();
    }
  }
  return adapter;
}

void Watch(xe::Emulator* emulator, IDXGIAdapter3* adapter) {
  using namespace std::chrono;
  namespace sp = xe::spartan;
  Levels app_levels, gpu_levels;
  uint64_t app_peak = 0, gpu_peak = 0;
  auto last_audio = steady_clock::now(), last_memory = last_audio, last_report = last_audio;
  for (;;) {
    std::this_thread::sleep_for(seconds(1));
    const auto now = steady_clock::now();
    if (sp::flush_log_requested.exchange(false)) {
      xe::FlushLog();
    }
    if (!g_first_frame_us.load()) {
      if (const uint64_t frame = xe::logging::GetFrameNumber()) {
        g_first_frame.store(frame);
        g_first_frame_us.store(sp::NowUs());
      }
    }

    // Audio: every 10 s, only when the sound ran dry or nearly.
    if (now - last_audio >= seconds(10)) {
      last_audio = now;
      const uint64_t frames = sp::audio_frames.exchange(0), dry = sp::audio_dry.exchange(0),
                     one_left = sp::audio_one_left.exchange(0);
      const uint32_t fewest = sp::audio_min_queued.exchange(UINT32_MAX);
      g_audio_dry.fetch_add(dry);
      g_audio_one_left.fetch_add(one_left);
      SoundLine();
      if (dry || one_left) {
        XELOGW(
            "Spartan audio: in 10 s {} frames to XAudio2; the sound ran dry {} times (a gap), down to one frame {} "
            "times; fewest queued {} of {}",
            frames, dry, one_left, fewest == UINT32_MAX ? 0 : fewest, sp::audio_queue_size.load());
      }
      // Game files: only a window with a read or write of 100 ms or more.
      const sp::IoStats io = sp::IoTakeWindow();
      if (io.slow_reads || io.slow_writes) {
        std::string text = fmt::format("Spartan files: in 10 s {} reads ({:.1f} MB, {} ms)", io.reads,
                                       io.read_bytes / 1048576.0, io.read_us / 1000);
        if (io.slow_reads) {
          text += fmt::format(", {} took 100 ms or more, the slowest {} ms: '{}' ({} KB, {})", io.slow_reads,
                              io.slowest_read_us / 1000, io.slowest_read_file, (io.slowest_read_bytes + 1023) / 1024,
                              io.slowest_read_medium);
        }
        text += fmt::format("; {} writes", io.writes);
        if (io.slow_writes) {
          text += fmt::format(", {} took 100 ms or more, the slowest {} ms: '{}'", io.slow_writes,
                              io.slowest_write_us / 1000, io.slowest_write_file);
        }
        if (io.drive_wait_us) {
          text += fmt::format("; emulated drive speed waits {} ms", io.drive_wait_us / 1000);
        }
        XELOGW("{}", text);
      }
    }

    // Memory: the app against its limit, the Xbox GPU memory against its budget.
    uint64_t app = 0, limit = 0;
    try {
      app = winrt::Windows::System::MemoryManager::AppMemoryUsage();
      limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
    } catch (...) {
    }
    uint64_t gpu = 0, budget = 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO video = {};
    if (adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &video))) {
      gpu = video.CurrentUsage;
      budget = video.Budget;
    }
    app_peak = std::max(app_peak, app);
    gpu_peak = std::max(gpu_peak, gpu);
    g_app_peak.store(app_peak);
    g_gpu_peak.store(gpu_peak);
    if (limit) g_app_limit.store(limit);
    if (budget) g_gpu_budget.store(budget);
    if (limit) {
      app_levels.Check(100.0 * double(app) / double(limit), [&](int level) {
        XELOGW("Spartan memory: the app passed {}% of its memory limit: {} MB of {} MB (the Xbox closes it at the "
               "limit)",
               level, app >> 20, limit >> 20);
        if (level >= 95) xe::FlushLog();
      });
    }
    if (budget) {
      gpu_levels.Check(100.0 * double(gpu) / double(budget), [&](int level) {
        XELOGW("Spartan memory: Xbox GPU memory passed {}% of its budget: {} MB of {} MB", level, gpu >> 20,
               budget >> 20);
      });
    }
    if (now - last_memory >= seconds(60) && emulator->is_title_open()) {
      last_memory = now;
      XELOGI("Spartan memory: app {} MB of {} MB (peak {} MB), Xbox GPU memory {} MB of a {} MB budget (peak {} MB)",
             app >> 20, limit >> 20, app_peak >> 20, gpu >> 20, budget >> 20, gpu_peak >> 20);
    }
    if (now - last_report >= minutes(5) && emulator->is_title_open()) {
      last_report = now;
      WriteReport("every 5 minutes");
    }
  }
}

// On the window's thread, when the app is sent to the background or suspended. The log is flushed after: the Xbox
// may end the app next without warning.
void LeaveReport(const char* when) {
  const uint64_t now = xe::spartan::NowUs();
  const uint64_t last = g_leave_report_us.load();
  if (last && now - last < 30'000'000) {
    return;
  }
  g_leave_report_us.store(now);
  WriteReport(when);
  xe::FlushLog();
}

}  // namespace

void StartTrackers(xe::Emulator* emulator) {
  if (!emulator || g_started.exchange(true)) {
    return;
  }
  XELOGI(
      "Spartan trackers: on (short stalls from 80 ms, freezes from 1 s with their cause, shader builds, game file "
      "reads and writes, saves, sound making and XMA decoding, audio gaps, memory levels, crashes, a report card every 5 "
      "minutes) - the Spartan Test "
      "trackers by PSYCHOsmurf; source {} {}; thread pc = {}",
      XE_BUILD_BRANCH, XE_BUILD_COMMIT_SHORT,
      cvars::log_safepoint_pc ? "the last loop it passed (log_safepoint_pc on)"
                              : "where it was last preempted (log_safepoint_pc off)");
  InstallCrashHandlers();
  IDXGIAdapter3* adapter = FindAdapter(emulator);
  if (!adapter) {
    XELOGW("Spartan trackers: the GPU memory budget can't be read (no DXGI adapter), app memory only");
  }
  std::thread(Watch, emulator, adapter).detach();
}

void WatchAppLeaving() {
  static std::atomic<bool> watching{false};
  if (watching.exchange(true)) {
    return;
  }
  using winrt::Windows::ApplicationModel::Core::CoreApplication;
  try {
    CoreApplication::EnteredBackground([](auto&&, auto&&) { LeaveReport("the app went to the background"); });
    CoreApplication::Suspending([](auto&&, auto&&) { LeaveReport("the app is being suspended"); });
  } catch (const winrt::hresult_error& error) {
    XELOGW("Spartan trackers: no report card on leaving the app (0x{:08X})", uint32_t(error.code().value));
  } catch (...) {
    XELOGW("Spartan trackers: no report card on leaving the app");
  }
}

void FinalReport() {
  if (g_started.load()) {
    WriteReport("Xenia closing");
  }
}

}  // namespace spartan
