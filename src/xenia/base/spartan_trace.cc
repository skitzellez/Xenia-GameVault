// Spartan Test trackers for Xenia (PSYCHOsmurf / Spartan Test).
// Released under the BSD license, like Xenia - see LICENSE in the root.
#include "xenia/base/spartan_trace.h"

#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace spartan {

void* CurrentThreadQueryHandle() {
#if XE_PLATFORM_WIN32
  // Xenia's thread objects hold GetCurrentThread()'s pseudo handle, which means "the calling thread" wherever it is
  // used, so another thread reading it gets its own CPU time. This is a real handle to this thread, only for reading
  // its times. Never closed (one per thread start): the sampler can never read a closed or reused handle.
  HANDLE real = nullptr;
  if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real,
                      THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
    return real;
  }
#endif
  return nullptr;
}

namespace {

constexpr uint32_t kIoSlots = 64;

struct IoState {
  std::mutex lock;
  IoInFlight slots[kIoSlots];
  bool used[kIoSlots] = {};
  IoStats window, session;
};
// Leaked on purpose (like the logger's globals): game threads still read files while the app exits.
IoState& Io() {
  static IoState* state = new IoState();
  return *state;
}

void Add(IoStats& stats, bool write, std::string_view file, const char* medium, uint64_t bytes, uint64_t io_us,
         uint64_t drive_wait_us) {
  const bool slow = io_us >= kSlowIoUs;
  if (write) {
    stats.writes++;
    stats.write_bytes += bytes;
    stats.write_us += io_us;
    stats.slow_writes += slow;
    if (io_us > stats.slowest_write_us) {
      stats.slowest_write_us = io_us;
      stats.slowest_write_file.assign(file);
    }
  } else {
    stats.reads++;
    stats.read_bytes += bytes;
    stats.read_us += io_us;
    stats.slow_reads += slow;
    if (io_us > stats.slowest_read_us) {
      stats.slowest_read_us = io_us;
      stats.slowest_read_bytes = bytes;
      stats.slowest_read_medium = medium;
      stats.slowest_read_file.assign(file);
    }
  }
  stats.drive_wait_us += drive_wait_us;
}

}  // namespace

uint32_t IoBegin(uint32_t thread_id, bool write, std::string_view file, const char* medium, uint64_t offset,
                 uint64_t bytes, uint64_t start_us) {
  IoState& io = Io();
  std::lock_guard<std::mutex> lock(io.lock);
  for (uint32_t i = 0; i < kIoSlots; ++i) {
    if (!io.used[i]) {
      io.used[i] = true;
      IoInFlight& slot = io.slots[i];
      slot.thread_id = thread_id;
      slot.write = write;
      slot.start_us = start_us;
      slot.offset = offset;
      slot.bytes = bytes;
      slot.medium = medium;
      slot.file.assign(file);
      return i + 1;
    }
  }
  return 0;
}

void IoEnd(uint32_t ticket, bool write, std::string_view file, const char* medium, uint64_t done_bytes,
           uint64_t start_us, uint64_t io_done_us, uint64_t end_us) {
  const uint64_t io_us = io_done_us > start_us ? io_done_us - start_us : 0;
  const uint64_t drive_wait_us = end_us > io_done_us ? end_us - io_done_us : 0;
  IoState& io = Io();
  std::lock_guard<std::mutex> lock(io.lock);
  if (ticket && ticket <= kIoSlots) {
    io.used[ticket - 1] = false;
  }
  Add(io.window, write, file, medium, done_bytes, io_us, drive_wait_us);
  Add(io.session, write, file, medium, done_bytes, io_us, drive_wait_us);
}

std::vector<IoInFlight> IoRunning() {
  IoState& io = Io();
  std::vector<IoInFlight> running;
  std::lock_guard<std::mutex> lock(io.lock);
  for (uint32_t i = 0; i < kIoSlots; ++i) {
    if (io.used[i]) running.push_back(io.slots[i]);
  }
  return running;
}

IoStats IoTakeWindow() {
  IoState& io = Io();
  std::lock_guard<std::mutex> lock(io.lock);
  IoStats window = std::move(io.window);
  io.window = IoStats();
  return window;
}

IoStats IoSession() {
  IoState& io = Io();
  std::lock_guard<std::mutex> lock(io.lock);
  return io.session;
}

}  // namespace spartan
}  // namespace xe
