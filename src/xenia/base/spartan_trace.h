// Spartan Test trackers for Xenia (PSYCHOsmurf / Spartan Test).
// Released under the BSD license, like Xenia - see LICENSE in the root.
//
// What the GPU thread is doing right now, the shader builders, the game's file reads and writes, and the sound
// output's health, read by the short-stall sampler and the freeze report (kernel/guest_scheduler_spartan.cc) and the
// app-side lines and report card (xenia-canary-uwp/spartan_tracker.cc).
// Plain relaxed atomics: a stale value only makes one sample a little off. Cheap enough for player builds.
#ifndef XENIA_BASE_SPARTAN_TRACE_H_
#define XENIA_BASE_SPARTAN_TRACE_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace xe {
namespace spartan {

// The GPU thread's phase. A scope sets it and puts the one before back, so the innermost wait shows.
enum class GpuPhase : uint8_t {
  kNotStarted = 0,
  kIdle,          // nothing new from the game in the ring buffer
  kCommands,      // reading the game's commands (draws, register writes)
  kWaitRegMem,    // WAIT_REG_MEM: waits for a value the game (CPU) has to write
  kAwaitGpu,      // waits for the Xbox GPU to finish earlier work
  kQueryWait,     // waits for an occlusion query result
  kPipeline,      // a draw waits for its pipeline to be built now
  kTexture,       // loading a texture from game memory
  kUpload,        // uploading game memory to the GPU
  kSwap,          // handing a frame to the presenter
  kCount,
};

inline const char* GpuPhaseName(GpuPhase phase) {
  switch (phase) {
    case GpuPhase::kNotStarted:
      return "not started";
    case GpuPhase::kIdle:
      return "idle (nothing new from the game)";
    case GpuPhase::kCommands:
      return "running the game's commands";
    case GpuPhase::kWaitRegMem:
      return "waiting for the game (WAIT_REG_MEM)";
    case GpuPhase::kAwaitGpu:
      return "waiting for the Xbox GPU to finish";
    case GpuPhase::kQueryWait:
      return "waiting for an occlusion query";
    case GpuPhase::kPipeline:
      return "building a pipeline now";
    case GpuPhase::kTexture:
      return "loading a texture";
    case GpuPhase::kUpload:
      return "uploading game memory";
    case GpuPhase::kSwap:
      return "presenting a frame";
    default:
      return "?";
  }
}

inline std::atomic<uint8_t> gpu_phase{uint8_t(GpuPhase::kNotStarted)};
// Set on the GPU (command processor) thread, so the phase scopes in shared code only act there.
inline thread_local bool tls_is_gpu_thread = false;
// The GPU thread's handle (HANDLE on Windows, from CurrentThreadQueryHandle), for its CPU time in the stall lines.
inline std::atomic<void*> gpu_thread_handle{nullptr};
// Ring buffer positions after the last executed block, and the write pointer the game last set.
inline std::atomic<uint32_t> gpu_read_ptr{0}, gpu_write_ptr{0};
// The WAIT_REG_MEM being waited on: register or memory address, the value wanted (ref under mask) and the last seen.
inline std::atomic<uint32_t> wait_addr{0}, wait_ref{0}, wait_mask{0}, wait_value{0};
inline std::atomic<bool> wait_is_memory{false};

class GpuPhaseScope {
 public:
  explicit GpuPhaseScope(GpuPhase phase) : active_(tls_is_gpu_thread) {
    if (active_) {
      previous_ = gpu_phase.exchange(uint8_t(phase), std::memory_order_relaxed);
    }
  }
  ~GpuPhaseScope() {
    if (active_) {
      gpu_phase.store(previous_, std::memory_order_relaxed);
    }
  }
  GpuPhaseScope(const GpuPhaseScope&) = delete;
  GpuPhaseScope& operator=(const GpuPhaseScope&) = delete;

