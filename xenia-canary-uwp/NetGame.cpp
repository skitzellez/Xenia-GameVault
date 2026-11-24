// Copyright (c) 2026 Skitzelles - part of Xenia for Game Vault (github.com/skitzellez/xbox-game-vault).
// Licensed like the rest of Xenia (BSD, see LICENSE in the root).
//
// Xenia for Game Vault: an Xbox 360 game played straight from a PC's shared folder or a NAS (Game Vault's network
// games), with no copy on the console first. Apps on the Xbox cannot open \\PC\share paths (the console answers
// error 65), so this speaks SMB2/3 itself through libsmb2 (smb2.dll, LGPL-2.1, the same build Game Vault uses,
// loaded when a network game starts: Xenia runs without it). Like xemu's network discs (block/uwp-smb.c).
//
// Reads go through a cache of 1 MB pieces, so the game's many small reads do not cost a network round trip each;
// a dropped connection (a PC that slept, Wi-Fi that blinked) is opened again once by itself.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <fcntl.h>
#include <stdio.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <sstream>
#include <type_traits>
#include <vector>

#include "NetGame.h"
#include "xenia/base/gv_perf.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"

namespace NetGame {
namespace {

// libsmb2 (include/smb2/libsmb2.h), the few calls used; the library is loaded at run time
struct smb2_context;
struct smb2fh;
struct smb2dir;
struct smb2_stat_64 {   // as libsmb2 lays it out
  uint32_t smb2_type;
  uint32_t smb2_nlink;
  uint64_t smb2_ino;
  uint64_t smb2_size;
  uint64_t times[8];
};
struct smb2dirent {
  const char* name;
  smb2_stat_64 st;
};
constexpr uint32_t kTypeDirectory = 1;   // SMB2_TYPE_DIRECTORY
struct Smb2 {
  smb2_context* (*init_context)();
  void (*destroy_context)(smb2_context*);
  void (*set_timeout)(smb2_context*, int);
  void (*set_security_mode)(smb2_context*, uint16_t);
  void (*set_domain)(smb2_context*, const char*);
  void (*set_password)(smb2_context*, const char*);
  int (*connect_share)(smb2_context*, const char*, const char*, const char*);
  int (*disconnect_share)(smb2_context*);
  smb2fh* (*open)(smb2_context*, const char*, int);
  int (*close)(smb2_context*, smb2fh*);
  int (*pread)(smb2_context*, smb2fh*, uint8_t*, uint32_t, uint64_t);
  int64_t (*lseek)(smb2_context*, smb2fh*, int64_t, int, uint64_t*);
  uint32_t (*get_max_read_size)(smb2_context*);
  const char* (*get_error)(smb2_context*);
  // a game that is a folder of files (Games on Demand, unpacked)
  smb2dir* (*opendir)(smb2_context*, const char*);
  smb2dirent* (*readdir)(smb2_context*, smb2dir*);
  void (*closedir)(smb2_context*, smb2dir*);
  int (*stat)(smb2_context*, const char*, smb2_stat_64*);
};
constexpr uint16_t kSigningEnabled = 0x0001;   // SMB2_NEGOTIATE_SIGNING_ENABLED

const Smb2* LoadSmb2() {
  static Smb2 api;
  static bool tried = false, ok = false;
  if (tried) {
    return ok ? &api : nullptr;
  }
  tried = true;
  HMODULE m = LoadPackagedLibrary(L"smb2.dll", 0);
  if (!m) {
    XELOGE("network: smb2.dll could not be loaded ({})", GetLastError());
    return nullptr;
  }
  auto get = [&](auto& fn, const char* name) {
    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
    return fn != nullptr;
  };
  ok = get(api.init_context, "smb2_init_context") && get(api.destroy_context, "smb2_destroy_context") &&
       get(api.set_timeout, "smb2_set_timeout") && get(api.set_security_mode, "smb2_set_security_mode") &&
       get(api.set_domain, "smb2_set_domain") && get(api.set_password, "smb2_set_password") &&
       get(api.connect_share, "smb2_connect_share") && get(api.disconnect_share, "smb2_disconnect_share") &&
       get(api.open, "smb2_open") && get(api.close, "smb2_close") && get(api.pread, "smb2_pread") &&
       get(api.lseek, "smb2_lseek") && get(api.get_max_read_size, "smb2_get_max_read_size") &&
       get(api.get_error, "smb2_get_error") && get(api.opendir, "smb2_opendir") &&
       get(api.readdir, "smb2_readdir") && get(api.closedir, "smb2_closedir") && get(api.stat, "smb2_stat");
  if (!ok) {
    XELOGE("network: smb2.dll is missing a function");
  } else {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
  }
  return ok ? &api : nullptr;
}

std::wstring Wide(const std::string& utf8) {
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  std::wstring w(size_t(std::max(n, 0)), L'\0');
  if (n > 0) {
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), w.data(), n);
  }
  return w;
}

