// Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
// PolyForm Noncommercial License 1.0.0: not for sale, and this notice stays in every copy.
//
// Game Vault: Xbox 360 shader sharing with cdn.gamevault.fyi.
// At a game's start the console takes the shader records other players already shared for
// that game (guest shaders .xsh + draw setups .xpso: nothing of the player or the machine),
// so a first start builds its pipelines in the background like a later start, not in the
// middle of play. While the game runs, the records this console added are sent back (every
// 10 minutes when the files grew). The server checks each record's own hash and only shares
// a record once two consoles sent it. Every step has a short time limit; no network = the
// game starts as before.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/xxhash.h"
#include "xenia/config.h"
#include "xenia/gpu/shader_storage.h"

#if __has_include("gv_share_key.h")
#include "gv_share_key.h"   // (not in git: kGvShareKey, the upload key)
#else
static constexpr const char* kGvShareKey = "";
#endif

DEFINE_bool(gv_shader_share, true,
            "Game Vault: share Xbox 360 shader records with other players "
            "(take theirs at a game's start, send this console's new ones).",
            "GPU");
DEFINE_string(gv_shader_share_url, "https://cdn.gamevault.fyi",
              "Game Vault: shader sharing server.", "GPU");

namespace {

using Bytes = std::vector<uint8_t>;
std::atomic<uint32_t> g_session{0};   // a new game start ends the last game's sender

std::string Hex(const uint8_t* p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; ++i) s += fmt::format("{:02X}", p[i]);
  return s;
}

