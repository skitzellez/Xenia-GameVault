// Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
// PolyForm Noncommercial License 1.0.0: not for sale, and this notice stays in every copy.
// Game Vault: counters for the dip log (gv_dips.log): what the emulator was doing in the seconds a game's frame rate
// fell. Cheap enough to stay in player builds.
#ifndef XENIA_BASE_GV_PERF_H_
#define XENIA_BASE_GV_PERF_H_

#include <atomic>
#include <chrono>
#include <cstdint>

#include <intrin.h>

namespace xe {
namespace gv_perf {

struct Timed {
  std::atomic<uint64_t> count{0}, us{0}, max_us{0};
};
inline Timed net_read, texture_load, memory_upload, pipeline_now;
// Game Vault 1.1.8.480: disc image reads from a drive on the console (USB, internal), not the network
inline Timed drive_read;
// network read-ahead: pieces fetched ahead, of those read by the game, waits on one in flight, pieces read on demand
// frame times (presenter): longest in the second, frames over 25 ms and over 50 ms
inline std::atomic<uint64_t> frame_max_us{0}, frames_over_25{0}, frames_over_50{0};
inline std::atomic<uint64_t> net_ahead{0}, net_ahead_used{0}, net_ahead_waits{0}, net_demand{0};
// 1.1.8.485: draws_total never reset (creation threads read it to see a quiet game); pipelines built in the
// background this second and how many are still waiting in the queue.
inline std::atomic<uint64_t> draws_total{0}, pipelines_built{0}, pipelines_left{0};
// 1.1.8.488: the pipelines on the title's used list (gvhot) and how many are built: the "getting graphics ready"
// counter on screen (emulator_window.cc)
inline std::atomic<uint64_t> hot_total{0}, hot_built{0};
inline std::atomic<uint64_t> upload_bytes{0}, draws{0}, draws_skipped{0}, pipelines_queued{0}, pipelines_not_waited{0};
// Texture cache: made and destroyed since the last look, how many it holds and their host memory (MB) now.
// Shared memory: writes by the game caught on watched pages, and requests that had to take the lock.
inline std::atomic<uint64_t> mem_faults{0}, mem_slow_requests{0};
inline std::atomic<uint64_t> tex_created{0}, tex_destroyed{0}, tex_count{0}, tex_mb{0};

// Time in each stage of a draw on the GPU thread, in processor clock ticks.
inline std::atomic<uint64_t> draw_stage[8];
inline void DrawStage(int stage, uint64_t& last) {
  const uint64_t now = __rdtsc();
  draw_stage[stage].fetch_add(now - last, std::memory_order_relaxed);
  last = now;
}

class Scope {
 public:
  explicit Scope(Timed& timed) : timed_(timed), start_(std::chrono::steady_clock::now()) {}
  ~Scope() {
    const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - start_)
                                     .count());
    timed_.count.fetch_add(1, std::memory_order_relaxed);
    timed_.us.fetch_add(us, std::memory_order_relaxed);
    uint64_t seen = timed_.max_us.load(std::memory_order_relaxed);
    while (us > seen && !timed_.max_us.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {
    }
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Timed& timed_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace gv_perf
}  // namespace xe

#endif  // XENIA_BASE_GV_PERF_H_