struct Cred {
  std::string server, share, user, password;
};

void Wipe(std::string& s) {
  std::fill(s.begin(), s.end(), ' ');
  s.clear();
}

// the sign-in for server\share from Game Vault's file; the file is gone from the disk at once (written over, deleted)
Cred ReadCred(const std::string& file, const std::string& server, const std::string& share) {
  Cred found{server, share, "Guest", ""};
  std::wstring wfile = Wide(file);
  if (file.empty()) {
    return found;
  }
  std::string text;
  {
    std::ifstream in(std::filesystem::path(wfile), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  }
  if (!text.empty()) {
    std::ofstream out(std::filesystem::path(wfile), std::ios::binary | std::ios::trunc);
    out << std::string(text.size(), ' ');
  }
  DeleteFileW(wfile.c_str());
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
      line.pop_back();
    }
    std::vector<std::string> f;
    size_t at = 0;
    for (int i = 0; i < 3; i++) {
      size_t tab = line.find('\t', at);
      if (tab == std::string::npos) {
        break;
      }
      f.push_back(line.substr(at, tab - at));
      at = tab + 1;
    }
    f.push_back(line.substr(at));
    if (f.size() == 4 && !_stricmp(f[0].c_str(), server.c_str()) && !_stricmp(f[1].c_str(), share.c_str())) {
      found.user = f[2].empty() ? "Guest" : f[2];
      found.password = f[3];
    }
    for (auto& s : f) {
      Wipe(s);
    }
    Wipe(line);
  }
  Wipe(text);
  return found;
}

class SmbGame : public xe::filesystem::NetFile {
 public:
  static constexpr uint64_t kChunk = 1 << 20;   // cache piece
  static constexpr size_t kSlots = 96;          // 96 MB of them
  // Game Vault 1.1.8.480: read-ahead. Once the game reads two pieces in a row, a second connection fetches the
  // next kAhead pieces in the background, so the next read is already here (Spec Ops from a NAS: each 1 MB piece
  // took ~100 ms and up to 800 ms of a second went to waiting for them).
  static constexpr uint64_t kAhead = 8;

  SmbGame(const Smb2* api, Cred cred, std::string path) : api_(api), cred_(std::move(cred)), path_(std::move(path)) {}
  ~SmbGame() override {
    {
      std::lock_guard<std::mutex> lock(lock_);
      stop_ = true;
    }
    cv_.notify_all();
    if (ahead_thread_.joinable()) {
      ahead_thread_.join();
    }
    Close(main_);
    Close(ahead_);
    Wipe(cred_.password);
  }

  bool Connect() {
    uint64_t end = 0;
    if (!Open(main_, &end)) {
      return false;
    }
    size_ = end;
    return true;
  }

  uint64_t size() const override { return size_; }

  bool Read(uint64_t offset, void* buffer, size_t length) override {
    if (offset > size_ || length > size_ - offset) {
      return false;
    }
    xe::gv_perf::Scope gv_scope(xe::gv_perf::net_read);
    std::unique_lock<std::mutex> lock(lock_);
    auto* out = static_cast<uint8_t*>(buffer);
    while (length) {
      uint64_t chunk = offset / kChunk;
      Slot* slot = Fetch(chunk, lock);
      if (!slot) {
        return false;
      }
      size_t in = size_t(offset - chunk * kChunk);
      size_t n = std::min<size_t>(length, slot->data.size() - in);
      if (!n) {
        return false;
      }
      std::memcpy(out, slot->data.data() + in, n);
      out += n;
      offset += n;
      length -= n;
    }
    return true;
  }

 private:
  struct Slot {
    int64_t chunk = -1;
    uint64_t used = 0;
    bool ahead = false;   // fetched by the read-ahead and not read by the game yet
    std::vector<uint8_t> data;
  };
  // one SMB connection with the game file open (libsmb2 contexts are not shared between threads)
  struct Conn {
    smb2_context* ctx = nullptr;
    smb2fh* fh = nullptr;
    uint32_t max_read = 64 * 1024;
  };