bool ReadFile(const std::filesystem::path& path, Bytes& out) {
  out.clear();
  FILE* f = xe::filesystem::OpenFile(path, "rb");
  if (!f) return false;
  uint8_t buf[64 * 1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  fclose(f);
  return true;
}

uint64_t FileSize(const std::filesystem::path& path) {
  std::error_code ec;
  uint64_t s = std::filesystem::file_size(path, ec);
  return ec ? 0 : s;
}

// Runs one HTTP call on its own thread and gives up after `limit` (the network may hang).
// GET when `body` is null. Returns true on HTTP 200 with the reply in `reply`.
bool Http(const std::string& url, const Bytes* body, std::chrono::milliseconds limit,
          Bytes& reply) {
  struct Shared {
    std::mutex m;
    std::condition_variable cv;
    bool done = false, ok = false;
    Bytes data;
  };
  auto sh = std::make_shared<Shared>();
  Bytes send = body ? *body : Bytes();
  bool post = body != nullptr;
  std::thread([sh, url, send = std::move(send), post, limit]() {
    bool ok = false;
    Bytes data;
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    try {
      winrt::Windows::Web::Http::Filters::HttpBaseProtocolFilter filter;
      filter.CacheControl().ReadBehavior(
          winrt::Windows::Web::Http::Filters::HttpCacheReadBehavior::NoCache);
      winrt::Windows::Web::Http::HttpClient client(filter);
      client.DefaultRequestHeaders().Append(L"User-Agent", L"GameVault-Xenia/1");
      winrt::Windows::Foundation::Uri uri(winrt::to_hstring(url));
      winrt::Windows::Foundation::IAsyncOperationWithProgress<
          winrt::Windows::Web::Http::HttpResponseMessage,
          winrt::Windows::Web::Http::HttpProgress>
          op{nullptr};
      if (post) {
        winrt::Windows::Storage::Streams::DataWriter writer;
        writer.WriteBytes(winrt::array_view<const uint8_t>(send.data(),
                                                           send.data() + send.size()));
        winrt::Windows::Web::Http::HttpBufferContent content(writer.DetachBuffer());
        content.Headers().ContentType(
            winrt::Windows::Web::Http::Headers::HttpMediaTypeHeaderValue(
                L"application/octet-stream"));
        winrt::Windows::Web::Http::HttpRequestMessage req(
            winrt::Windows::Web::Http::HttpMethod::Post(), uri);
        req.Content(content);
        req.Headers().Append(L"X-GV-Key", winrt::to_hstring(kGvShareKey));
        op = client.SendRequestAsync(req);
      } else {
        op = client.GetAsync(uri);
      }
      if (op.wait_for(limit) != winrt::Windows::Foundation::AsyncStatus::Completed) {
        op.Cancel();
      } else {
        auto response = op.GetResults();
        if (response.StatusCode() == winrt::Windows::Web::Http::HttpStatusCode::Ok) {
          auto rd = response.Content().ReadAsBufferAsync();
          if (rd.wait_for(limit) == winrt::Windows::Foundation::AsyncStatus::Completed) {
            auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(rd.GetResults());
            data.resize(reader.UnconsumedBufferLength());
            reader.ReadBytes(winrt::array_view<uint8_t>(data.data(), data.data() + data.size()));
            ok = true;
          } else {
            rd.Cancel();
          }
        }
      }
    } catch (...) {
      ok = false;
    }
    std::lock_guard<std::mutex> lock(sh->m);
    sh->ok = ok;
    sh->data = std::move(data);
    sh->done = true;
    sh->cv.notify_all();
  }).detach();
  std::unique_lock<std::mutex> lock(sh->m);
  if (!sh->cv.wait_for(lock, limit + std::chrono::seconds(1), [&] { return sh->done; }))
    return false;
  reply = std::move(sh->data);
  return sh->ok;
}

// Splits a storage file into (hash, record) after its header; stops at the first record
// whose own hash does not match (like Xenia's own reading).
template <typename F>
void EachXsh(const Bytes& f, F&& fn) {
  size_t pos = 8;
  while (pos + 12 <= f.size()) {
    uint64_t hash;
    uint32_t bits;
    std::memcpy(&hash, &f[pos], 8);
    std::memcpy(&bits, &f[pos + 8], 4);
    size_t end = pos + 12 + size_t(bits & 0x7FFFFFFF) * 4;
    if (end > f.size() || XXH3_64bits(&f[pos + 12], end - pos - 12) != hash) break;
    fn(hash, pos, end);
    pos = end;
  }
}
template <typename F>
void EachXpso(const Bytes& f, size_t rec, F&& fn) {
  for (size_t pos = 12; rec > 8 && pos + rec <= f.size(); pos += rec) {
    uint64_t hash;
    std::memcpy(&hash, &f[pos], 8);
    if (XXH3_64bits(&f[pos + 8], rec - 8) != hash) break;
    fn(hash, pos, pos + rec);
  }
}

// Adds the shared records this console does not have yet. Returns records added.
size_t Merge(const std::filesystem::path& path, const uint8_t* header, size_t header_size,
             const Bytes& shared, bool xsh, size_t rec) {
  if (shared.size() <= header_size || std::memcmp(shared.data(), header, header_size)) return 0;
  Bytes local;
  ReadFile(path, local);
  bool fresh = local.size() < header_size || std::memcmp(local.data(), header, header_size);
  std::unordered_set<uint64_t> have;
  size_t valid_end = header_size;
  if (!fresh) {
    auto add = [&](uint64_t h, size_t, size_t end) { have.insert(h); valid_end = end; };
    if (xsh) EachXsh(local, add); else EachXpso(local, rec, add);
  }
  Bytes out;
  auto take = [&](uint64_t h, size_t b, size_t e) {
    if (have.insert(h).second) out.insert(out.end(), shared.begin() + b, shared.begin() + e);
  };
  size_t added_before = out.size();
  if (xsh) EachXsh(shared, take); else EachXpso(shared, rec, take);
  if (out.size() == added_before) return 0;
  size_t count = 0;
  if (xsh) {
    Bytes tmp(header, header + header_size);
    tmp.insert(tmp.end(), out.begin(), out.end());
    EachXsh(tmp, [&](uint64_t, size_t, size_t) { ++count; });
  } else {
    count = out.size() / rec;
  }
  // Keep this console's records (up to the last good one), then the shared ones.
  Bytes result;
  if (fresh) {
    result.assign(header, header + header_size);
  } else {
    result.assign(local.begin(), local.begin() + valid_end);
  }
  result.insert(result.end(), out.begin(), out.end());
  std::filesystem::path tmp_path = path;
  tmp_path += ".gvshare.tmp";
  {
    FILE* f = xe::filesystem::OpenFile(tmp_path, "wb");
    if (!f) return 0;
    bool ok = fwrite(result.data(), 1, result.size(), f) == result.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) return 0;
  }
  std::error_code ec;
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::filesystem::remove(tmp_path, ec);
    return 0;
  }
  return count;
}

