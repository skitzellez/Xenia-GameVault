// Spartan Test trackers for Xenia (PSYCHOsmurf / Spartan Test).
// Released under the BSD license, like Xenia - see LICENSE in the root.
//
// The stall and freeze trackers, for Xbox 360 games:
//
// Short stalls: a frame held 80 ms or more. Once per stall, 80 ms in, every guest thread is tagged RUN (running on
// one of the 6 emulated CPUs), Q (ready, but its emulated CPU is busy with another thread), w (blocked, with what it
// waits on) or S (suspended), and the GPU thread's phase is taken. The tally goes to the log every 10 s with a second
// line: the host CPU time each emulated CPU and the GPU thread got from the last frame to the sample. An emulated CPU
// near the span computes; near 0 while a thread is RUN or Q on it = the Xbox gave it no core; GPU "idle" = the game
// handed it nothing.
//
// Freezes: no new frame for 1 s. A report at 1, 2, 3, 5, 10 and 20 s, then every 30 s up to 10 minutes: what each
// emulated CPU runs, every blocked and ready thread, the GPU thread's phase and ring buffer, and the CPU time since
// the report before (spinning or blocked). "frames resumed after" when it ends. The log is flushed (by the trackers'
// thread, within a second) after each report, so a freeze the player ends by closing the app is still in it.
//
// Each report also says what the shader builders did since the one before (pipelines the GPU thread built itself or
// waited for, the background builders' pipelines and CPU time), which game file reads and writes are still running,
// and a cause: the first that fits of shaders being built, the Xbox GPU, a file read or write running 0.5 s or more,
// the GPU thread's own work, a game thread busy on an emulated CPU, or the game waiting on itself. A freeze ends
// with the cause most of its reports gave, which goes into the session totals of the report card.
//
// Guest addresses are logged raw: looking up function names takes the global lock, which a frozen guest thread can
// hold, and the watchdog must never block (it drives preemption).

#include "xenia/kernel/guest_scheduler.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/spartan_trace.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace kernel {

