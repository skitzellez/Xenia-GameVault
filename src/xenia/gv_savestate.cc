/* Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
 * BSD licence, like Xenia (see LICENSE in the root).
 *
 * Save states for Xbox 360 games (test feature).
 *
 * How a state is taken: the guest scheduler is held, so every game thread is parked. A thread can be written down
 * when all it is doing is known from the guest side: it was parked by a JIT safepoint (it goes on at that guest
 * address), or it sits in a kernel wait that can simply be asked again (it goes on at the import's thunk, with the
 * registers the call was made with). A thread inside anything else (a callback from host code, a kernel call that
 * is half done) can't, so the game is let run a few milliseconds and tried again.
 *
 * What is kept: all game memory, the heap page tables, thread registers, graphics registers and ring pointers.
 * Events, semaphores and mutants mirror their state in game memory, so they are re-read from it after a load.
 * What is NOT kept (so far): timers, open files' positions, sockets, sound decoder state, render target contents,
 * threads or kernel objects made or closed between the save and the load.
 */

#include "xenia/gv_savestate.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <atomic>
#include <cstdlib>
#include <map>
#include <thread>
#include <vector>

#include "third_party/zstd/lib/zstd.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/register_file.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xmutant.h"
#include "xenia/kernel/xsession.h"
#include "xenia/kernel/xtimer.h"
#include "xenia/apu/audio_system.h"
#include "xenia/apu/xma_decoder.h"
#include <array>
#include <set>
#include "xenia/ui/presenter.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe {

namespace {

constexpr uint32_t kMagic = 0x53565647;  // "GVVS"
constexpr uint32_t kVersion = 7;
constexpr size_t kChunk = 4 * 1024 * 1024;
// The guest registers come first in PPCContext; host pointers follow.
constexpr size_t kGuestRegsSize = offsetof(cpu::ppc::PPCContext, thread_id);

// Blocks of up to 4 MB, each zstd-compressed: [raw size][packed size][data].
class Writer {
 public:
  explicit Writer(FILE* file) : file_(file) { raw_.reserve(kChunk); }
  void Write(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    total_ += length;
    while (length) {
      const size_t take = std::min(length, kChunk - raw_.size());
      raw_.insert(raw_.end(), bytes, bytes + take);
      bytes += take;
      length -= take;
      if (raw_.size() == kChunk) {
        Flush();
      }
    }
  }
  template <typename T>
  void Put(const T& value) {
    Write(&value, sizeof(T));
  }
  void Flush() {
    if (raw_.empty()) {
      return;
    }
    packed_.resize(ZSTD_compressBound(raw_.size()));
    const size_t packed =
        ZSTD_compress(packed_.data(), packed_.size(), raw_.data(), raw_.size(), 1);
    if (ZSTD_isError(packed)) {
      ok_ = false;
      raw_.clear();
      return;
    }
    const uint32_t head[2] = {uint32_t(raw_.size()), uint32_t(packed)};
    if (std::fwrite(head, sizeof(head), 1, file_) != 1 ||
        std::fwrite(packed_.data(), 1, packed, file_) != packed) {
      ok_ = false;
    }
    file_bytes_ += sizeof(head) + packed;
    raw_.clear();
  }
  uint64_t total() const { return total_; }
  uint64_t file_bytes() const { return file_bytes_; }
  bool ok() const { return ok_; }

 private:
  FILE* file_;
  std::vector<uint8_t> raw_;
  std::vector<uint8_t> packed_;
  uint64_t total_ = 0;
  uint64_t file_bytes_ = 0;
  bool ok_ = true;
};

class Reader {
 public:
  explicit Reader(FILE* file) : file_(file) {}
  void Read(void* data, size_t length) {
    uint8_t* bytes = static_cast<uint8_t*>(data);
    while (length) {
      if (at_ == raw_.size() && !Fill()) {
        std::memset(bytes, 0, length);
        ok_ = false;
        return;
      }
      const size_t take = std::min(length, raw_.size() - at_);
      std::memcpy(bytes, raw_.data() + at_, take);
      at_ += take;
      bytes += take;
      length -= take;
    }
  }
  template <typename T>
  T Get() {
    T value = {};
    Read(&value, sizeof(T));
    return value;
  }
  bool ok() const { return ok_; }