// "<sent xsh size> <sent xpso size>": what this console last sent (or took), so only
// growth from play is sent.
std::filesystem::path MarkPath(const xe::gpu::ShaderShareFiles& f) {
  return f.xsh.parent_path() / fmt::format("{:08X}.gvshare", f.title_id);
}
// The title's used-list (pipelines it drew with, in first-use order): <cache_root>/<TID>.gvhot.
std::filesystem::path HotPath(const xe::gpu::ShaderShareFiles& f) {
  return f.xsh.parent_path().parent_path().parent_path() /
         fmt::format("{:08X}.gvhot", f.title_id);
}
std::atomic<uint64_t> g_mark_hot{0};   // third number of the mark: used-list size sent/taken
void ReadMark(const xe::gpu::ShaderShareFiles& f, uint64_t& xsh, uint64_t& xpso) {
  xsh = xpso = 0;
  uint64_t hot = 0;
  std::ifstream in(MarkPath(f));
  in >> xsh >> xpso >> hot;
  g_mark_hot = hot;
}
void WriteMark(const xe::gpu::ShaderShareFiles& f, uint64_t xsh, uint64_t xpso) {
  std::ofstream out(MarkPath(f), std::ios::trunc);
  out << xsh << " " << xpso << " " << g_mark_hot.load() << "\n";
}

void SendGrown(const xe::gpu::ShaderShareFiles& f, const std::string& base) {
  if (!kGvShareKey[0]) return;
  uint64_t sent_xsh, sent_xpso;
  ReadMark(f, sent_xsh, sent_xpso);
  uint64_t now_xsh = FileSize(f.xsh), now_xpso = FileSize(f.xpso);
  bool any = false;
  for (int i = 0; i < 2; ++i) {
    bool xsh = i == 0;
    uint64_t now = xsh ? now_xsh : now_xpso, sent = xsh ? sent_xsh : sent_xpso;
    if (now <= sent || now <= (xsh ? 8u : 12u)) continue;
    Bytes file, reply;
    if (!ReadFile(xsh ? f.xsh : f.xpso, file)) continue;
    std::string url = fmt::format("{}/api/x360/v1/upload?tid={:08X}&kind={}", base,
                                  f.title_id, xsh ? "xsh" : "xpso");
    if (Http(url, &file, std::chrono::seconds(20), reply)) {
      any = true;
      if (xsh) sent_xsh = now; else sent_xpso = now;
      XELOGI("Game Vault: shader sharing: sent {} ({} KB): {}", xsh ? "shaders" : "draw setups",
             file.size() >> 10, std::string(reply.begin(), reply.end()));
    } else {
      XELOGW("Game Vault: shader sharing: sending {} did not go through (tried again later)",
             xsh ? "shaders" : "draw setups");
    }
  }
  // The used-list: the server ranks every console's lists into one shared list per game.
  uint64_t now_hot = FileSize(HotPath(f));
  if (now_hot > g_mark_hot.load()) {
    Bytes file, reply;
    if (ReadFile(HotPath(f), file)) {
      std::string url = fmt::format("{}/api/x360/v1/upload?tid={:08X}&kind=hot&fmt={}", base,
                                    f.title_id, Hex(f.xpso_header.data(), f.xpso_header.size()));
      if (Http(url, &file, std::chrono::seconds(20), reply)) {
        any = true;
        g_mark_hot = now_hot;
        XELOGI("Game Vault: shader sharing: sent the used-list ({} KB): {}", file.size() >> 10,
               std::string(reply.begin(), reply.end()));
      }
    }
  }
  if (any) WriteMark(f, sent_xsh, sent_xpso);
}

// The shared used-list, only for a game this console has no list for yet (its first start), so
// "Getting Graphics Ready" builds first what players really use. A console's own list is never
// replaced. Returns the lines taken.
size_t TakeHotList(const xe::gpu::ShaderShareFiles& f, const std::string& base) {
  if (FileSize(HotPath(f)) != 0) return 0;
  std::string url = fmt::format("{}/x360/shaders/{}/{:08X}.gvhot", base,
                                Hex(f.xpso_header.data(), f.xpso_header.size()), f.title_id);
  Bytes shared;
  if (!Http(url, nullptr, std::chrono::seconds(3), shared) || shared.empty()) return 0;
  std::string text(shared.begin(), shared.end()), clean;
  size_t lines = 0, start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() == 16 &&
        line.find_first_not_of("0123456789ABCDEFabcdef") == std::string::npos) {
      clean += line + "\n";
      ++lines;
    }
    start = end + 1;
  }
  if (!lines) return 0;
  std::ofstream(HotPath(f), std::ios::trunc | std::ios::binary) << clean;
  return lines;
}

