// Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
// PolyForm Noncommercial License 1.0.0: not for sale, and this notice stays in every copy.
//
// Game Vault's Skylanders portal without the toy (see virtual_portal.h).

#include "xenia/hid/portal/virtual_portal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"

DEFINE_int32(portal_reply_header, -1,
             "Game Vault Skylanders portal: put 0B 14 in front of the portal's replies. -1 = when the game's "
             "commands have it, 0 = never, 1 = always.",
             "HID");

namespace xe {
namespace hid {

namespace {

constexpr uint8_t kHeader0 = 0x0B;
constexpr uint8_t kHeader1 = 0x14;
// The real portal reports its figures about every 8 ms; between reports a read finds nothing.
constexpr auto kStatusInterval = std::chrono::milliseconds(8);
constexpr uint32_t kLogLimit = 80;

uint16_t Crc16Ccitt(const uint8_t* data, size_t size) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < size; ++i) {
    crc ^= uint16_t(data[i]) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
  }
  return crc;
}

uint16_t ReadLe16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t ReadLe32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }
void WriteLe16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}
void WriteLe32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

std::string Hex(const uint8_t* p, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    static const char* kDigits = "0123456789ABCDEF";
    out += kDigits[p[i] >> 4];
    out += kDigits[p[i] & 15];
    if (i + 1 < n) out += ' ';
  }
  return out;
}

bool IsFigureFile(const std::filesystem::path& p) {
  std::string ext = xe::path_to_utf8(p.extension());
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (ext != ".sky" && ext != ".bin" && ext != ".dmp" && ext != ".dump") return false;
  std::error_code ec;
  return std::filesystem::is_regular_file(p, ec) && std::filesystem::file_size(p, ec) >= VirtualPortal::kFigureSize;
}

// Names become file names: keep letters, digits, spaces and a few marks.
std::string SafeFileName(const std::string& name) {
  std::string out;
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == ' ' || c == '-' || c == '_' || c == '\'' || c == '(' || c == ')' || c == '.') {
      out += c;
    }
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out.empty() ? std::string("Figure") : out;
}

}  // namespace

VirtualPortal::VirtualPortal() : Portal() {}
VirtualPortal::~VirtualPortal() {
  if (log_) std::fclose(log_);
}

void VirtualPortal::SetLogFile(const std::filesystem::path& file) {
  std::lock_guard<std::mutex> lock(log_mutex_);
  if (log_) std::fclose(log_);
  log_ = xe::filesystem::OpenFile(file, "wt");
}

void VirtualPortal::Note(const std::string& line) {
  XELOGI("Game Vault portal: {}", line);
  std::lock_guard<std::mutex> lock(log_mutex_);
  if (log_) {
    std::fprintf(log_, "%s\n", line.c_str());
    std::fflush(log_);
  }
}

void VirtualPortal::SetEnabled(bool enabled) {
  if (enabled_ != enabled) {
    Note(fmt::format("{} (figures folder {})", enabled ? "on" : "off", xe::path_to_utf8(folder_)));
  }
  enabled_ = enabled;
  if (!enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    replies_.clear();
    header_seen_ = false;
    commands_logged_ = 0;
    reads_logged_ = 0;
  }
}

void VirtualPortal::SetFigureFolder(const std::filesystem::path& folder) {
  folder_ = folder;
  std::error_code ec;
  std::filesystem::create_directories(folder_, ec);
}

std::vector<std::filesystem::path> VirtualPortal::ListFigureFiles() const {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  if (folder_.empty() || !std::filesystem::is_directory(folder_, ec)) return files;
  for (auto it = std::filesystem::directory_iterator(folder_, ec); !ec && it != std::filesystem::directory_iterator();
       it.increment(ec)) {
    if (IsFigureFile(it->path())) files.push_back(it->path());
  }
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
    std::string x = xe::path_to_utf8(a.filename()), y = xe::path_to_utf8(b.filename());
    std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::transform(y.begin(), y.end(), y.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return x < y;
  });
  return files;
}

int VirtualPortal::Place(const std::filesystem::path& file, std::string& error) {
  std::array<uint8_t, kFigureSize> data{};
  {
    std::ifstream in(file, std::ios::binary);
    if (!in || !in.read(reinterpret_cast<char*>(data.data()), data.size())) {
      error = "This figure file can't be read.";
      return -1;
    }
  }
  const uint32_t serial = ReadLe32(data.data());
  std::lock_guard<std::mutex> lock(mutex_);
  int slot = -1;
  for (int i = 0; i < kSlots; ++i) {
    if (figures_[i].status & 1) {
      if (figures_[i].file == file) {
        error = "This figure is already on the portal.";
        return -1;
      }
      continue;
    }
    if (figures_[i].last_serial == serial && serial) {
      slot = i;  // the same toy comes back to its old spot, as on the real portal
      break;
    }
    if (slot < 0) slot = i;
  }
  if (slot < 0) {
    error = "The portal is full. Take a figure off first.";
    return -1;
  }
  Figure& f = figures_[slot];
  f.file = file;
  f.data = data;
  f.status = 3;
  f.pending.clear();
  f.pending.push_back(3);  // "arrived" ...
  f.pending.push_back(1);  // ... then "on the portal"
  f.last_serial = serial;
  Note(fmt::format("{} placed in slot {} (character {}, variant {:04X})", xe::path_to_utf8(file.filename()),
         slot, ReadLe16(data.data() + 0x10), ReadLe16(data.data() + 0x1C)));
  return slot;
}