 private:
  bool active_;
  uint8_t previous_ = 0;
};

// A handle another thread can read this thread's CPU time with (Windows; nullptr elsewhere). Kept for the app's life.
void* CurrentThreadQueryHandle();

// The native handle of each emulated CPU's dispatch thread (guest scheduler), published by the thread itself while its
// loop runs. The sampler only keeps the handle value, never the thread object, which shutdown frees.
inline std::atomic<void*> cpu_thread_handle[6] = {};

// Guest thread names by thread id, written where Xenia names a thread (XThread::set_name, under its lock) and read by
// the sampler after it has let go of the scheduler lock: the game can rename a thread at any moment, so the sampler
// never reads an XThread's own name string. A leaf lock: nothing else is taken while it is held.
// Leaked on purpose (like the logger's globals): threads still name and sample while the app exits.
struct ThreadNames {
  std::mutex lock;
  std::unordered_map<uint32_t, std::string> names;
};
inline ThreadNames& thread_names() {
  static ThreadNames* names = new ThreadNames();
  return *names;
}
inline void SetThreadName(uint32_t thread_id, std::string_view name) {
  ThreadNames& t = thread_names();
  std::lock_guard<std::mutex> lock(t.lock);
  if (t.names.size() >= 4096 && !t.names.count(thread_id)) {
    t.names.clear();  // a game making thousands of short threads: start the list again
  }
  t.names[thread_id] = std::string(name);
}
inline std::string ThreadName(uint32_t thread_id) {
  ThreadNames& t = thread_names();
  std::lock_guard<std::mutex> lock(t.lock);
  auto it = t.names.find(thread_id);
  return it != t.names.end() ? it->second : std::string();
}

// Set by the freeze report; the trackers' thread (spartan_tracker.cc) flushes the log, so the watchdog never waits.
inline std::atomic<bool> flush_log_requested{false};

// Sound output (XAudio2 driver), taken every 10 s by the audio line: frames handed to XAudio2, of those how many found
// its queue empty (the sound had already run dry: an audible gap) or down to one frame, and the fewest frames queued.
inline std::atomic<uint64_t> audio_frames{0}, audio_dry{0}, audio_one_left{0};
inline std::atomic<uint32_t> audio_min_queued{UINT32_MAX}, audio_queue_size{0};

// Microseconds of the steady clock, the one time base of all the trackers.
inline uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// Three values that belong together, written by one thread and read by others without a lock (a sequence lock): a
// reader that meets a write in progress tries again, and after 8 tries gives up (Read returns false).
struct Snapshot3 {
  std::atomic<uint32_t> seq{0};
  std::atomic<uint64_t> v[3] = {};
  void Write(uint64_t a, uint64_t b, uint64_t c) {
    const uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    v[0].store(a, std::memory_order_relaxed);
    v[1].store(b, std::memory_order_relaxed);
    v[2].store(c, std::memory_order_relaxed);
    seq.store(s + 2, std::memory_order_release);
  }
  bool Read(uint64_t out[3]) const {
    for (int tries = 0; tries < 8; ++tries) {
      const uint32_t s = seq.load(std::memory_order_acquire);
      if (s & 1) continue;
      out[0] = v[0].load(std::memory_order_relaxed);
      out[1] = v[1].load(std::memory_order_relaxed);
      out[2] = v[2].load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (seq.load(std::memory_order_relaxed) == s) return true;
    }
    return false;
  }
};

// Shader builds (gpu/d3d12/pipeline_cache.cc). "Now": a draw on the GPU thread found its pipeline missing and the
// GPU thread built it itself (translation = the Xbox 360 shaders made into PC shaders, driver = the Xbox's graphics
// driver making the pipeline from them), or waited while a builder thread finished it. Builders: the background
// "D3D12 Pipelines" threads (stored pipelines, look-ahead, queued ones). Running totals; readers take differences.
struct PipelineCounters {
  std::atomic<uint64_t> now_built{0}, now_translate_us{0}, now_driver_us{0};
  std::atomic<uint64_t> now_waited{0}, now_wait_us{0};
  std::atomic<uint64_t> bg_built{0}, bg_us{0}, failed{0};
  // The slowest the GPU thread built itself this session: total us, vertex and pixel shader hashes (GPU thread only).
  std::atomic<uint64_t> now_slowest_us{0};
  Snapshot3 now_slowest;
};
inline PipelineCounters pipelines;
// The builder threads' handles (CurrentThreadQueryHandle), by builder index, for their CPU time. A restarted builder
// takes its slot over; the old handle stays open (see CurrentThreadQueryHandle), so a reader never sees a closed one.
constexpr int kBuilderSlots = 16;
inline std::atomic<void*> builder_thread_handle[kBuilderSlots] = {};
// The pipeline each builder is building right now: when it started (0 = none), vertex and pixel shader hashes.
// Written only by that builder.
inline Snapshot3 builder_current[kBuilderSlots];
// The slowest pipeline each builder built this session: us, vertex and pixel shader hashes (that builder only).
inline Snapshot3 builder_slowest[kBuilderSlots];
// What the GPU thread does about a pipeline a draw needs: 0 nothing, 1 builds it itself, 2 waits for a builder that
// has it; and that pipeline: when it started, vertex and pixel shader hashes. GPU thread only.
inline std::atomic<uint8_t> gpu_pipeline_kind{0};
inline Snapshot3 gpu_pipeline_current;
// A pipeline that takes this long to build, or that the GPU thread waits this long for, gets a line of its own.
constexpr uint64_t kSlowPipelineUs = 2'000'000;
constexpr uint64_t kLongPipelineWaitUs = 1'000'000;

// The game's file reads and writes (kernel/xfile.cc), timed from the request to the data being there. Every request
// is on a list while it runs, so a freeze report can say which thread waits for which file and for how long.
struct IoInFlight {
  uint32_t thread_id = 0;  // guest thread, 0 off a guest thread (an async completion)
  bool write = false;
  uint64_t start_us = 0, offset = 0, bytes = 0;
  const char* medium = "";
  std::string file;
};
struct IoStats {
  uint64_t reads = 0, read_bytes = 0, read_us = 0, slow_reads = 0;  // slow: 100 ms or more
  uint64_t writes = 0, write_bytes = 0, write_us = 0, slow_writes = 0;
  uint64_t drive_wait_us = 0;  // the emulated drive speed (when on), after the data was read
  uint64_t slowest_read_us = 0, slowest_read_bytes = 0, slowest_write_us = 0;
  const char* slowest_read_medium = "";
  std::string slowest_read_file, slowest_write_file;
};
constexpr uint64_t kSlowIoUs = 100'000;
// Begin puts a request on the list and returns its ticket for End (0 when the list is full: it is still counted).
// Both take a leaf lock for a few copies only, never while the I/O runs. io_done_us: the data was there; end_us: the
// emulated drive time (when on) was waited out too.
uint32_t IoBegin(uint32_t thread_id, bool write, std::string_view file, const char* medium, uint64_t offset,
                 uint64_t bytes, uint64_t start_us);
void IoEnd(uint32_t ticket, bool write, std::string_view file, const char* medium, uint64_t done_bytes,
           uint64_t start_us, uint64_t io_done_us, uint64_t end_us);
std::vector<IoInFlight> IoRunning();
// Totals since the last call (window) and since the start (session).
IoStats IoTakeWindow();
IoStats IoSession();

// Why a freeze happened, decided at each report of it (guest_scheduler_spartan.cc).
enum class Cause : uint8_t {
  kShaders = 0,  // the GPU thread builds or waits for a pipeline a draw needs
  kXboxGpu,      // waiting for the Xbox GPU (earlier work, an occlusion query)
  kGpuWork,      // the GPU thread busy with its own work (textures, uploads, commands)
  kFileRead,     // a game thread waits for a file read (the disc image, local or network)
  kFileWrite,    // a game thread waits for a file write (saving)
  kGameBusy,     // a game thread busy on an emulated CPU (computing, or spinning while it waits)
  kGameWaiting,  // the game's threads all wait (timers, events, sleep) and nothing above is going on
  kCount,
};
inline const char* CauseName(Cause cause) {
  switch (cause) {
    case Cause::kShaders:
      return "shaders being built";
    case Cause::kXboxGpu:
      return "waiting for the Xbox GPU";
    case Cause::kGpuWork:
      return "the GPU thread's own work";
    case Cause::kFileRead:
      return "a file read";
    case Cause::kFileWrite:
      return "a file write (saving)";
    case Cause::kGameBusy:
      return "a game thread busy on an emulated CPU";
    case Cause::kGameWaiting:
      return "the game waiting on itself";
    default:
      return "?";
  }
}

// Session totals for the report card, written by the freeze and short-stall trackers.
inline std::atomic<uint64_t> freeze_count[size_t(Cause::kCount)] = {};
inline std::atomic<uint64_t> freeze_ms[size_t(Cause::kCount)] = {};
// The longest freeze: ms, cause, frame.
inline Snapshot3 worst_freeze;
inline std::atomic<uint64_t> short_stalls_by_phase[size_t(GpuPhase::kCount)] = {};

// Raises a to v if v is larger.
inline void AtomicMax(std::atomic<uint64_t>& a, uint64_t v) {
  uint64_t seen = a.load(std::memory_order_relaxed);
  while (v > seen && !a.compare_exchange_weak(seen, v, std::memory_order_relaxed)) {
  }
}

// Sound making (apu/audio_system.cc, apu/xma_decoder.cc), taken every 10 s by the sound line. The game's sound code
// is the callback the audio worker runs once per sound frame (5.3 ms) to make the next frame; late: it started more
// than one sound frame after its due time. XMA is the Xbox 360's compressed sound (voices, music, effects); each
// stream (an XMA context) is decoded when the game kicks it, on the thread that kicks it, or on the XMA thread with
// use_dedicated_xma_thread. Running totals; readers take differences.
constexpr uint64_t kSoundFrameUs = 5333;
constexpr uint32_t kXmaStreams = 320;  // XmaDecoder::kContextCount
struct SoundCounters {
  std::atomic<uint64_t> callbacks{0}, callback_us{0}, callback_max_us{0}, callbacks_long{0};
  std::atomic<uint64_t> pumps_late{0}, pump_late_max_us{0}, frames_not_sent{0};
  std::atomic<uint64_t> decodes{0}, decode_us{0}, decode_in_callback_us{0};
  std::atomic<uint64_t> decode_max{0};  // us << 16 | stream, so the larger time wins
  std::atomic<uint64_t> streams_seen[(kXmaStreams + 63) / 64] = {};  // a bit per stream decoded
  std::atomic<int64_t> streams_open{0};
  std::atomic<uint64_t> streams_open_max{0}, opened{0}, refused{0};
};
inline SoundCounters sound;
// True on a thread while it runs the game's sound code, so decoding it does there is told apart.
inline thread_local bool in_sound_callback = false;
inline void SoundDecoded(uint32_t stream, uint64_t us) {
  sound.decodes.fetch_add(1, std::memory_order_relaxed);
  sound.decode_us.fetch_add(us, std::memory_order_relaxed);
  if (in_sound_callback) {
    sound.decode_in_callback_us.fetch_add(us, std::memory_order_relaxed);
  }
  AtomicMax(sound.decode_max, (us << 16) | (stream & 0xFFFF));
  if (stream < kXmaStreams) {
    sound.streams_seen[stream / 64].fetch_or(uint64_t(1) << (stream % 64), std::memory_order_relaxed);
  }
}
inline void SoundStreamOpened(bool ok) {
  if (!ok) {
    sound.refused.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  sound.opened.fetch_add(1, std::memory_order_relaxed);
  const int64_t open = sound.streams_open.fetch_add(1, std::memory_order_relaxed) + 1;
  if (open > 0) {
    AtomicMax(sound.streams_open_max, uint64_t(open));
  }
}
inline void SoundStreamClosed() { sound.streams_open.fetch_sub(1, std::memory_order_relaxed); }

}  // namespace spartan
}  // namespace xe

#endif  // XENIA_BASE_SPARTAN_TRACE_H_