  bool Open(Conn& c, uint64_t* end) {
    Close(c);
    std::string user = cred_.user, domain;
    size_t slash = user.find('\\');
    if (slash != std::string::npos) {   // DOMAIN\user
      domain = user.substr(0, slash);
      user = user.substr(slash + 1);
    }
    c.ctx = api_->init_context();
    if (!c.ctx) {
      return false;
    }
    api_->set_timeout(c.ctx, 15);
    api_->set_security_mode(c.ctx, kSigningEnabled);
    if (!domain.empty()) {
      api_->set_domain(c.ctx, domain.c_str());
    }
    api_->set_password(c.ctx, cred_.password.c_str());
    if (api_->connect_share(c.ctx, cred_.server.c_str(), cred_.share.c_str(), user.c_str()) < 0) {
      XELOGE("network: {}\\{}: {}", cred_.server, cred_.share, api_->get_error(c.ctx));
      api_->destroy_context(c.ctx);
      c.ctx = nullptr;
      return false;
    }
    c.fh = api_->open(c.ctx, path_.c_str(), O_RDONLY);
    if (!c.fh) {
      XELOGE("network: {} could not be opened: {}", path_, api_->get_error(c.ctx));
      Close(c);
      return false;
    }
    if (api_->lseek(c.ctx, c.fh, 0, SEEK_END, end) < 0) {
      Close(c);
      return false;
    }
    c.max_read = std::max<uint32_t>(64 * 1024, std::min<uint32_t>(api_->get_max_read_size(c.ctx), kChunk));
    return true;
  }

  void Close(Conn& c) {
    if (c.ctx) {
      if (c.fh) {
        api_->close(c.ctx, c.fh);
      }
      api_->disconnect_share(c.ctx);
      api_->destroy_context(c.ctx);
    }
    c.ctx = nullptr;
    c.fh = nullptr;
  }

  bool ReadNet(Conn& c, uint64_t at, uint8_t* buf, size_t len) {
    size_t done = 0;
    while (done < len) {
      uint32_t n = uint32_t(std::min<size_t>(len - done, c.max_read));
      int got = c.ctx && c.fh ? api_->pread(c.ctx, c.fh, buf + done, n, at + done) : -1;
      if (got <= 0) {
        return false;
      }
      done += size_t(got);
    }
    return true;
  }

  Slot* Find(uint64_t chunk) {
    for (auto& s : slots_) {
      if (s.chunk == int64_t(chunk)) {
        return &s;
      }
    }
    return nullptr;
  }

  // the empty piece, or the one read longest ago
  Slot* Oldest() {
    Slot* pick = &slots_[0];
    for (auto& s : slots_) {
      if (s.used < pick->used) {
        pick = &s;
      }
    }
    return pick;
  }

  // the game moved to another piece: two in a row start (or move on) the read-ahead window
  void Note(uint64_t chunk) {
    if (int64_t(chunk) == last_) {
      return;
    }
    const bool in_a_row = last_ >= 0 && int64_t(chunk) == last_ + 1;
    last_ = int64_t(chunk);
    if (!in_a_row || ahead_failed_) {
      return;
    }
    const uint64_t pieces = (size_ + kChunk - 1) / kChunk;
    if (!pieces) {
      return;
    }
    if (ahead_from_ <= chunk || ahead_from_ > chunk + kAhead) {
      ahead_from_ = chunk + 1;
    }
    ahead_to_ = std::min<uint64_t>(chunk + kAhead, pieces - 1);
    if (!ahead_thread_.joinable()) {
      ahead_thread_ = std::thread([this]() { AheadLoop(); });
    }
    cv_.notify_all();
  }