 private:
  bool Fill() {
    uint32_t head[2];
    if (std::fread(head, sizeof(head), 1, file_) != 1 || head[0] > kChunk ||
        head[1] > ZSTD_compressBound(kChunk)) {
      return false;
    }
    packed_.resize(head[1]);
    if (std::fread(packed_.data(), 1, head[1], file_) != head[1]) {
      return false;
    }
    raw_.resize(head[0]);
    const size_t got =
        ZSTD_decompress(raw_.data(), raw_.size(), packed_.data(), head[1]);
    at_ = 0;
    return !ZSTD_isError(got) && got == head[0];
  }
  FILE* file_;
  std::vector<uint8_t> raw_;
  std::vector<uint8_t> packed_;
  size_t at_ = 0;
  bool ok_ = true;
};

// The file's blocks follow one another right up to its end (a file cut short is found before the game is
// touched). Leaves the file at |start|.
bool FileIsWhole(FILE* file, long start) {
  if (_fseeki64(file, 0, SEEK_END) != 0) {
    return false;
  }
  const int64_t size = _ftelli64(file);
  int64_t at = start;
  uint32_t blocks = 0;
  while (at < size) {
    uint32_t head[2];
    if (_fseeki64(file, at, SEEK_SET) != 0 || std::fread(head, sizeof(head), 1, file) != 1 || !head[0] ||
        head[0] > kChunk || !head[1] || head[1] > ZSTD_compressBound(kChunk)) {
      return false;
    }
    at += int64_t(sizeof(head)) + head[1];
    ++blocks;
  }
  return at == size && blocks && _fseeki64(file, start, SEEK_SET) == 0;
}

enum class Park : uint32_t {
  kNo = 0,         // can't be written down right now
  kSafepoint = 1,  // goes on at a JIT safepoint
  kWait = 2,       // asks its kernel wait again
  kNeverRan = 3,   // made but not yet started: nothing to keep
};

struct ThreadRecord {
  uint32_t thread_id;
  uint32_t handle;
  Park park;
  uint32_t pc;
  uint32_t suspend_count;
  uint32_t flags;  // kSkipSignal
};

// The thread sits in NtSignalAndWaitForSingleObjectEx after its signal: made again, it only waits.
constexpr uint32_t kSkipSignal = 1;
// The thread had suspended itself (it sits in NtSuspendThread until another thread resumes it): made again, the call
// does not count a second suspension, it only waits for the one the state already holds.
constexpr uint32_t kSkipSuspend = 2;

bool IsSelfSuspend(const std::string& name) {
  return name.find("NtSuspendThread") != std::string::npos;
}
bool IsSignalAndWait(const std::string& name) {
  return name.find("SignalAndWait") != std::string::npos;
}

// Kernel calls a thread may be parked in and that give the same result when simply made again.
bool IsRepeatableWait(const std::string& name) {
  static const char* const kNames[] = {
      "KeWaitForSingleObject",    "KeWaitForMultipleObjects",
      "NtWaitForSingleObjectEx",  "NtWaitForMultipleObjectsEx",
      "KeDelayExecutionThread",   "NtYieldExecution",
      "XamInputGetState",         "KeWaitForSingleObjectEx",
      // Made again with the signal left out (it was given before the wait began): see kSkipSignal.
      "NtSignalAndWaitForSingleObjectEx",
      // A network thread waiting on its socket events (NFS Most Wanted 2012 keeps one in it all the time, so every
      // save failed with "the game was busy"). Waiting again gives the same answer; the time-out starts over.
      "NetDll_WSAWaitForMultipleEvents",
  };
  for (const char* candidate : kNames) {
    if (name.find(candidate) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// Where |thread| is parked. |why| says what it is doing when it can't be written down.
Park Classify(Emulator* emulator, kernel::XThread* thread, uint32_t* out_pc,
              std::string* why) {
  auto& links = thread->scheduler_links();
  auto* context = thread->thread_state()->context();
  *out_pc = 0;
  if (!thread->dispatch_fiber()) {
    *why = "no fiber";
    return Park::kNo;
  }
  if (!links.has_run) {
    return Park::kNeverRan;
  }
  if (context->gv_host_depth != 1) {
    *why = fmt::format("inside a callback (depth {})", context->gv_host_depth);
    return Park::kNo;
  }
  if (context->gv_extern_pc) {
    auto* function = emulator->processor()->QueryFunction(context->gv_extern_pc);
    const std::string name = function ? function->name() : std::string("?");
    if (links.blocked && !links.wait_interruptible) {
      *why = "in a wait that can't be left: " + name;
      return Park::kNo;
    }
    if (IsSelfSuspend(name) && links.suspended && thread->suspend_count()) {
      // A worker that put itself to sleep (OutRun Online Arcade keeps one like this all the time). It never leaves
      // the call by itself, so waiting for it made every save fail with "the game was busy".
      *out_pc = context->gv_extern_pc;
      *why = name;
      return Park::kWait;
    }
    if (!IsRepeatableWait(name)) {
      *why = "inside " + name;
      return Park::kNo;
    }
    if (IsSignalAndWait(name) && !links.blocked) {
      *why = "about to signal in " + name;  // the signal may not have been given yet
      return Park::kNo;
    }
    *out_pc = context->gv_extern_pc;
    *why = name;
    return Park::kWait;
  }
  if (links.gv_at_safepoint) {
    if (!context->last_safepoint_pc) {
      *why = "safepoint without an address";
      return Park::kNo;
    }
    *out_pc = context->last_safepoint_pc;
    return Park::kSafepoint;
  }
  *why = "in host code";
  return Park::kNo;
}

// Holds the game's threads for as long as it lives (and leaves them held if they already were).
class Hold {
 public:
  explicit Hold(Emulator* emulator)
      : scheduler_(emulator->kernel_state()->guest_scheduler()) {
    was_paused_ = scheduler_->paused();
    scheduler_->SetPaused(true);
  }
  ~Hold() {
    if (!was_paused_) {
      scheduler_->SetPaused(false);
    }
  }
  bool Quiesce() { return scheduler_->GvQuiesce(3000); }
  // The game stays held for good (it is not fit to run on).
  void Keep() { was_paused_ = true; }
  // Lets the game run for a moment, then holds it again.
  void Nudge(int ms) {
    scheduler_->SetPaused(false);
    xe::threading::Sleep(std::chrono::milliseconds(ms));
    scheduler_->SetPaused(true);
  }
  kernel::GuestScheduler* scheduler() const { return scheduler_; }

 private:
  kernel::GuestScheduler* scheduler_;
  bool was_paused_ = false;
};

// Keeps the sound worker out of the game's callback for as long as it lives (made before the game is held: a
// callback under way must be able to end).
class SoundGate {
 public:
  explicit SoundGate(Emulator* emulator) : audio_(emulator->audio_system()) {
    ok_ = audio_->GvHoldWorker(2000);
  }
  ~SoundGate() {
    if (ok_) {
      audio_->GvReleaseWorker();
    }
  }
  bool ok() const { return ok_; }

 private:
  apu::AudioSystem* audio_;
  bool ok_ = false;
};

std::vector<kernel::object_ref<kernel::XThread>> GameThreads(Emulator* emulator) {
  auto all = emulator->kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
      kernel::XObject::Type::Thread);
  std::vector<kernel::object_ref<kernel::XThread>> threads;
  for (auto& thread : all) {
    bool listed = false;  // (a thread with two handles comes twice)
    for (auto& have : threads) listed |= have.get() == thread.get();
    // Running ones, and ones made but not started yet (not ones that have ended).
    if (!listed && thread->is_guest_thread() && thread->dispatch_fiber() && !thread->gv_forgot_guest_memory() &&
        (thread->is_running() || !thread->scheduler_links().has_run) &&
        !thread->scheduler_links().terminate_pending.load()) {
      threads.push_back(thread);
    }
  }
  return threads;
}

// The graphics thread has taken every command the (held) game queued.
bool WaitGpuIdle(Emulator* emulator) {
  auto* command_processor = emulator->graphics_system()->command_processor();
  for (int i = 0; i < 200; ++i) {
    if (command_processor->GvRingIdle()) {
      xe::threading::Sleep(std::chrono::milliseconds(30));
      if (command_processor->GvRingIdle()) {
        return true;
      }
    }
    xe::threading::Sleep(std::chrono::milliseconds(10));
  }
  return false;
}

std::map<uint32_t, uint32_t> CountObjects(Emulator* emulator, std::vector<uint32_t>* handles) {
  std::map<uint32_t, uint32_t> counts;
  for (auto& object : emulator->kernel_state()->object_table()->GetAllObjects()) {
    ++counts[uint32_t(object->type())];
    if (handles && !object->handles().empty()) {
      handles->push_back(object->handles()[0]);
    }
  }
  return counts;
}

}  // namespace

namespace {

// One kernel object of the game (not a thread, not one of Xenia's own) as the state knows it.
struct ObjectRecord {
  uint32_t type = 0;
  uint32_t restorable = 0;  // can be made again from |blob| if it was closed since
  uint64_t serial = 0;
  uint64_t file_position = 0;
  std::vector<std::pair<uint32_t, int32_t>> handles;  // handle, its count
  std::vector<uint8_t> blob;
};

// Told apart from other runs of Xenia: object serials only mean something within one run.
uint64_t SessionId() {
  static const uint64_t id = Clock::QueryHostSystemTime() ^ (Clock::QueryHostTickCount() << 20);
  return id;
}

bool IsGameObject(kernel::XObject* object) {
  return !object->is_host_object() && object->type() != kernel::XObject::Type::Thread &&
         !object->handles().empty();
}

const char* TypeName(uint32_t type) {
  static const char* const kNames[] = {"undefined", "enumerator", "event", "file", "io completion", "module",
                                       "mutant", "notify listener", "semaphore", "session", "socket",
                                       "symbolic link", "thread", "timer", "device"};
  return type < 15 ? kNames[type] : "?";
}

std::vector<ObjectRecord> CollectObjects(Emulator* emulator) {
  std::vector<ObjectRecord> records;
  auto* table = emulator->kernel_state()->object_table();
  std::vector<uint8_t> buffer(256 * 1024);
  for (auto& object : table->GetAllObjects()) {
    if (!IsGameObject(object.get())) {
      continue;
    }
    ObjectRecord record;
    record.type = uint32_t(object->type());
    record.serial = object->gv_serial();
    for (uint32_t handle : object->handles()) {
      record.handles.emplace_back(handle, table->GvHandleRefCount(handle));
    }
    bool can_save = false;
    switch (object->type()) {
      case kernel::XObject::Type::Event:
      case kernel::XObject::Type::Semaphore:
      case kernel::XObject::Type::NotifyListener:
      case kernel::XObject::Type::SymbolicLink:
      case kernel::XObject::Type::Timer:
      case kernel::XObject::Type::Session:  // (its Save says no when players on other consoles are in it)
        can_save = true;
        break;
      case kernel::XObject::Type::Mutant: {
        // Who holds it: the thread's number in the low half, how many times over in the high half.
        uint32_t owner = 0, recursion = 0;
        static_cast<kernel::XMutant*>(object.get())->GvGetOwner(&owner, &recursion);
        record.file_position = uint64_t(owner) | (uint64_t(recursion) << 32);
        can_save = true;
        break;
      }
      case kernel::XObject::Type::File:
        record.file_position = static_cast<kernel::XFile*>(object.get())->position();
        can_save = static_cast<kernel::XFile*>(object.get())->file() != nullptr;
        break;
      default:
        break;
    }
    if (can_save) {
      ByteStream stream(buffer.data(), buffer.size());
      if (object->Save(&stream) && stream.offset() < buffer.size()) {
        record.blob.assign(buffer.data(), buffer.data() + stream.offset());
        record.restorable = 1;
      }
    }
    records.push_back(std::move(record));
  }
  return records;
}

// Kinds a state from another run can do without if they are missing (the game gets an error on that handle).
bool IsSoftKind(uint32_t type) {
  return type == uint32_t(kernel::XObject::Type::Enumerator) || type == uint32_t(kernel::XObject::Type::Socket) ||
         type == uint32_t(kernel::XObject::Type::File);
}

std::string build_tag_;

std::atomic<uint64_t> load_guard_until_ms{0};
std::atomic<uint32_t> load_watch_id{0};
}  // namespace

void GvSetStateBuildTag(const std::string& tag) { build_tag_ = tag; }

void GvCloseIfCrashAfterLoad(const char* what) {
  if (Clock::QueryHostUptimeMillis() >= load_guard_until_ms.load()) {
    return;
  }
  XELOGE("Game Vault: load state: the game crashed right after a state was loaded ({}): closing Xenia", what);
  xe::FlushLog();
  std::quick_exit(3);
}

bool GvSaveState(Emulator* emulator, const std::filesystem::path& path,
                 std::string* message) {
  if (!emulator->is_title_open() || !kernel::GuestScheduler::enabled()) {
    *message = "No game is running";
    return false;
  }
  const uint64_t started_ms = Clock::QueryHostUptimeMillis();
  SoundGate sound_gate(emulator);
  if (!sound_gate.ok()) {
    *message = "Save failed: the game's sound was busy";
    XELOGE("Game Vault: save state: REFUSED, the sound callback did not end within 2 s");
    return false;
  }
  Hold hold(emulator);

  // Each thread parked where it can be written down; the game is let run a little between tries.
  std::vector<kernel::object_ref<kernel::XThread>> threads;
  std::vector<ThreadRecord> records;
  std::string blocker;
  bool settled = false;
  int tries = 0;
  for (; tries < 400 && !settled; ++tries) {
    if (!hold.Quiesce()) {
      *message = "Save failed: the game's threads did not stop";
      XELOGE("Game Vault: save state: threads did not stop");
      return false;
    }
    threads = GameThreads(emulator);
    records.clear();
    settled = true;
    for (auto& thread : threads) {
      ThreadRecord record = {};
      std::string why;
      record.thread_id = thread->thread_id();
      record.handle = thread->handle();
      record.park = Classify(emulator, thread.get(), &record.pc, &why);
      record.suspend_count = thread->suspend_count();
      if (record.park == Park::kWait && IsSignalAndWait(why)) {
        record.flags |= kSkipSignal;
      }
      if (record.park == Park::kWait && IsSelfSuspend(why)) {
        record.flags |= kSkipSuspend;
      }
      if (record.park == Park::kNo) {
        settled = false;
        blocker = fmt::format("thread {:08X} '{}' {}", thread->thread_id(),
                              thread->thread_name(), why);
        break;
      }
      records.push_back(record);
    }
    if (!settled) {
      if (tries < 5 || tries % 50 == 0) {
        XELOGI("Game Vault: save state: try {}: {}", tries + 1, blocker);
      }
      hold.Nudge(1 + tries % 7);
    }
  }
  if (!settled) {
    *message = "Save failed: the game was busy (" + blocker + ")";
    XELOGE("Game Vault: save state: gave up after {} tries: {}", tries, blocker);
    return false;
  }
  for (size_t i = 0; i < threads.size(); ++i) {
    std::string why;
    uint32_t pc = 0;
    Classify(emulator, threads[i].get(), &pc, &why);
    XELOGI(
        "Game Vault: save state: thread {:08X} '{}': {} at {:08X} lr {:08X} {}{}",
        records[i].thread_id, threads[i]->thread_name(),
        records[i].park == Park::kSafepoint ? "running"
        : records[i].park == Park::kWait    ? "waiting"
                                            : "not started",
        records[i].pc, uint32_t(threads[i]->thread_state()->context()->lr), why,
        records[i].suspend_count ? " (suspended)" : "");
  }
  if (!WaitGpuIdle(emulator)) {
    XELOGW("Game Vault: save state: the graphics thread still has work queued");
  }

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path part = path;
  part += ".part";
  {
    // Room for the file (the old state stays until the new one is whole) and some to spare for the game's own saves.
    const auto space = std::filesystem::space(path.parent_path(), ec);
    if (!ec && space.available != static_cast<std::uintmax_t>(-1)) {
      XELOGI("Game Vault: save state: {} MB of storage free", space.available / (1024 * 1024));
      if (space.available < 400ull * 1024 * 1024) {
        *message = "Save failed: not enough free storage";
        XELOGE("Game Vault: save state: REFUSED, only {} MB of storage free", space.available / (1024 * 1024));
        return false;
      }
    }
    ec.clear();
  }
  FILE* file = xe::filesystem::OpenFile(part, "wb");
  if (!file) {
    *message = "Save failed: the file can't be made";
    return false;
  }
  const uint32_t head[4] = {kMagic, kVersion, emulator->title_id(),
                            uint32_t(kGuestRegsSize)};
  std::fwrite(head, sizeof(head), 1, file);
  Writer writer(file);
  uint64_t mark = 0;
  auto section = [&](const char* name) {
    XELOGI("Game Vault: save state: {}: {} KB", name, (writer.total() - mark) / 1024);
    mark = writer.total();
  };

  // The game's clock, then the threads.
  writer.Put(uint64_t(Clock::QueryGuestTickCount()));
  writer.Put(uint32_t(records.size()));
  {
    std::vector<uint8_t> buffer(64 * 1024);
    auto* table = emulator->kernel_state()->object_table();
    for (size_t i = 0; i < records.size(); ++i) {
      writer.Put(records[i]);
      writer.Write(threads[i]->thread_state()->context(), kGuestRegsSize);
      // What it takes to make the thread again if it has ended by the time of a load.
      writer.Put(uint64_t(threads[i]->gv_serial()));
      ByteStream stream(buffer.data(), buffer.size());
      const bool saved = threads[i]->GvSave(&stream);
      writer.Put(uint32_t(saved ? stream.offset() : 0));
      writer.Write(buffer.data(), saved ? stream.offset() : 0);
      writer.Put(uint32_t(threads[i]->handles().size()));
      for (uint32_t handle : threads[i]->handles()) {
        writer.Put(handle);
        writer.Put(int32_t(table->GvHandleRefCount(handle)));
      }
    }
  }
  section("threads");

  // Kernel objects: only what is there, for the load to compare with.
  writer.Put(SessionId());
  auto objects = CollectObjects(emulator);
  writer.Put(uint32_t(objects.size()));
  std::map<uint32_t, uint32_t> counts;
  uint32_t not_restorable = 0;
  for (const ObjectRecord& object : objects) {
    ++counts[object.type];
    not_restorable += !object.restorable;
    writer.Put(object.type);
    writer.Put(object.restorable);
    writer.Put(object.serial);
    writer.Put(object.file_position);
    writer.Put(uint32_t(object.handles.size()));
    for (auto& [handle, count] : object.handles) {
      writer.Put(handle);
      writer.Put(count);
    }
    writer.Put(uint32_t(object.blob.size()));
    writer.Write(object.blob.data(), object.blob.size());
  }
  std::string count_text;
  for (auto& [type, count] : counts) {
    count_text += fmt::format(" {} {},", count, TypeName(type));
  }
  XELOGI("Game Vault: save state: kernel objects:{} of them {} that can't be made again if closed",
         count_text, not_restorable);
  {
    // Objects Xenia made for structures the game set up itself: only which ones there are.
    std::vector<uint64_t> natives;
    for (auto& object : emulator->kernel_state()->object_table()->GetAllObjects()) {
      if (object->gv_is_native() && object->type() != kernel::XObject::Type::Thread) {
        natives.push_back(object->gv_serial());
      }
    }
    writer.Put(uint32_t(natives.size()));
    writer.Write(natives.data(), natives.size() * sizeof(uint64_t));
    // Sound: the game's callbacks, and which decoder contexts it holds.
    auto clients = emulator->audio_system()->GvClients();
    writer.Put(uint32_t(clients.size()));
    writer.Write(clients.data(), clients.size() * sizeof(clients[0]));
    std::vector<uint64_t> xma;
    if (auto* decoder = emulator->audio_system()->xma_decoder()) {
      xma = decoder->gv_context_bitmap().data();
    }
    writer.Put(uint32_t(xma.size()));
    writer.Write(xma.data(), xma.size() * sizeof(uint64_t));
    writer.Put(uint32_t(build_tag_.size()));
    writer.Write(build_tag_.data(), build_tag_.size());
    XELOGI("Game Vault: save state: {} native objects, {} sound clients, build {}", natives.size(), clients.size(),
           build_tag_);
    uint64_t sound_frames = 0;
    float sound_peak = 0.0f;
    emulator->audio_system()->GvSoundStats(1, &sound_frames, &sound_peak);
    XELOGI("Game Vault: save state: sound before the save: loudest {:.2f} since the last save or load", sound_peak);
  }
  section("kernel objects");

  // Graphics: registers and where the command ring stands.
  auto* graphics = emulator->graphics_system();
  writer.Write(graphics->register_file()->values,
               sizeof(graphics->register_file()->values));
  uint8_t ring[64] = {};
  ByteStream ring_stream(ring, sizeof(ring));
  graphics->Save(&ring_stream);
  writer.Write(ring, sizeof(ring));
  {
    // The picture's brightness tables (the game sets them now and then, not every frame).
    std::vector<gpu::reg::DC_LUT_30_COLOR> table(256);
    std::vector<gpu::reg::DC_LUT_PWL_DATA> pwl(3 * 128);
    uint32_t component = 0;
    graphics->command_processor()->GvGetGammaRamp(table.data(), pwl.data(), &component);
    writer.Write(table.data(), sizeof(gpu::reg::DC_LUT_30_COLOR) * 256);
    writer.Write(pwl.data(), sizeof(gpu::reg::DC_LUT_PWL_DATA) * 3 * 128);
    writer.Put(component);
  }
  section("graphics");

  // Memory.
  uint64_t memory_bytes = 0;
  emulator->memory()->GvSaveState(
      [&writer](const void* data, size_t length) { writer.Write(data, length); },
      &memory_bytes);
  section("memory");
  writer.Flush();
  const bool ok = writer.ok() && std::fflush(file) == 0;
  std::fclose(file);
  if (!ok) {
    std::filesystem::remove(part, ec);
    *message = "Save failed: the file could not be written (storage full?)";
    XELOGE("Game Vault: save state: write failed");
    return false;
  }
  std::filesystem::remove(path, ec);
  std::filesystem::rename(part, path, ec);
  if (ec) {
    *message = "Save failed: the file could not be put in place";
    return false;
  }
  *message = "State saved";
  XELOGI(
      "Game Vault: save state: SAVED {} threads, {} MB of memory, file {} MB, {} "
      "tries, {} ms",
      records.size(), memory_bytes / (1024 * 1024),
      (writer.file_bytes() + sizeof(head)) / (1024 * 1024), tries,
      Clock::QueryHostUptimeMillis() - started_ms);
  return true;
}

bool GvLoadState(Emulator* emulator, const std::filesystem::path& path,
                 std::string* message) {
  if (!emulator->is_title_open() || !kernel::GuestScheduler::enabled()) {
    *message = "No game is running";
    return false;
  }
  const uint64_t started_ms = Clock::QueryHostUptimeMillis();
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    *message = "No saved state for this game";
    return false;
  }
  struct Closer {
    FILE* file;
    ~Closer() { std::fclose(file); }
  } closer{file};
  uint32_t head[4] = {};
  if (std::fread(head, sizeof(head), 1, file) != 1 || head[0] != kMagic ||
      head[1] != kVersion || head[3] != uint32_t(kGuestRegsSize)) {
    *message = "Load failed: the state is from another version";
    return false;
  }
  if (head[2] != emulator->title_id()) {
    *message = "Load failed: the state is from another game";
    return false;
  }
  if (!FileIsWhole(file, sizeof(head))) {
    *message = "Load failed: the state file is damaged";
    XELOGE("Game Vault: load state: REFUSED, the state file is cut short or damaged");
    return false;
  }
  Reader reader(file);
  const uint64_t guest_ticks = reader.Get<uint64_t>();
  const uint32_t thread_count = reader.Get<uint32_t>();
  if (!reader.ok() || thread_count > 4096) {
    *message = "Load failed: the state file is damaged";
    return false;
  }
  std::vector<ThreadRecord> records(thread_count);
  std::vector<std::vector<uint8_t>> registers(thread_count);
  std::vector<uint64_t> thread_serials(thread_count);
  std::vector<std::vector<uint8_t>> thread_blobs(thread_count);
  std::vector<std::vector<std::pair<uint32_t, int32_t>>> thread_handles(thread_count);
  for (uint32_t i = 0; i < thread_count; ++i) {
    records[i] = reader.Get<ThreadRecord>();
    registers[i].resize(kGuestRegsSize);
    reader.Read(registers[i].data(), kGuestRegsSize);
    thread_serials[i] = reader.Get<uint64_t>();
    const uint32_t blob_size = reader.Get<uint32_t>();
    if (!reader.ok() || blob_size > 64 * 1024) {
      *message = "Load failed: the state file is damaged";
      return false;
    }
    thread_blobs[i].resize(blob_size);
    reader.Read(thread_blobs[i].data(), blob_size);
    const uint32_t handle_count = reader.Get<uint32_t>();
    if (!reader.ok() || handle_count > 4096) {
      *message = "Load failed: the state file is damaged";
      return false;
    }
    for (uint32_t h = 0; h < handle_count; ++h) {
      const uint32_t handle = reader.Get<uint32_t>();
      thread_handles[i].emplace_back(handle, reader.Get<int32_t>());
    }
  }
  const bool same_session = reader.Get<uint64_t>() == SessionId();
  const uint32_t object_count = reader.Get<uint32_t>();
  if (!reader.ok() || object_count > 65536) {
    *message = "Load failed: the state file is damaged";
    return false;
  }
  std::vector<ObjectRecord> saved_objects(object_count);
  for (ObjectRecord& object : saved_objects) {
    object.type = reader.Get<uint32_t>();
    object.restorable = reader.Get<uint32_t>();
    object.serial = reader.Get<uint64_t>();
    object.file_position = reader.Get<uint64_t>();
    const uint32_t handle_count = reader.Get<uint32_t>();
    if (!reader.ok() || handle_count > 4096) {
      *message = "Load failed: the state file is damaged";
      return false;
    }
    for (uint32_t i = 0; i < handle_count; ++i) {
      const uint32_t handle = reader.Get<uint32_t>();
      object.handles.emplace_back(handle, reader.Get<int32_t>());
    }
    const uint32_t blob_size = reader.Get<uint32_t>();
    if (!reader.ok() || blob_size > 256 * 1024) {
      *message = "Load failed: the state file is damaged";
      return false;
    }
    object.blob.resize(blob_size);
    reader.Read(object.blob.data(), blob_size);
  }
  std::set<uint64_t> saved_natives;
  std::vector<std::array<uint32_t, 3>> saved_clients;
  std::vector<uint64_t> saved_xma;
  std::string saved_build;
  {
    const uint32_t native_count = reader.Get<uint32_t>();
    if (!reader.ok() || native_count > (1u << 20)) {
      *message = "Load failed: the state file is damaged";
      return false;
    }
    for (uint32_t i = 0; i < native_count; ++i) {
      saved_natives.insert(reader.Get<uint64_t>());
    }
    const uint32_t client_count = reader.Get<uint32_t>();
    if (!reader.ok() || client_count > 64) {
      *message = "Load failed: the state file is damaged";
      return false;
    }
    saved_clients.resize(client_count);
    reader.Read(saved_clients.data(), client_count * sizeof(saved_clients[0]));
    const uint32_t xma_count = reader.Get<uint32_t>();
    if (!reader.ok() || xma_count > 4096) {
      *message = "Load failed: the state file is damaged";
      return false;
    }
    saved_xma.resize(xma_count);
    reader.Read(saved_xma.data(), xma_count * sizeof(uint64_t));
    const uint32_t build_size = reader.Get<uint32_t>();
    if (!reader.ok() || build_size > 64) {
      *message = "Load failed: the state file is damaged";
      return false;
    }
    saved_build.resize(build_size);
    reader.Read(saved_build.data(), build_size);
  }
  XELOGI("Game Vault: load state: file read; made in {} run of Xenia, build {}", same_session ? "this" : "another",
         saved_build);
  if (!same_session && saved_build != build_tag_) {
    *message = "Load refused: this state is from another version of Xenia";
    XELOGE("Game Vault: load state: REFUSED, the state is from build {}, this is {}", saved_build, build_tag_);
    return false;
  }
  if (!reader.ok()) {
    *message = "Load failed: the state file is damaged";
    return false;
  }

  SoundGate sound_gate(emulator);
  if (!sound_gate.ok()) {
    *message = "Load failed: the game's sound was busy";
    XELOGE("Game Vault: load state: REFUSED, the sound callback did not end within 2 s");
    return false;
  }
  Hold hold(emulator);
  // The threads as they are now: each must be parked where it can be made to leave what it is doing.
  std::vector<kernel::object_ref<kernel::XThread>> threads;
  std::string blocker;
  bool settled = false;
  for (int tries = 0; tries < 400 && !settled; ++tries) {
    if (!hold.Quiesce()) {
      *message = "Load failed: the game's threads did not stop";
      return false;
    }
    threads = GameThreads(emulator);
    settled = true;
    for (auto& thread : threads) {
      auto& links = thread->scheduler_links();
      if (links.blocked && !links.wait_interruptible) {
        settled = false;
        blocker = fmt::format("thread {:08X} '{}'", thread->thread_id(),
                              thread->thread_name());
        break;
      }
    }
    if (!settled) {
      hold.Nudge(1 + tries % 7);
    }
  }
  if (!settled) {
    *message = "Load failed: the game was busy (" + blocker + ")";
    return false;
  }
  // Each thread of the state: still there (it is sent back to where the state has it), or ended since - or
  // started since, when the state has it as not started - and then made again. Threads the state does not
  // know are ended.
  std::vector<kernel::XThread*> thread_now(thread_count, nullptr);
  std::vector<bool> thread_adopt(thread_count, false);
  std::vector<kernel::XThread*> threads_to_end;
  uint32_t threads_to_make = 0;
  {
    std::map<uint64_t, kernel::XThread*> by_serial;
    for (auto& thread : threads) {
      by_serial[thread->gv_serial()] = thread.get();
    }
    for (uint32_t i = 0; i < thread_count; ++i) {
      auto it = by_serial.end();
      if (same_session) {
        it = by_serial.find(thread_serials[i]);
        if (it != by_serial.end() &&
            (records[i].park == Park::kNeverRan) != !it->second->scheduler_links().has_run) {
          it = by_serial.end();
        }
      } else if (records[i].park != Park::kNeverRan && !thread_blobs[i].empty()) {
        // Another run of Xenia: a running thread under the same number and handle becomes the state's.
        for (auto n = by_serial.begin(); n != by_serial.end(); ++n) {
          if (n->second->thread_id() == records[i].thread_id && n->second->handle() == records[i].handle &&
              n->second->scheduler_links().has_run) {
            it = n;
            thread_adopt[i] = true;
            break;
          }
        }
      }
      if (it != by_serial.end()) {
        thread_now[i] = it->second;
        by_serial.erase(it);
        continue;
      }
      if (thread_blobs[i].empty()) {
        *message = "Load failed: a thread of the state can't be made again";
        return false;
      }
      ++threads_to_make;
    }
    for (auto& [serial, thread] : by_serial) {
      threads_to_end.push_back(thread);
    }
    XELOGI("Game Vault: load state: threads: {} still there, {} to make again, {} newer ones to end",
           thread_count - threads_to_make, threads_to_make, threads_to_end.size());
  }
  if (!WaitGpuIdle(emulator)) {
    XELOGW("Game Vault: load state: the graphics thread still has work queued");
  }
  // The game's graphics interrupt callback runs game code on the graphics thread: it must not run while the
  // memory goes back (NFS Most Wanted 2012, which keeps the graphics thread busy reading data back, crashed in it
  // reading 0BADF00D half-way through the load). Held, not dropped, until the load is through or given up.
  struct CallbackHold {
    gpu::GraphicsSystem* graphics;
    explicit CallbackHold(gpu::GraphicsSystem* g) : graphics(g) { graphics->GvHoldGuestCallbacks(true); }
    ~CallbackHold() { graphics->GvHoldGuestCallbacks(false); }
  } callback_hold(emulator->graphics_system());

  // Kernel objects, both ways. One the game closed since the save is made again after its memory is back (if its
  // kind can be: else the load is refused here, before anything is changed - Kameo crashed going on with handles
  // that led nowhere). One the game made since the save is unknown to the state: its handles are taken away.
  auto* table = emulator->kernel_state()->object_table();
  std::vector<ObjectRecord*> to_make;
  std::vector<std::pair<kernel::XObject*, ObjectRecord*>> kept;
  std::vector<kernel::object_ref<kernel::XObject>> kept_refs;
  std::vector<uint32_t> to_drop;
  {
    std::map<uint64_t, kernel::object_ref<kernel::XObject>> now;
    for (auto& object : table->GetAllObjects()) {
      if (IsGameObject(object.get())) {
        now[object->gv_serial()] = object;
      }
    }
    uint32_t lost = 0;
    std::string lost_text;
    uint32_t soft = 0;
    for (ObjectRecord& object : saved_objects) {
      auto it = now.end();
      if (same_session) {
        it = now.find(object.serial);
      } else if (!object.restorable && !object.handles.empty()) {
        // Another run of Xenia: a kind that can't be made again is taken as the same if it sits on the same handle.
        for (auto n = now.begin(); n != now.end(); ++n) {
          if (uint32_t(n->second->type()) == object.type && n->second->handles()[0] == object.handles[0].first) {
            it = n;
            break;
          }
        }
      }
      if (it != now.end()) {
        kept.emplace_back(it->second.get(), &object);
        kept_refs.push_back(it->second);
        now.erase(it);
      } else if (object.restorable) {
        to_make.push_back(&object);
      } else if (IsSoftKind(object.type)) {
        ++soft;
        XELOGW("Game Vault: load state: a {} of the state (handle {:08X}) is not there and can't be made again; the "
               "game goes on without it", TypeName(object.type), object.handles.empty() ? 0 : object.handles[0].first);
      } else {
        ++lost;
        lost_text += fmt::format(" {} {:08X}", TypeName(object.type),
                                 object.handles.empty() ? 0 : object.handles[0].first);
      }
    }
    for (auto& [serial, object] : now) {
      for (uint32_t handle : object->handles()) {
        to_drop.push_back(handle);
      }
    }
    uint32_t natives_dropped = 0;
    for (auto& object : table->GetAllObjects()) {
      if (object->gv_is_native() && object->type() != kernel::XObject::Type::Thread &&
          (!same_session || !saved_natives.count(object->gv_serial()))) {
        ++natives_dropped;
        for (uint32_t handle : object->handles()) {
          to_drop.push_back(handle);
        }
      }
    }
    XELOGI("Game Vault: load state: {} native objects the state does not have go (made anew when the game uses them)",
           natives_dropped);
    XELOGI(
        "Game Vault: load state: kernel objects: {} still there, {} closed since the save (made again), {} made "
        "since (taken away), {} gone for good{}{}",
        kept.size(), to_make.size(), now.size(), lost, same_session ? "" : " [state from another run]", lost_text);
    if (lost) {
      *message = fmt::format(
          "Load refused: the game has moved on too far from this state ({} of its "
          "system objects are gone)", lost);
      XELOGE("Game Vault: load state: REFUSED, {} kernel objects of the state can't be made again", lost);
      return false;
    }
  }
  // Nothing below may wait for ever: the lock around game memory must be free now (a parked thread holding it
  // would hang the load half done).
  {
    auto lock = global_critical_region::AcquireDeferred();
    bool got = false;
    for (int i = 0; i < 200 && !(got = lock.try_lock()); ++i) {
      xe::threading::Sleep(std::chrono::milliseconds(10));
    }
    if (!got) {
      *message = "Load refused: the game was busy (memory lock held)";
      XELOGE("Game Vault: load state: REFUSED, the global lock stayed held for 2 s");
      return false;
    }
  }
  // From here the game is changed: no way back if something fails.
  // No thread stays signed up as a waiter on an object that may go.
  for (auto& thread : threads) {
    kernel::XObject::AbandonCooperativeWait(thread.get());
  }
  // Threads the state does not have end now, and leave game memory alone when they are cleared away later.
  for (kernel::XThread* thread : threads_to_end) {
    XELOGI("Game Vault: load state: thread {:08X} '{}' is not in the state: ended",
           thread->thread_id(), thread->thread_name());
    thread->GvForgetGuestMemory();
    const std::vector<uint32_t> handles(thread->handles().begin(), thread->handles().end());
    thread->Terminate(0);
    for (uint32_t handle : handles) {
      table->GvDropHandle(handle);
    }
  }
  // Threads that ENDED by themselves since the save are still there as objects until the game closes their handles,
  // and free their stack and thread blocks in game memory when they finally go. After the load that memory belongs
  // to the thread made again in their place (or to whatever the state has there), so they must leave it alone:
  // OutRun Online Arcade's start-up thread ends seconds after the title screen, and a load from then on crashed the
  // game a few seconds later.
  {
    uint64_t newest_serial = 0;
    for (uint64_t serial : thread_serials) {
      newest_serial = std::max(newest_serial, serial);
    }
    uint32_t left_alone = 0;
    const auto forget = [&left_alone](kernel::XThread* old) {
      if (old && old->is_guest_thread() && !old->gv_forgot_guest_memory() && !old->is_running() &&
          old->scheduler_links().has_run) {
        old->GvForgetGuestMemory();
        ++left_alone;
      }
    };
    for (uint32_t i = 0; i < thread_count; ++i) {
      if (!thread_now[i]) {
        forget(emulator->kernel_state()->GetThreadByID(records[i].thread_id).get());
      }
    }
    for (auto& old : table->GetObjectsByType<kernel::XThread>(kernel::XObject::Type::Thread)) {
      // From another run every thread of this run is foreign to the state; in the same run, the ones made after it.
      if (!same_session || old->gv_serial() > newest_serial) {
        forget(old.get());
      }
    }
    if (left_alone) {
      XELOGI("Game Vault: load state: {} ended thread(s) will not free game memory when cleared away", left_alone);
    }
  }
  // Objects made since the save go while the memory they sit in is still theirs.
  XELOGI("Game Vault: load state: step 1, {} newer handles taken away", to_drop.size());
  xe::FlushLog();
  for (uint32_t handle : to_drop) {
    table->GvDropHandle(handle);
  }
  XELOGI("Game Vault: load state: step 2, memory");
  xe::FlushLog();
  load_guard_until_ms.store(Clock::QueryHostUptimeMillis() + 30000);

  auto* graphics = emulator->graphics_system();
  reader.Read(graphics->register_file()->values,
              sizeof(graphics->register_file()->values));
  uint8_t ring[64] = {};
  reader.Read(ring, sizeof(ring));
  ByteStream ring_stream(ring, sizeof(ring));
  graphics->Restore(&ring_stream);
  {
    std::vector<gpu::reg::DC_LUT_30_COLOR> table(256);
    std::vector<gpu::reg::DC_LUT_PWL_DATA> pwl(3 * 128);
    reader.Read(table.data(), sizeof(gpu::reg::DC_LUT_30_COLOR) * 256);
    reader.Read(pwl.data(), sizeof(gpu::reg::DC_LUT_PWL_DATA) * 3 * 128);
    const uint32_t component = reader.Get<uint32_t>();
    if (reader.ok()) {
      graphics->command_processor()->RestoreGammaRamp(table.data(), pwl.data(), component);
    }
  }

  uint64_t memory_bytes = 0;
  const bool memory_ok = emulator->memory()->GvRestoreState(
      [&reader](void* data, size_t length) { reader.Read(data, length); },
      &memory_bytes);
  if (!memory_ok || !reader.ok()) {
    // The game's memory is half the state's now: it stays held, and the caller closes the game.
    hold.Keep();
    load_guard_until_ms.store(0);
    *message = "Load failed part-way: the state file is damaged";
    XELOGE("Game Vault: load state: memory restore failed (ok={}, file ok={}); the game stays held",
           memory_ok, reader.ok());
    xe::FlushLog();
    return false;
  }

  // The game's clock goes on from the moment of the save (its timers would jump otherwise).
  Clock::GvSetGuestTickCount(guest_ticks);
  emulator->kernel_state()->UpdateKeTimestampBundle();

  // Objects closed since the save are made again (their memory is back), files are put where they stood, and
  // events, semaphores and mutants take their state from the game memory just put back.
  uint32_t synced = 0, made_again = 0;
  XELOGI("Game Vault: load state: step 3, memory back ({} MB), objects", memory_bytes / (1024 * 1024));
  xe::FlushLog();
  for (ObjectRecord* object : to_make) {
    XELOGI("Game Vault: load state: making a {} again, handle {:08X}", TypeName(object->type),
           object->handles.empty() ? 0 : object->handles[0].first);
    xe::FlushLog();
    for (auto& [handle, count] : object->handles) {
      if (table->GvHandleRefCount(handle) || table->LookupObject<kernel::XObject>(handle)) {
        table->GvDropHandle(handle);  // a kept object took this number since the save
      }
    }
    for (auto& [handle, count] : object->handles) {
      table->GvEnsureSlot(handle);
    }
    ByteStream stream(object->blob.data(), object->blob.size());
    kernel::object_ref<kernel::XObject> restored;
    if (object->type == uint32_t(kernel::XObject::Type::Mutant)) {
      restored = kernel::XMutant::GvRestore(emulator->kernel_state(), &stream);
    } else if (object->type == uint32_t(kernel::XObject::Type::Timer)) {
      restored = kernel::XTimer::GvRestore(emulator->kernel_state(), &stream);
    } else if (object->type == uint32_t(kernel::XObject::Type::Session)) {
      restored = kernel::XSession::GvRestore(emulator->kernel_state(), &stream);
    } else {
      restored = kernel::XObject::Restore(emulator->kernel_state(), kernel::XObject::Type(object->type), &stream);
    }
    if (!restored) {
      XELOGE("Game Vault: load state: a {} could not be made again", TypeName(object->type));
      continue;
    }
    if (restored->type() == kernel::XObject::Type::File &&
        !static_cast<kernel::XFile*>(restored.get())->file()) {
      XELOGW("Game Vault: load state: file {:08X} could not be opened again; the game goes on without it",
             object->handles.empty() ? 0 : object->handles[0].first);
      for (auto& [handle, count] : object->handles) {
        table->GvDropHandle(handle);
      }
      continue;
    }
    for (auto& [handle, count] : object->handles) {
      table->GvSetHandleRefCount(handle, std::max(count, 1));
    }
    ++made_again;
  }
  // Sound: decoder contexts as the state has them; from another run, the game's callbacks too.
  if (auto* decoder = emulator->audio_system()->xma_decoder()) {
    if (decoder->gv_context_bitmap().data().size() == saved_xma.size()) {
      decoder->gv_context_bitmap().data() = saved_xma;
    }
  }
  if (!same_session) {
    emulator->audio_system()->GvMatchClients(saved_clients);
  }
  XELOGI("Game Vault: load state: step 4, {} objects made again; files, then re-reading", made_again);
  xe::FlushLog();
  for (auto& [object, record] : kept) {
    if (object->type() == kernel::XObject::Type::File) {
      static_cast<kernel::XFile*>(object)->set_position(record->file_position);
    }
    if (object->type() == kernel::XObject::Type::Session && record->restorable && !record->blob.empty()) {
      ByteStream stream(record->blob.data(), record->blob.size());
      static_cast<kernel::XSession*>(object)->GvApply(&stream);
    }
  }
  for (auto& object : table->GetAllObjects()) {
    if (kernel::XObject::HasDispatcherHeader(object->type()) &&
        object->type() != kernel::XObject::Type::Thread) {
      object->GvSyncFromGuest();
      ++synced;
    }
  }

  // Threads: registers back, then each leaves what it was doing and goes on where the state says.
  const uint32_t ended = uint32_t(threads_to_end.size());
  XELOGI("Game Vault: load state: step 5, {} objects re-read; threads", synced);
  xe::FlushLog();
  uint32_t threads_made = 0;
  for (uint32_t i = 0; i < thread_count; ++i) {
    const ThreadRecord& record = records[i];
    kernel::XThread* thread = thread_now[i];
    kernel::object_ref<kernel::XThread> made;
    if (!thread) {
      XELOGI("Game Vault: load state: making thread {:08X} again at {:08X}", record.thread_id, record.pc);
      xe::FlushLog();
      for (auto& [handle, count] : thread_handles[i]) {
        if (table->LookupObject<kernel::XObject>(handle)) {
          table->GvDropHandle(handle);
        }
      }
      for (auto& [handle, count] : thread_handles[i]) {
        table->GvEnsureSlot(handle);
      }
      ByteStream stream(thread_blobs[i].data(), thread_blobs[i].size());
      made = kernel::XThread::GvRestore(emulator->kernel_state(), &stream,
                                        record.park == Park::kNeverRan ? 0 : record.pc);
      if (!made) {
        XELOGE("Game Vault: load state: thread {:08X} could not be made again", record.thread_id);
        continue;
      }
      for (auto& [handle, count] : thread_handles[i]) {
        table->GvSetHandleRefCount(handle, std::max(count, 1));
      }
      thread = made.get();
      ++threads_made;
    } else if (record.park == Park::kNeverRan) {
      continue;  // not started then, not started now
    } else if (thread_adopt[i]) {
      ByteStream stream(thread_blobs[i].data(), thread_blobs[i].size());
      if (!thread->GvAdopt(&stream)) {
        XELOGE("Game Vault: load state: thread {:08X} could not take the state's place", record.thread_id);
      }
    }
    // (Host pointers behind the guest registers are the thread's own and stay.)
    std::memcpy(thread->thread_state()->context(), registers[i].data(),
                kGuestRegsSize);
    // The thread's block in game memory names its host register block; a state from another run of Xenia
    // holds that run's address.
    emulator->memory()
        ->TranslateVirtual<kernel::X_KPCR*>(thread->pcr_ptr())
        ->host_stash = reinterpret_cast<uint64_t>(thread->thread_state()->context());
    thread->gv_skip_signal_once = record.park == Park::kWait && (record.flags & kSkipSignal);
    thread->gv_skip_suspend_once = record.park == Park::kWait && (record.flags & kSkipSuspend);
    if (made) {
      if (record.park != Park::kNeverRan || !record.suspend_count) {
        hold.scheduler()->MarkReady(thread);
      }
    } else {
      hold.scheduler()->GvReenterThread(thread, record.pc);
    }
  }
  // Mutants are held by who held them in the state (the threads are all there now).
  for (const ObjectRecord& object : saved_objects) {
    if (object.type != uint32_t(kernel::XObject::Type::Mutant) || object.handles.empty()) {
      continue;
    }
    auto found = table->LookupObject<kernel::XObject>(object.handles[0].first);
    if (!found || found->type() != kernel::XObject::Type::Mutant) {
      continue;
    }
    auto* mutant = static_cast<kernel::XMutant*>(found.get());
    const uint32_t owner_id = uint32_t(object.file_position);
    auto owner = owner_id ? emulator->kernel_state()->GetThreadByID(owner_id) : nullptr;
    if (owner_id && !owner) {
      XELOGE("Game Vault: load state: mutant {:08X}: its holder, thread {:08X}, is not there", object.handles[0].first, owner_id);
    }
    mutant->GvSetOwner(owner.get(), uint32_t(object.file_position >> 32));
  }
  // Watch: the game must show a new picture within 12 s of going on, else Xenia closes instead of sitting hung.
  {
    const uint32_t watch = ++load_watch_id;
    // (Frames the presenter was given; Xenia's log frame number never moves in this build, which closed Xenia
    // 12 s after every load in 1.1.8.273.)
    auto frames = [emulator]() -> uint64_t {
      auto* presenter = emulator->graphics_system()->presenter();
      return presenter ? presenter->guest_output_refresh_count() : 0;
    };
    const uint64_t frame_at_load = frames();
    auto* scheduler = hold.scheduler();
    auto* audio = emulator->audio_system();
    std::thread([watch, frame_at_load, scheduler, frames, audio]() {
      uint64_t running_ms = 0;
      while (running_ms < 12000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (load_watch_id.load() != watch) return;      // a newer load has its own watch
        if (frames() != frame_at_load) {
          XELOGI("Game Vault: load state: the game draws again ({} ms after going on)", running_ms);
          // Sound: what the game hands over in the next seconds (about 187 frames a second when all is well).
          // If none come, the output slots are given back once (frames the game owed before the load are not
          // owed any more) and it is counted again.
          uint64_t before = 0, after = 0, calls_before = 0, calls_after = 0, in_callback_ms = 0;
          float peak = 0.0f;
          audio->GvSoundStats(1, &before, &peak);
          audio->GvWorkerStats(&calls_before, &in_callback_ms);
          std::this_thread::sleep_for(std::chrono::seconds(2));
          if (load_watch_id.load() != watch) return;
          audio->GvSoundStats(1, &after, &peak);
          audio->GvWorkerStats(&calls_after, &in_callback_ms);
          XELOGI("Game Vault: load state: sound after the load: {} frames and {} callbacks in 2 s, loudest {:.2f}{}",
                 after - before, calls_after - calls_before, peak,
                 in_callback_ms > 1000 ? fmt::format(" (the worker has been inside a callback for {} ms)",
                                                     in_callback_ms) : std::string());
          if (after == before) {
            audio->GvRefillSlots();
            before = after, calls_before = calls_after;
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (load_watch_id.load() != watch) return;
            audio->GvSoundStats(1, &after, &peak);
            audio->GvWorkerStats(&calls_after, &in_callback_ms);
            XELOGI("Game Vault: load state: no sound frames came, output slots given back: then {} frames and {} "
                   "callbacks in 3 s, loudest {:.2f}{}", after - before, calls_after - calls_before, peak,
                   after == before ? " (NO SOUND: the game's sound has stopped)" : "");
          }
          return;
        }
        if (!scheduler->paused()) running_ms += 250;    // held under the in-game menu: no picture is expected
      }
      XELOGE("Game Vault: load state: no new picture 12 s after the load: closing Xenia (a hung load must not hold the console)");
      xe::FlushLog();
      std::quick_exit(4);
    }).detach();
  }
  *message = "State loaded";
  XELOGI(
      "Game Vault: load state: LOADED {} threads ({} made again, {} newer ones ended), {} MB of "
      "memory, {} kernel objects made again, {} re-read, {} ms",
      thread_count, threads_made, ended, memory_bytes / (1024 * 1024), made_again, synced,
      Clock::QueryHostUptimeMillis() - started_ms);
  xe::FlushLog();
  return true;
}

}  // namespace xe