bool VirtualPortal::Remove(int slot) {
  if (slot < 0 || slot >= kSlots) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  Figure& f = figures_[slot];
  if (!(f.status & 1)) return false;
  Note(fmt::format("{} taken off slot {}", xe::path_to_utf8(f.file.filename()), slot));
  f.status = 2;
  f.pending.clear();
  f.pending.push_back(2);  // "left" ...
  f.pending.push_back(0);  // ... then empty
  f.file.clear();
  return true;
}

void VirtualPortal::RemoveAll() {
  for (int i = 0; i < kSlots; ++i) Remove(i);
}

VirtualPortal::SlotInfo VirtualPortal::GetSlot(int slot) {
  SlotInfo info;
  if (slot < 0 || slot >= kSlots) return info;
  std::lock_guard<std::mutex> lock(mutex_);
  const Figure& f = figures_[slot];
  if (!(f.status & 1) || f.file.empty()) return info;
  info.present = true;
  info.name = xe::path_to_utf8(f.file.stem());
  info.id = ReadLe16(f.data.data() + 0x10);
  info.variant = ReadLe16(f.data.data() + 0x1C);
  return info;
}

bool VirtualPortal::IsPlaced(const std::filesystem::path& file) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& f : figures_) {
    if ((f.status & 1) && f.file == file) return true;
  }
  return false;
}

std::filesystem::path VirtualPortal::CreateFigure(uint16_t id, uint16_t variant, const std::string& name,
                                                  std::string& error) {
  if (folder_.empty()) {
    error = "There is no figures folder.";
    return {};
  }
  std::error_code ec;
  std::filesystem::create_directories(folder_, ec);
  const std::string base = SafeFileName(name);
  std::filesystem::path file = folder_ / xe::to_path(base + ".sky");
  for (int n = 2; std::filesystem::exists(file, ec) && n < 1000; ++n) {
    file = folder_ / xe::to_path(fmt::format("{} ({}).sky", base, n));
  }
  std::array<uint8_t, kFigureSize> data{};
  // Each 4-block sector ends in its access block: sector 0 read-only, the rest the game may write.
  WriteLe32(data.data() + 0x36, 0x690F0F0F);
  for (size_t sector = 1; sector < kFigureSize / 0x40; ++sector) {
    WriteLe32(data.data() + sector * 0x40 + 0x36, 0x69080F7F);
  }
  // A serial of its own (the game tells toys apart by it), then character, variant and the header's checksum.
  const uint32_t serial =
      uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^ (uint32_t(id) << 16) ^ variant;
  WriteLe32(data.data(), serial ? serial : 1);
  data[4] = data[0] ^ data[1] ^ data[2] ^ data[3];  // the toy's check byte after its serial
  WriteLe16(data.data() + 0x10, id);
  WriteLe16(data.data() + 0x1C, variant);
  WriteLe16(data.data() + 0x1E, Crc16Ccitt(data.data(), 0x1E));
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out || !out.write(reinterpret_cast<const char*>(data.data()), data.size())) {
    error = "The figure file couldn't be written.";
    return {};
  }
  Note(fmt::format("new figure {} (character {}, variant {:04X})", xe::path_to_utf8(file.filename()), id,
         variant));
  return file;
}

bool VirtualPortal::ReplyHeader() const {
  if (cvars::portal_reply_header >= 0) return cvars::portal_reply_header != 0;
  return header_seen_;
}

void VirtualPortal::QueueReply(const uint8_t* bytes, size_t size) {
  Reply r{};
  const size_t at = ReplyHeader() ? 2 : 0;
  if (at) {
    r[0] = kHeader0;
    r[1] = kHeader1;
  }
  std::memcpy(r.data() + at, bytes, std::min(size, r.size() - at));
  replies_.push_back(r);
  if (replies_.size() > 32) replies_.pop_front();  // a game that never reads: don't grow forever
}

void VirtualPortal::QueueReply(std::initializer_list<uint8_t> bytes) { QueueReply(bytes.begin(), bytes.size()); }

void VirtualPortal::MakeStatus(uint8_t* out) {
  // 'S', 2 bits per slot (slot 0 lowest), a counter, and whether the portal is on.
  uint16_t bits = 0;
  for (int i = kSlots - 1; i >= 0; --i) {
    Figure& f = figures_[i];
    if (!f.pending.empty()) {
      f.status = f.pending.front();
      f.pending.pop_front();
    }
    bits = uint16_t((bits << 2) | (f.status & 3));
  }
  out[0] = 'S';
  WriteLe16(out + 1, bits);
  out[3] = 0;
  out[4] = 0;
  out[5] = counter_++;
  out[6] = active_ ? 1 : 0;
}