  Slot* Fetch(uint64_t chunk, std::unique_lock<std::mutex>& lock) {
    for (;;) {
      if (Slot* s = Find(chunk)) {
        if (s->ahead) {
          s->ahead = false;
          ++ahead_hits_;   // fetched ahead: a network wait saved
          xe::gv_perf::net_ahead_used.fetch_add(1, std::memory_order_relaxed);
        }
        s->used = ++clock_;
        Note(chunk);
        return s;
      }
      if (inflight_ != int64_t(chunk)) {
        break;
      }
      ++ahead_waits_;
      xe::gv_perf::net_ahead_waits.fetch_add(1, std::memory_order_relaxed);
      cv_.wait(lock);   // the read-ahead is fetching this very piece: wait for it instead of reading it twice
    }
    Slot* pick = Oldest();
    uint64_t at = chunk * kChunk;
    size_t len = size_t(std::min<uint64_t>(kChunk, size_ - at));
    pick->chunk = -1;
    pick->ahead = false;
    pick->data.resize(len);
    if (!ReadNet(main_, at, pick->data.data(), len)) {
      // once more on a fresh connection: the PC slept, the Wi-Fi blinked
      XELOGW("network: read at {:X} failed ({}), connecting again", at,
             main_.ctx ? api_->get_error(main_.ctx) : "no connection");
      if (!Connect() || !ReadNet(main_, at, pick->data.data(), len)) {
        XELOGE("network: the game could not be read at {:X}", at);
        return nullptr;
      }
    }
    ++misses_;
    xe::gv_perf::net_demand.fetch_add(1, std::memory_order_relaxed);
    pick->chunk = int64_t(chunk);
    pick->used = ++clock_;
    Note(chunk);
    return pick;
  }

  // the read-ahead thread: its own connection, the lock dropped while it waits on the network
  void AheadLoop() {
    std::vector<uint8_t> buf;
    uint64_t end = 0, fetched = 0;
    auto last_log = GetTickCount64();
    std::unique_lock<std::mutex> lock(lock_);
    while (!stop_) {
      int64_t next = -1;
      while (ahead_from_ <= ahead_to_) {
        uint64_t c = ahead_from_++;
        if (!Find(c)) {
          next = int64_t(c);
          break;
        }
      }
      if (next < 0) {
        cv_.wait(lock);
        continue;
      }
      inflight_ = next;
      lock.unlock();
      const uint64_t at = uint64_t(next) * kChunk;
      const size_t len = size_t(std::min<uint64_t>(kChunk, size_ - at));
      buf.resize(len);
      bool ok = ahead_.ctx && ReadNet(ahead_, at, buf.data(), len);
      if (!ok) {
        ok = Open(ahead_, &end) && ReadNet(ahead_, at, buf.data(), len);   // first use, or a dropped connection
      }
      lock.lock();
      inflight_ = -1;
      if (!ok) {
        XELOGW("network: read-ahead at {:X} failed, off for this game (reads go on as before)", at);
        ahead_failed_ = true;
        cv_.notify_all();
        break;
      }
      ++fetched;
      xe::gv_perf::net_ahead.fetch_add(1, std::memory_order_relaxed);
      if (!Find(uint64_t(next))) {
        Slot* pick = Oldest();
        pick->data.swap(buf);
        pick->chunk = next;
        pick->ahead = true;
        pick->used = ++clock_;   // counts as just used, so the pieces the game is about to read are kept
      }
      cv_.notify_all();
      const auto now = GetTickCount64();
      if (now - last_log > 30000) {
        XELOGI("network read-ahead: {} pieces fetched ahead, {} used, {} waited on, {} read on demand", fetched,
               ahead_hits_, ahead_waits_, misses_);
        last_log = now;
      }
    }
  }

  const Smb2* api_;
  Cred cred_;
  std::string path_;
  Conn main_, ahead_;
  uint64_t size_ = 0;
  std::mutex lock_;
  std::condition_variable cv_;
  Slot slots_[kSlots];
  uint64_t clock_ = 0;
  int64_t last_ = -1, inflight_ = -1;
  uint64_t ahead_from_ = 1, ahead_to_ = 0;
  uint64_t ahead_hits_ = 0, ahead_waits_ = 0, misses_ = 0;
  bool stop_ = false, ahead_failed_ = false;
  std::thread ahead_thread_;
};

// A game that is a folder of files on the share (Games on Demand: <header> + <header>.data\Data0000...; unpacked:
// default.xex and the rest): one connection for all of them, each file with its own handle and a few cache pieces
class SmbFolder : public xe::filesystem::NetFolder, public std::enable_shared_from_this<SmbFolder> {
 public:
  SmbFolder(const Smb2* api, Cred cred, std::string base) : api_(api), cred_(std::move(cred)), base_(std::move(base)) {}
  ~SmbFolder() override {
    Drop();
    Wipe(cred_.password);
  }

  bool Connect() {
    std::lock_guard<std::mutex> lock(lock_);
    return ConnectLocked();
  }