void OnTitleStorage(const xe::gpu::ShaderShareFiles& f) {
  uint32_t session = ++g_session;
  if (!cvars::gv_shader_share) return;
  std::string base = cvars::gv_shader_share_url;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (base.empty()) return;

  // 1) Take the shared records (a short wait: a first start is worth a few seconds; no
  //    network fails at once).
  uint64_t mark_xsh, mark_xpso;
  ReadMark(f, mark_xsh, mark_xpso);
  const uint64_t before[2] = {FileSize(f.xsh), FileSize(f.xpso)};
  auto t0 = std::chrono::steady_clock::now();
  size_t added[2] = {0, 0};
  for (int i = 0; i < 2; ++i) {
    bool xsh = i == 0;
    const uint8_t* header = xsh ? f.xsh_header.data() : f.xpso_header.data();
    size_t header_size = xsh ? f.xsh_header.size() : f.xpso_header.size();
    std::string url = fmt::format("{}/x360/shaders/{}/{:08X}{}", base, Hex(header, header_size),
                                  f.title_id, xsh ? ".xsh" : ".rtv.d3d12.xpso");
    Bytes shared;
    if (!Http(url, nullptr, std::chrono::seconds(3), shared)) {
      XELOGI("Game Vault: shader sharing: no shared {} for {:08X} (not shared yet, or no network)",
             xsh ? "shaders" : "draw setups", f.title_id);
      continue;
    }
    added[i] = Merge(xsh ? f.xsh : f.xpso, header, header_size, shared, xsh, f.xpso_record_size);
  }
  // Records just taken came from the server: they don't count as this console's to send
  // (when everything of its own was already sent).
  if (before[0] <= mark_xsh) mark_xsh = FileSize(f.xsh);
  if (before[1] <= mark_xpso) mark_xpso = FileSize(f.xpso);
  size_t hot_lines = TakeHotList(f, base);
  if (hot_lines) g_mark_hot = FileSize(HotPath(f));   // (taken, not this console's to send)
  if (added[0] || added[1] || hot_lines) WriteMark(f, mark_xsh, mark_xpso);
  XELOGI("Game Vault: shader sharing: {:08X} took {} shaders + {} draw setups + a used-list of {} "
         "from other players ({} ms)",
         f.title_id, added[0], added[1], hot_lines,
         std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               t0)
             .count());

  // 2) In the background: send what this console added last time (at once), then what this
  //    game adds now (after 4 minutes, then every 10).
  if (!kGvShareKey[0]) return;
  xe::gpu::ShaderShareFiles copy = f;
  std::thread([copy, base, session]() {
    auto wait = std::chrono::seconds(20);
    for (;;) {
      auto until = std::chrono::steady_clock::now() + wait;
      while (std::chrono::steady_clock::now() < until) {
        if (g_session.load() != session) return;
        std::this_thread::sleep_for(std::chrono::seconds(5));
      }
      if (g_session.load() != session) return;
      SendGrown(copy, base);
      wait = wait < std::chrono::minutes(4) ? std::chrono::minutes(4) : std::chrono::minutes(10);
    }
  }).detach();
}

}  // namespace

DEFINE_bool(gv_fixes, true,
            "Game Vault: take per-game fixes (settings and patches) from Game Vault's server. Off: only the fixes "
            "built into this Xenia.",
            "Game Vault");

void GvShaderShareInstall() {
  xe::gpu::ShaderShareHook() = OnTitleStorage;
  // per-game fixes (config.h): one small file for every game, 3 s at most, only every 6 hours
  config::gamevault_fixes_fetch = []() -> std::string {
    if (!cvars::gv_fixes) return {};
    std::string base = cvars::gv_shader_share_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    Bytes reply;
    if (!Http(base + "/fixes/x360/fixes.toml", nullptr, std::chrono::seconds(3), reply)) return {};
    return std::string(reply.begin(), reply.end());
  };
}