namespace {

constexpr uint64_t kShortStallUs = 80'000;
constexpr uint64_t kTallyPeriodUs = 10'000'000;
// A watchdog tick this late means the whole app was suspended (Xbox guide, constrained mode), not a game stall.
constexpr uint64_t kHostPauseUs = 500'000;
// Emulated CPUs 0-5, then the GPU thread.
constexpr int kCpuSlots = 7;
constexpr int kGpuSlot = 6;

uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

uint64_t ThreadCpuUs(void* handle) {
#if XE_PLATFORM_WIN32
  FILETIME c, e, k, u;
  if (handle && GetThreadTimes(HANDLE(handle), &c, &e, &k, &u)) {
    const auto ticks = [](const FILETIME& t) {
      return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return (ticks(k) + ticks(u)) / 10;
  }
#endif
  return 0;
}

const char* ObjectKind(XObject* object) {
  if (!object) {
    return "object";
  }
  switch (object->type()) {
    case XObject::Type::Event:
      return "event";
    case XObject::Type::Semaphore:
      return "semaphore";
    case XObject::Type::Mutant:
      return "mutant";
    case XObject::Type::Thread:
      return "thread";
    case XObject::Type::Timer:
      return "timer";
    default:
      return "object";
  }
}

// One guest thread as seen at a sample, copied under the scheduler lock so nothing is dereferenced afterwards.
struct Seen {
  uint32_t tid = 0;
  std::string name;
  bool main = false;
  char tag = '?';  // R(UN), Q, w, S
  int cpu = -1;
  uint32_t pc = 0, lr = 0, ext = 0;
  std::string wait;  // for w: "event", "any of 3", "sleep", "host call" ...
  uint32_t handle = 0;
  int64_t due_ms = -1;
};

struct State {
  const void* owner = nullptr;
  uint32_t frame = 0;
  uint64_t frame_at = 0, last_tick = 0;
  bool sampled = false;
  // Freeze reports
  uint32_t reports = 0;
  uint64_t next_report_s = 1;
  uint64_t cpu_at_report[kCpuSlots] = {};
  uint64_t report_at = 0;
  // Short stalls
  std::map<std::string, uint32_t> tally;
  uint32_t stalls = 0;
  uint64_t tally_at = 0;
  uint64_t cpu_at_frame[kCpuSlots] = {};
  uint64_t cpu_sum[kCpuSlots] = {};
  uint64_t span_sum = 0;
  // Shader builders: their CPU time and the pipeline counters at the last frame and the last report.
  uint64_t builders_at_frame = 0, builders_at_report = 0, builders_sum = 0;
  struct Pipes {
    uint64_t now_built = 0, now_translate_us = 0, now_driver_us = 0, now_waited = 0, now_wait_us = 0;
    uint64_t bg_built = 0, bg_us = 0;
  } pipes_at_frame, pipes_at_report;
  // This freeze: how many reports gave each cause, and each emulated CPU's pc at the last report.
  uint32_t cause_reports[size_t(xe::spartan::Cause::kCount)] = {};
  uint32_t pc_at_report[kCpuSlots] = {};
};
// One per watchdog thread: on a relaunch the old scheduler's watchdog can still be stopping while the new one starts.
thread_local State g_state;

std::string ThreadLabel(const Seen& t) {
  return fmt::format("'{}'{}", t.name.empty() ? fmt::format("tid {:08X}", t.tid) : t.name, t.main ? " (main)" : "");
}

State::Pipes TakePipes() {
  const xe::spartan::PipelineCounters& c = xe::spartan::pipelines;
  State::Pipes p;
  p.now_built = c.now_built.load(std::memory_order_relaxed);
  p.now_translate_us = c.now_translate_us.load(std::memory_order_relaxed);
  p.now_driver_us = c.now_driver_us.load(std::memory_order_relaxed);
  p.now_waited = c.now_waited.load(std::memory_order_relaxed);
  p.now_wait_us = c.now_wait_us.load(std::memory_order_relaxed);
  p.bg_built = c.bg_built.load(std::memory_order_relaxed);
  p.bg_us = c.bg_us.load(std::memory_order_relaxed);
  return p;
}

// The builders' CPU time together (a restarted builder can make it go back: callers clamp at 0).
uint64_t BuildersCpuUs() {
  uint64_t sum = 0;
  for (int i = 0; i < xe::spartan::kBuilderSlots; ++i) {
    if (void* handle = xe::spartan::builder_thread_handle[i].load(std::memory_order_relaxed)) {
      sum += ThreadCpuUs(handle);
    }
  }
  return sum;
}

uint64_t Since(uint64_t now, uint64_t then) { return now >= then ? now - then : 0; }

std::string IoText(const xe::spartan::IoInFlight& io, uint64_t now) {
  std::string who = io.thread_id ? xe::spartan::ThreadName(io.thread_id) : std::string();
  if (who.empty()) {
    who = io.thread_id ? fmt::format("tid {:08X}", io.thread_id) : "an I/O worker (async)";
  } else {
    who = fmt::format("'{}'", who);
  }
  return fmt::format("{} '{}' for {} ms so far ({} KB at {:X}, {}), {}", io.write ? "writing" : "reading", io.file,
                     Since(now, io.start_us) / 1000, (io.bytes + 1023) / 1024, io.offset, io.medium, who);
}

uint64_t NextReportSecond(uint64_t after) {
  static constexpr uint64_t kFirst[] = {1, 2, 3, 5, 10, 20};
  for (uint64_t s : kFirst) {
    if (s > after) return s;
  }
  return (after / 30 + 1) * 30;
}

}  // namespace

void GuestScheduler::SpartanTick() {
  State& st = g_state;
  const uint64_t now = NowUs();
  if (st.owner != this) {
    st = State();
    st.owner = this;
    st.tally_at = now;
  }
  const bool host_paused = st.last_tick && now - st.last_tick > kHostPauseUs;
  st.last_tick = now;

  // Handle values only (published by the threads themselves): cpus_[i].host_thread is created after this watchdog
  // starts and freed before it stops, so it is never touched here.
  void* handles[kCpuSlots] = {};
  for (int i = 0; i < kGpuSlot; ++i) {
    handles[i] = xe::spartan::cpu_thread_handle[i].load(std::memory_order_relaxed);
  }
  handles[kGpuSlot] = xe::spartan::gpu_thread_handle.load(std::memory_order_relaxed);
  const auto cpu_times = [&handles](uint64_t out[kCpuSlots]) {
    for (int i = 0; i < kCpuSlots; ++i) out[i] = ThreadCpuUs(handles[i]);
  };

  // The 10 s tally of short stalls, the frame-holders first (main thread, running and queued threads, the GPU), then
  // by count. Every entry is written: a capped list would cut exactly the ones that matter.
  if (now - st.tally_at >= kTallyPeriodUs) {
    if (st.stalls) {
      std::vector<std::tuple<int, uint32_t, std::string>> rows;
      for (const auto& [key, count] : st.tally) {
        const bool first = key.find("(main)") != std::string::npos || key.find("(RUN") != std::string::npos ||
                           key.find("(Q") != std::string::npos || key.rfind("GPU ", 0) == 0 ||
                           key.rfind("emulated CPUs", 0) == 0;
        rows.emplace_back(first ? 0 : 1, count, key);
      }
      std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) < std::get<0>(b);
        if (std::get<1>(a) != std::get<1>(b)) return std::get<1>(a) > std::get<1>(b);
        return std::get<2>(a) < std::get<2>(b);
      });
      std::string text;
      for (size_t i = 0; i < rows.size(); ++i) {
        text += fmt::format("{} {} x{}", i ? ";" : "", std::get<2>(rows[i]), std::get<1>(rows[i]));
      }
      XELOGW("Spartan short stalls: {} in 10 s (sampled 80 ms in):{}", st.stalls, text);
      const double n = st.stalls;
      std::string per_cpu;
      uint64_t busy = 0;
      for (int i = 0; i < kGpuSlot; ++i) {
        per_cpu += fmt::format("{}{:.0f}", i ? "/" : "", st.cpu_sum[i] / n / 1000.0);
        busy += st.cpu_sum[i];
      }
      busy += st.cpu_sum[kGpuSlot] + st.builders_sum;
      XELOGW(
          "Spartan short stalls CPU: per stall {:.0f} ms since the last frame: emulated CPUs 0-5 {} ms, GPU thread "
          "{:.0f} ms, shader builders {:.0f} ms ({:.1f} host threads busy)",
          st.span_sum / n / 1000.0, per_cpu, st.cpu_sum[kGpuSlot] / n / 1000.0, st.builders_sum / n / 1000.0,
          st.span_sum ? double(busy) / double(st.span_sum) : 0.0);
    }
    st.tally.clear();
    st.stalls = 0;
    st.span_sum = 0;
    st.builders_sum = 0;
    std::fill(std::begin(st.cpu_sum), std::end(st.cpu_sum), 0);
    st.tally_at = now;
  }

  const uint32_t frame = xe::logging::GetFrameNumber();
  // Not a stall: the game has not shown a frame yet, the in-game menu holds it, the app was suspended, or it stops.
  if (!frame || paused_.load() || shutting_down_.load() || host_paused) {
    st.frame = frame;
    st.frame_at = now;
    st.sampled = true;
    st.reports = 0;
    st.next_report_s = 1;
    std::fill(std::begin(st.cause_reports), std::end(st.cause_reports), 0);
    return;
  }
  if (frame != st.frame) {
    if (st.reports) {
      // The cause most of its reports gave; on a tie the one listed first in spartan::Cause.
      size_t cause = 0;
      for (size_t i = 1; i < size_t(xe::spartan::Cause::kCount); ++i) {
        if (st.cause_reports[i] > st.cause_reports[cause]) cause = i;
      }
      const uint64_t ms = (now - st.frame_at) / 1000;
      XELOGW("Spartan stall: frames resumed after {:.2f} s (cause: {})", (now - st.frame_at) / 1e6,
             xe::spartan::CauseName(xe::spartan::Cause(cause)));
      xe::spartan::freeze_count[cause].fetch_add(1, std::memory_order_relaxed);
      xe::spartan::freeze_ms[cause].fetch_add(ms, std::memory_order_relaxed);
      // Written by this watchdog only (a relaunch can overlap two for a moment: then a report card may show one
      // freeze's time with another's cause, never anything worse).
      if (ms > xe::spartan::worst_freeze.v[0].load(std::memory_order_relaxed)) {
        xe::spartan::worst_freeze.Write(ms, cause, st.frame);
      }
    }
    st.frame = frame;
    st.frame_at = now;
    st.sampled = false;
    st.reports = 0;
    st.next_report_s = 1;
    std::fill(std::begin(st.cause_reports), std::end(st.cause_reports), 0);
    cpu_times(st.cpu_at_frame);
    st.builders_at_frame = BuildersCpuUs();
    st.pipes_at_frame = TakePipes();
    return;
  }
  const uint64_t idle = now - st.frame_at;
  const bool sample = !st.sampled && idle >= kShortStallUs;
  const bool report = idle >= st.next_report_s * 1'000'000 && st.next_report_s <= 600;
  if (!sample && !report) {
    return;
  }

  // Everything below reads the queues under lock_, copies, and formats after releasing it.
  std::vector<Seen> seen;
  bool cpu_busy[kMaxCpus] = {};
  {
    std::lock_guard<std::mutex> lock(lock_);
    const auto take = [&seen](XThread* t, char tag, int cpu) {
      Seen s;
      s.tid = t->thread_id();
      s.main = t->main_thread();
      s.tag = tag;
      s.cpu = cpu;
      if (auto* state = t->thread_state()) {
        if (auto* context = state->context()) {
          s.pc = context->last_safepoint_pc;
          s.lr = uint32_t(context->lr);
          s.ext = context->gv_extern_pc;
        }
      }
      if (tag == 'w') {
        auto& links = t->scheduler_links();
        switch (static_cast<XThread::CooperativeWaitKind>(links.wait_kind)) {
          case XThread::CooperativeWaitKind::kSingle:
            s.wait = ObjectKind(t->cooperative_wait_object());
            break;
          case XThread::CooperativeWaitKind::kMultiAny:
            s.wait = fmt::format("any of {}", links.wait_handle_count);
            break;
          case XThread::CooperativeWaitKind::kMultiAll:
            s.wait = fmt::format("all of {}", links.wait_handle_count);
            break;
          case XThread::CooperativeWaitKind::kDelay:
            s.wait = "sleep";
            break;
          case XThread::CooperativeWaitKind::kFence:
            s.wait = "host call";
            break;
          default:
            s.wait = "wait";
            break;
        }
        s.handle = links.wait_handle_count ? links.wait_handles[0] : 0;
        if (links.wait_deadline_ms) {
          s.due_ms = int64_t(links.wait_deadline_ms) - int64_t(Clock::QueryHostUptimeMillis());
        }
      }
      seen.push_back(std::move(s));
    };
    for (int i = 0; i < kMaxCpus; ++i) {
      Cpu& cpu = cpus_[i];
      if (cpu.current_thread) {
        cpu_busy[i] = true;
        take(cpu.current_thread, 'R', i);
      }
      for (int prio = 31; prio >= 0; --prio) {
        for (XThread* t = cpu.ready_head[prio]; t; t = t->scheduler_links().ready_next) take(t, 'Q', i);
      }
      for (XThread* t = cpu.blocked_head; t; t = t->scheduler_links().ready_next) take(t, 'w', i);
      for (XThread* t = cpu.suspended_head; t; t = t->scheduler_links().ready_next) take(t, 'S', i);
    }
  }
  // Names from the trackers' own list, after the scheduler lock is let go (see spartan_trace.h)
  for (Seen& t : seen) {
    t.name = xe::spartan::ThreadName(t.tid);
  }
  const auto phase = xe::spartan::GpuPhase(xe::spartan::gpu_phase.load(std::memory_order_relaxed));
  uint64_t cpu_now[kCpuSlots];
  cpu_times(cpu_now);
  const uint64_t builders_now = BuildersCpuUs();

  if (sample) {
    st.sampled = true;
    st.stalls++;
    st.span_sum += idle;
    for (int i = 0; i < kCpuSlots; ++i) {
      st.cpu_sum[i] += cpu_now[i] >= st.cpu_at_frame[i] ? cpu_now[i] - st.cpu_at_frame[i] : 0;
    }
    st.builders_sum += Since(builders_now, st.builders_at_frame);
    xe::spartan::short_stalls_by_phase[std::min(size_t(phase), size_t(xe::spartan::GpuPhase::kCount) - 1)]
        .fetch_add(1, std::memory_order_relaxed);
    uint32_t running = 0;
    for (bool b : cpu_busy) running += b;
    st.tally[fmt::format("emulated CPUs running {}", running)]++;
    st.tally[fmt::format("GPU {}", xe::spartan::GpuPhaseName(phase))]++;
    for (const Seen& t : seen) {
      std::string key;
      switch (t.tag) {
        case 'R':
          key = t.pc ? fmt::format("{}(RUN) pc {:08X}", ThreadLabel(t), t.pc)
                     : fmt::format("{}(RUN) lr {:08X}", ThreadLabel(t), t.lr);
          if (t.ext) key += fmt::format(" in import {:08X}", t.ext);
          break;
        case 'Q':
          key = fmt::format("{}(Q)", ThreadLabel(t));
          break;
        case 'w':
          key = fmt::format("{}(w {}) lr {:08X}", ThreadLabel(t), t.wait, t.lr);
          break;
        default:
          key = fmt::format("{}(S)", ThreadLabel(t));
          break;
      }
      st.tally[key]++;
    }
  }

  if (report) {
    st.reports++;
    const uint64_t since = st.reports == 1 ? idle : now - st.report_at;
    const uint64_t* base = st.reports == 1 ? st.cpu_at_frame : st.cpu_at_report;
    const auto cpu_ms = [&](int i) { return cpu_now[i] >= base[i] ? (cpu_now[i] - base[i]) / 1000 : 0; };
    const uint64_t window_ms = std::max<uint64_t>(since / 1000, 1);

    // Shader builders since the last report (or the last frame).
    const State::Pipes& pipes_base = st.reports == 1 ? st.pipes_at_frame : st.pipes_at_report;
    const State::Pipes pipes_now = TakePipes();
    const uint64_t now_built = Since(pipes_now.now_built, pipes_base.now_built);
    const uint64_t now_translate_ms = Since(pipes_now.now_translate_us, pipes_base.now_translate_us) / 1000;
    const uint64_t now_driver_ms = Since(pipes_now.now_driver_us, pipes_base.now_driver_us) / 1000;
    const uint64_t now_waited = Since(pipes_now.now_waited, pipes_base.now_waited);
    const uint64_t now_wait_ms = Since(pipes_now.now_wait_us, pipes_base.now_wait_us) / 1000;
    const uint64_t bg_built = Since(pipes_now.bg_built, pipes_base.bg_built);
    const uint64_t bg_ms = Since(pipes_now.bg_us, pipes_base.bg_us) / 1000;
    const uint64_t builders_ms =
        Since(builders_now, st.reports == 1 ? st.builders_at_frame : st.builders_at_report) / 1000;
    // Pipelines being built right now (the counters above only count finished ones).
    uint64_t gpu_on_ms = 0;
    std::string on_now;
    uint64_t current[3];
    const uint8_t gpu_kind = xe::spartan::gpu_pipeline_kind.load(std::memory_order_relaxed);
    if (gpu_kind && xe::spartan::gpu_pipeline_current.Read(current) && current[0]) {
      gpu_on_ms = Since(now, current[0]) / 1000;
      on_now = fmt::format("the GPU thread {} for {} ms so far (VS {:016X} PS {:016X})",
                           gpu_kind == 1 ? "builds one itself" : "waits for a builder", gpu_on_ms, current[1],
                           current[2]);
    }
    for (int i = 0; i < xe::spartan::kBuilderSlots; ++i) {
      if (xe::spartan::builder_current[i].Read(current) && current[0] &&
          Since(now, current[0]) >= xe::spartan::kSlowIoUs) {
        on_now += fmt::format("{}builder {} on VS {:016X} PS {:016X} for {} ms", on_now.empty() ? "" : "; ", i,
                              current[1], current[2], Since(now, current[0]) / 1000);
      }
    }
    const uint64_t shader_ms =
        std::min(window_ms, now_translate_ms + now_driver_ms + now_wait_ms + std::min(gpu_on_ms, window_ms));

    // Game file reads and writes still running, the oldest first.
    std::vector<xe::spartan::IoInFlight> io = xe::spartan::IoRunning();
    std::sort(io.begin(), io.end(), [](const auto& a, const auto& b) { return a.start_us < b.start_us; });
    const uint64_t oldest_io_ms = io.empty() ? 0 : Since(now, io.front().start_us) / 1000;

    // The cause: the first that fits (see the top of this file).
    using xe::spartan::Cause;
    using xe::spartan::GpuPhase;
    int busiest = 0;
    for (int i = 1; i < kGpuSlot; ++i) {
      if (cpu_ms(i) > cpu_ms(busiest)) busiest = i;
    }
    const Seen* busiest_thread = nullptr;
    for (const Seen& t : seen) {
      if (t.tag == 'R' && t.cpu == busiest) busiest_thread = &t;
    }
    Cause cause;
    std::string detail;
    if (phase == GpuPhase::kPipeline || shader_ms >= window_ms / 2) {
      cause = Cause::kShaders;
      detail = fmt::format("a draw waits for its pipeline ({} ms of the last {} ms went to building or waiting)",
                           shader_ms, window_ms);
    } else if (phase == GpuPhase::kAwaitGpu || phase == GpuPhase::kQueryWait) {
      cause = Cause::kXboxGpu;
      detail = xe::spartan::GpuPhaseName(phase);
    } else if (oldest_io_ms >= 500) {
      cause = io.front().write ? Cause::kFileWrite : Cause::kFileRead;
      detail = IoText(io.front(), now);
    } else if ((phase == GpuPhase::kCommands || phase == GpuPhase::kTexture || phase == GpuPhase::kUpload ||
                phase == GpuPhase::kSwap) &&
               cpu_ms(kGpuSlot) >= window_ms / 2) {
      cause = Cause::kGpuWork;
      detail = fmt::format("{}, {} of the last {} ms busy", xe::spartan::GpuPhaseName(phase), cpu_ms(kGpuSlot),
                           window_ms);
    } else if (cpu_ms(busiest) >= window_ms / 2) {
      cause = Cause::kGameBusy;
      if (busiest_thread) {
        detail = fmt::format("{} on CPU{} used {} of the last {} ms, at pc {:08X} ({})", ThreadLabel(*busiest_thread),
                             busiest, cpu_ms(busiest), window_ms, busiest_thread->pc,
                             st.reports == 1                                   ? "first report"
                             : busiest_thread->pc == st.pc_at_report[busiest] ? "the same place as the last report"
                                                                               : "moved since the last report");
      } else {
        detail = fmt::format("CPU{} used {} of the last {} ms", busiest, cpu_ms(busiest), window_ms);
      }
    } else {
      cause = Cause::kGameWaiting;
      detail = fmt::format("no emulated CPU half busy, no file read or write running 0.5 s, GPU thread {}",
                           xe::spartan::GpuPhaseName(phase));
    }
    st.cause_reports[size_t(cause)]++;

    XELOGW("Spartan stall: no new frame for {:.0f} s (frame {})", idle / 1e6, frame);
    XELOGW("Spartan stall   cause: {}: {}", xe::spartan::CauseName(cause), detail);
    std::string gpu = fmt::format("Spartan stall   GPU thread: {}; ring read {:X} write {:X}",
                                  xe::spartan::GpuPhaseName(phase),
                                  xe::spartan::gpu_read_ptr.load(std::memory_order_relaxed),
                                  xe::spartan::gpu_write_ptr.load(std::memory_order_relaxed));
    if (phase == xe::spartan::GpuPhase::kWaitRegMem) {
      gpu += fmt::format(" ({} {:08X}: wants {:08X} under mask {:08X}, has {:08X})",
                         xe::spartan::wait_is_memory.load() ? "memory" : "register",
                         xe::spartan::wait_addr.load(), xe::spartan::wait_ref.load(), xe::spartan::wait_mask.load(),
                         xe::spartan::wait_value.load());
    }
    gpu += fmt::format("; host CPU {} ms of the last {} ms", cpu_ms(kGpuSlot), since / 1000);
    XELOGW("{}", gpu);
    for (int i = 0; i < kMaxCpus && i < kGpuSlot; ++i) {
      const Seen* running = nullptr;
      for (const Seen& t : seen) {
        if (t.tag == 'R' && t.cpu == i) running = &t;
      }
      if (running) {
        XELOGW("Spartan stall   CPU{}: runs {} tid {:08X} pc {:08X} lr {:08X}{}; host CPU {} ms", i,
               ThreadLabel(*running), running->tid, running->pc, running->lr,
               running->ext ? fmt::format(" in import {:08X}", running->ext) : std::string(), cpu_ms(i));
      } else {
        XELOGW("Spartan stall   CPU{}: idle; host CPU {} ms", i, cpu_ms(i));
      }
    }
    if (now_built || now_waited || bg_built || builders_ms >= 50 || !on_now.empty()) {
      XELOGW(
          "Spartan stall   shaders: finished since the last report: the GPU thread built {} pipelines itself "
          "(translation {} ms, Xbox driver {} ms) and waited {} ms for {} a builder had; builders built {} ({} ms "
          "between them, {} ms of host CPU){}{}",
          now_built, now_translate_ms, now_driver_ms, now_wait_ms, now_waited, bg_built, bg_ms, builders_ms,
          on_now.empty() ? "" : "; now: ", on_now);
    }
    size_t files = 0;
    for (const auto& running_io : io) {
      if (Since(now, running_io.start_us) < xe::spartan::kSlowIoUs) break;
      if (++files > 8) {
        XELOGW("Spartan stall   ... more file reads and writes running");
        break;
      }
      XELOGW("Spartan stall   file: {}", IoText(running_io, now));
    }
    // Waiting threads and the ready queues; the main thread first. 48 lines at most.
    std::vector<const Seen*> rest;
    for (const Seen& t : seen) {
      if (t.tag != 'R') rest.push_back(&t);
    }
    std::stable_sort(rest.begin(), rest.end(), [](const Seen* a, const Seen* b) { return a->main > b->main; });
    size_t shown = 0;
    for (const Seen* t : rest) {
      if (++shown > 48) {
        XELOGW("Spartan stall   ... {} more threads", rest.size() - 48);
        break;
      }
      if (t->tag == 'w') {
        XELOGW("Spartan stall     waits: {} tid {:08X} on {} handle {:08X} lr {:08X}{}{}", ThreadLabel(*t), t->tid,
               t->wait, t->handle, t->lr, t->ext ? fmt::format(" in import {:08X}", t->ext) : std::string(),
               t->due_ms >= 0 ? fmt::format(", times out in {} ms", t->due_ms) : std::string());
      } else {
        XELOGW("Spartan stall     {}: {} tid {:08X} on CPU{} pc {:08X} lr {:08X}",
               t->tag == 'Q' ? "ready, CPU busy" : "suspended", ThreadLabel(*t), t->tid, t->cpu, t->pc, t->lr);
      }
    }
    std::copy(std::begin(cpu_now), std::end(cpu_now), std::begin(st.cpu_at_report));
    st.builders_at_report = builders_now;
    st.pipes_at_report = pipes_now;
    for (int i = 0; i < kCpuSlots; ++i) st.pc_at_report[i] = 0;
    for (const Seen& t : seen) {
      if (t.tag == 'R' && t.cpu >= 0 && t.cpu < kCpuSlots) st.pc_at_report[t.cpu] = t.pc;
    }
    st.report_at = now;
    st.next_report_s = NextReportSecond(idle / 1'000'000);
    xe::spartan::flush_log_requested.store(true, std::memory_order_relaxed);
  }
}

}  // namespace kernel
}  // namespace xe