  bool Stat(const std::string& rel, bool* is_dir, uint64_t* size) override {
    std::lock_guard<std::mutex> lock(lock_);
    smb2_stat_64 st{};
    std::string path = Full(rel);
    if (!ctx_ || api_->stat(ctx_, path.c_str(), &st) < 0) {
      // (a name that is not there fails too: once more on a fresh connection only when the connection is gone)
      if (ctx_ || !ConnectLocked() || api_->stat(ctx_, path.c_str(), &st) < 0) {
        if (stat_misses_++ < 50) {   // (a game looking for files that are not there: the first ones, for reports)
          XELOGI("network: {} is not in the game's folder ({})", path, ctx_ ? api_->get_error(ctx_) : "no connection");
        }
        return false;
      }
    }
    *is_dir = st.smb2_type == kTypeDirectory;
    *size = st.smb2_size;
    return true;
  }

  bool List(const std::string& rel, std::vector<xe::filesystem::NetFolderEntry>& out) override {
    std::lock_guard<std::mutex> lock(lock_);
    std::string path = Full(rel);
    smb2dir* dir = ctx_ ? api_->opendir(ctx_, path.c_str()) : nullptr;
    if (!dir && (!ConnectLocked() || !(dir = api_->opendir(ctx_, path.c_str())))) {
      XELOGE("network: {} could not be listed", path);
      return false;
    }
    while (smb2dirent* e = api_->readdir(ctx_, dir)) {
      if (!e->name || !strcmp(e->name, ".") || !strcmp(e->name, "..")) {
        continue;
      }
      out.push_back({e->name, e->st.smb2_type == kTypeDirectory, e->st.smb2_size});
    }
    api_->closedir(ctx_, dir);
    XELOGI("network: {} listed, {} entries", path, out.size());
    return true;
  }

  std::shared_ptr<xe::filesystem::NetFile> OpenFile(const std::string& rel) override;

  // Reads a file of the folder through *fh, opened (again) when it is not open on the current connection
  bool ReadAt(const std::string& path, smb2fh** fh, uint64_t* gen, uint64_t at, uint8_t* buf, size_t len) {
    std::lock_guard<std::mutex> lock(lock_);
    for (int attempt = 0; attempt < 2; attempt++) {
      if (attempt) {
        // once more on a fresh connection: the PC slept, the Wi-Fi blinked
        XELOGW("network: read of {} at {:X} failed, connecting again", path, at);
        if (!ConnectLocked()) {
          return false;
        }
      }
      if (!ctx_) {
        continue;
      }
      if (!*fh || *gen != generation_) {
        *fh = api_->open(ctx_, path.c_str(), O_RDONLY);
        *gen = generation_;
        if (!*fh) {
          XELOGE("network: {} could not be opened: {}", path, api_->get_error(ctx_));
          continue;
        }
      }
      size_t done = 0;
      while (done < len) {
        uint32_t n = uint32_t(std::min<size_t>(len - done, max_read_));
        int got = api_->pread(ctx_, *fh, buf + done, n, at + done);
        if (got <= 0) {
          break;
        }
        done += size_t(got);
      }
      if (done == len) {
        return true;
      }
    }
    return false;
  }

  void Close(smb2fh* fh, uint64_t gen) {
    std::lock_guard<std::mutex> lock(lock_);
    if (fh && ctx_ && gen == generation_) {
      api_->close(ctx_, fh);
    }
  }

  std::string Full(const std::string& rel) const { return rel.empty() ? base_ : base_ + "\\" + rel; }

 private:
  bool ConnectLocked() {
    Drop();
    std::string user = cred_.user, domain;
    size_t slash = user.find('\\');
    if (slash != std::string::npos) {   // DOMAIN\user
      domain = user.substr(0, slash);
      user = user.substr(slash + 1);
    }
    ctx_ = api_->init_context();
    if (!ctx_) {
      return false;
    }
    api_->set_timeout(ctx_, 15);
    api_->set_security_mode(ctx_, kSigningEnabled);
    if (!domain.empty()) {
      api_->set_domain(ctx_, domain.c_str());
    }
    api_->set_password(ctx_, cred_.password.c_str());
    if (api_->connect_share(ctx_, cred_.server.c_str(), cred_.share.c_str(), user.c_str()) < 0) {
      XELOGE("network: {}\\{}: {}", cred_.server, cred_.share, api_->get_error(ctx_));
      api_->destroy_context(ctx_);
      ctx_ = nullptr;
      return false;
    }
    max_read_ = std::max<uint32_t>(64 * 1024, std::min<uint32_t>(api_->get_max_read_size(ctx_), 1 << 20));
    generation_++;   // every file handle of the old connection is gone
    return true;
  }
  void Drop() {
    if (ctx_) {
      api_->disconnect_share(ctx_);
      api_->destroy_context(ctx_);
    }
    ctx_ = nullptr;
  }