void VirtualPortal::SaveFigure(const Figure& f) {
  if (f.file.empty()) return;
  std::ofstream out(f.file, std::ios::binary | std::ios::in | std::ios::out);
  if (!out || !out.write(reinterpret_cast<const char*>(f.data.data()), f.data.size())) {
    Note(fmt::format("couldn't save {}", xe::path_to_utf8(f.file.filename())));
  }
}

void VirtualPortal::HandleCommand(const uint8_t* cmd, size_t size) {
  if (!size) return;
  auto arg = [&](size_t i) -> uint8_t { return i < size ? cmd[i] : 0; };
  switch (cmd[0]) {
    case 'A': {  // activate (1) or stop (0)
      QueueReply({'A', arg(1), 0xFF, 0x77});
      if (arg(1) && !active_) {
        // figures already on the portal arrive again
        for (auto& f : figures_) {
          if (f.status & 1) {
            f.pending.clear();
            f.pending.push_back(3);
            f.pending.push_back(1);
          }
        }
      }
      active_ = arg(1) != 0;
      break;
    }
    case 'R': {  // reset
      QueueReply({'R', 0x02, 0x18});
      for (auto& f : figures_) {
        if (!f.pending.empty()) f.status = f.pending.back();
        f.pending.clear();
        f.status &= 1;
      }
      active_ = false;
      break;
    }
    case 'Q': {  // read a block: Q <figure> <block>
      const int slot = arg(1) & 0x0F;
      const uint8_t block = arg(2);
      uint8_t r[19] = {'Q', uint8_t(slot), block};
      if (slot < kSlots && block < kFigureSize / 16 && (figures_[slot].status & 1)) {
        r[1] = uint8_t(0x10 | slot);
        std::memcpy(r + 3, figures_[slot].data.data() + block * 16, 16);
      }
      QueueReply(r, sizeof(r));
      break;
    }
    case 'W': {  // write a block: W <figure> <block> <16 bytes>
      const int slot = arg(1) & 0x0F;
      const uint8_t block = arg(2);
      uint8_t r[3] = {'W', uint8_t(slot), block};
      if (slot < kSlots && block < kFigureSize / 16 && (figures_[slot].status & 1) && size >= 19) {
        r[1] = uint8_t(0x10 | slot);
        std::memcpy(figures_[slot].data.data() + block * 16, cmd + 3, 16);
        SaveFigure(figures_[slot]);
      }
      QueueReply(r, sizeof(r));
      break;
    }
    case 'J':  // the game's light/sync request: acknowledged
      QueueReply({'J'});
      break;
    case 'M':  // portal speaker firmware: 0 = none, so the game plays sound on the TV
      QueueReply({'M', arg(1), 0x00, 0x19});
      break;
    case 'C':  // portal colour
    case 'L':  // Trap Team side lights
    case 'S':  // status request: the next read brings it
    case 'V':
      break;
    default:
      Note(fmt::format("unknown command {:02X}", cmd[0]));
      break;
  }
}

X_STATUS VirtualPortal::WriteInternal(std::span<uint8_t> data) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint8_t* cmd = data.data();
  size_t size = data.size();
  if (size >= 3 && cmd[0] == kHeader0 && cmd[1] == kHeader1) {
    header_seen_ = true;
    cmd += 2;
    size -= 2;
  }
  // (the portal colour fades come many times a second: left out so the log keeps room for reads and writes)
  if (commands_logged_ < kLogLimit && !(size && cmd[0] == 'C')) {
    ++commands_logged_;
    Note(fmt::format("game sent {}", Hex(data.data(), std::min<size_t>(data.size(), 24))));
  }
  HandleCommand(cmd, size);
  return X_ERROR_SUCCESS;
}

X_STATUS VirtualPortal::ReadInternal(std::span<uint8_t> data, int32_t& read_count) {
  std::lock_guard<std::mutex> lock(mutex_);
  Reply r{};
  bool status = false;
  if (!replies_.empty()) {
    r = replies_.front();
    replies_.pop_front();
  } else {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_status_ < kStatusInterval) {
      read_count = 0;  // nothing new yet, like the real portal between reports
      return X_ERROR_SUCCESS;
    }
    last_status_ = now;
    const size_t at = ReplyHeader() ? 2 : 0;
    if (at) {
      r[0] = kHeader0;
      r[1] = kHeader1;
    }
    MakeStatus(r.data() + at);
    status = true;
  }
  const size_t n = std::min(data.size(), r.size());
  std::memcpy(data.data(), r.data(), n);
  read_count = int32_t(n);
  // Log the replies and the first status reports (and any status that shows a figure) for the first tests.
  const bool figure_news = status && (r[ReplyHeader() ? 3 : 1] || r[ReplyHeader() ? 4 : 2]);
  if (reads_logged_ < kLogLimit && (!status || figure_news || reads_logged_ < 8)) {
    ++reads_logged_;
    Note(fmt::format("reply {}", Hex(r.data(), std::min<size_t>(n, 24))));
  }
  return X_ERROR_SUCCESS;
}

}  // namespace hid
}  // namespace xe