  const Smb2* api_;
  Cred cred_;
  std::string base_;
  smb2_context* ctx_ = nullptr;
  uint32_t max_read_ = 64 * 1024;
  uint64_t generation_ = 0;
  int stat_misses_ = 0;
  std::mutex lock_;
};

// One file of a SmbFolder: its handle and 4 cache pieces of 1 MB (a Games on Demand game has ~40 files of 160 MB)
class SmbFolderFile : public xe::filesystem::NetFile {
 public:
  static constexpr uint64_t kChunk = 1 << 20;
  static constexpr size_t kSlots = 4;

  SmbFolderFile(std::shared_ptr<SmbFolder> folder, std::string path, uint64_t size)
      : folder_(std::move(folder)), path_(std::move(path)), size_(size) {}
  ~SmbFolderFile() override { folder_->Close(fh_, gen_); }

  uint64_t size() const override { return size_; }

  bool Read(uint64_t offset, void* buffer, size_t length) override {
    if (offset > size_ || length > size_ - offset) {
      return false;
    }
    xe::gv_perf::Scope gv_scope(xe::gv_perf::net_read);
    std::lock_guard<std::mutex> lock(lock_);
    auto* out = static_cast<uint8_t*>(buffer);
    while (length) {
      uint64_t chunk = offset / kChunk;
      Slot* slot = Fetch(chunk);
      if (!slot) {
        return false;
      }
      size_t in = size_t(offset - chunk * kChunk);
      size_t n = std::min<size_t>(length, slot->data.size() - in);
      if (!n) {
        return false;
      }
      std::memcpy(out, slot->data.data() + in, n);
      out += n;
      offset += n;
      length -= n;
    }
    return true;
  }

 private:
  struct Slot {
    int64_t chunk = -1;
    uint64_t used = 0;
    std::vector<uint8_t> data;
  };

  Slot* Fetch(uint64_t chunk) {
    Slot* pick = nullptr;
    for (auto& s : slots_) {
      if (s.chunk == int64_t(chunk)) {
        s.used = ++clock_;
        return &s;
      }
      if (!pick || s.used < pick->used) {
        pick = &s;   // the empty one, or the one read longest ago
      }
    }
    uint64_t at = chunk * kChunk;
    size_t len = size_t(std::min<uint64_t>(kChunk, size_ - at));
    pick->chunk = -1;
    pick->data.resize(len);
    if (!folder_->ReadAt(path_, &fh_, &gen_, at, pick->data.data(), len)) {
      XELOGE("network: {} could not be read at {:X}", path_, at);
      return nullptr;
    }
    pick->chunk = int64_t(chunk);
    pick->used = ++clock_;
    return pick;
  }

  std::shared_ptr<SmbFolder> folder_;
  std::string path_;
  uint64_t size_;
  smb2fh* fh_ = nullptr;
  uint64_t gen_ = 0;
  std::mutex lock_;
  Slot slots_[kSlots];
  uint64_t clock_ = 0;
};

std::shared_ptr<xe::filesystem::NetFile> SmbFolder::OpenFile(const std::string& rel) {
  bool is_dir = false;
  uint64_t size = 0;
  if (!Stat(rel, &is_dir, &size) || is_dir) {
    return nullptr;
  }
  XELOGI("network: {} opened, {} bytes", Full(rel), size);
  return std::make_shared<SmbFolderFile>(shared_from_this(), Full(rel), size);
}

// A name for a stand-in path: plain letters only (Xenia takes the path it starts as text in the console's code page)
std::string PlainName(std::string name) {
  for (auto& c : name) {
    if (static_cast<unsigned char>(c) >= 0x80) {
      c = '_';
    }
  }
  return name;
}

}  // namespace

std::string Open(const std::string& url, const std::string& cred_file) {
  // "smb://server/share/a/b.iso" -> server, share, "a\b.iso" (libsmb2 sends the path as it is: Windows separators)
  if (url.rfind("smb://", 0) != 0) {
    return "";
  }
  size_t a = url.find('/', 6);
  size_t b = a == std::string::npos ? a : url.find('/', a + 1);
  if (a == std::string::npos || a == 6 || b == std::string::npos || b == a + 1 || b + 1 >= url.size()) {
    XELOGE("network: not a network game: {}", url);
    return "";
  }
  std::string server = url.substr(6, a - 6), share = url.substr(a + 1, b - a - 1), path = url.substr(b + 1);
  std::replace(path.begin(), path.end(), '/', '\\');
  const Smb2* api = LoadSmb2();
  Cred cred = ReadCred(cred_file, server, share);
  if (!api) {
    Wipe(cred.password);
    return "";
  }
  // a game that is a folder of files: an unpacked game's default.xex, or a Games on Demand header (no extension).
  // Its whole folder is read over the network; the program keeps its place in it.
  size_t cut = path.find_last_of('\\');
  std::string program = path.substr(cut == std::string::npos ? 0 : cut + 1);
  // a package with no extension that is the whole game in one file (an Xbox Live Arcade game, content type
  // 0x000D0000, in "<title ID>\000D0000\"): read as one file like a disc image, not as a folder game (the
  // package loader cannot open a file in a folder game: "Path to XContent container does not exist", 2026-10-27)
  if (cut != std::string::npos && program.find('.') == std::string::npos) {
    auto one = std::make_shared<SmbGame>(api, cred, path);
    uint8_t head[0x348] = {};
    if (one->Connect() && one->size() >= sizeof(head) && one->Read(0, head, sizeof(head)) &&
        (!std::memcmp(head, "LIVE", 4) || !std::memcmp(head, "PIRS", 4) || !std::memcmp(head, "CON ", 4)) &&
        (uint32_t(head[0x344]) << 24 | uint32_t(head[0x345]) << 16 | uint32_t(head[0x346]) << 8 | head[0x347]) ==
            0x000D0000) {
      XELOGI("network: {}\\{}\\{} is an Xbox Live Arcade package, {} bytes", server, share, path, one->size());
      std::string stand_in = "N:\\Game Vault network\\" + PlainName(program);
      xe::filesystem::SetNetFile(std::filesystem::path(Wide(stand_in)), one);
      Wipe(cred.password);
      return stand_in;
    }
  }
  // (any .xex: a prototype / dev-kit build's programs have other names than default.xex, Call of Duty NX1)
  size_t dot = program.find_last_of('.');
  bool xex = dot != std::string::npos && !_stricmp(program.c_str() + dot, ".xex");
  if (cut != std::string::npos && (xex || program.find('.') == std::string::npos)) {
    std::string base = path.substr(0, cut);
    auto folder = std::make_shared<SmbFolder>(api, std::move(cred), base);
    DWORD start = GetTickCount();
    bool is_dir = false;
    uint64_t size = 0;
    if (!folder->Connect() || !folder->Stat(program, &is_dir, &size) || is_dir) {
      XELOGE("network: {}\\{}\\{} could not be opened", server, share, path);
      return "";
    }
    XELOGI("network: folder game {}\\{}\\{} opened in {} ms, program {} bytes", server, share, base,
           GetTickCount() - start, size);
    std::string root = "N:\\Game Vault network\\" + PlainName(base.substr(base.find_last_of('\\') + 1));
    xe::filesystem::SetNetFolder(std::filesystem::path(Wide(root)), folder);
    return root + "\\" + PlainName(program);
  }
  auto game = std::make_shared<SmbGame>(api, std::move(cred), path);
  DWORD start = GetTickCount();
  if (!game->Connect()) {
    return "";
  }
  XELOGI("network: {}\\{}\\{} opened in {} ms, {} bytes", server, share, path, GetTickCount() - start, game->size());
  // a stand-in path that names no real file: Xenia's file functions find the reader by it
  // (plain letters only: Xenia takes the path it starts as text in the console's code page)
  std::string name = path.substr(path.find_last_of('\\') + 1);
  for (auto& c : name) {
    if (static_cast<unsigned char>(c) >= 0x80) {
      c = '_';
    }
  }
  std::string stand_in = "N:\\Game Vault network\\" + name;
  xe::filesystem::SetNetFile(std::filesystem::path(Wide(stand_in)), game);
  return stand_in;
}

}  // namespace NetGame
