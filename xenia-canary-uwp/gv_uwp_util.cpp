// Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
// PolyForm Noncommercial License 1.0.0: not for sale, and this notice stays in every copy.
// The Xbox app's helpers that the Edge build uses (from UWPUtil.cpp, which also held the Blades front end's
// downloads and metadata): the game Game Vault sent, the app's folders, saves kept with the player's other saves.
#include "UWPUtil.h"
#include "guest_input_capture.h"

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/emulator.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "third_party/fmt/include/fmt/format.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>

using namespace winrt::Windows::Storage::Pickers;
namespace UWP {
std::string m_game_path;
std::mutex m_game_path_mutex;
int m_DPI = 96;
UIInputCaptureState m_ui_input_capture;

winrt::fire_and_forget PickGame(xe::Emulator* emu) {
  FileOpenPicker openPicker;
  openPicker.ViewMode(PickerViewMode::List);
  openPicker.SuggestedStartLocation(PickerLocationId::ComputerFolder);
  openPicker.FileTypeFilter().Append(L"*");

  auto file = co_await openPicker.PickSingleFileAsync();
  if (file) {
    std::string path = winrt::to_string(file.Path().data());

    emu->LaunchPath(std::filesystem::u8path(path));
  }
}

winrt::fire_and_forget PickFolderAsync(
    std::function<void(std::string)> callback) {
  FolderPicker openPicker;
  openPicker.ViewMode(PickerViewMode::List);
  openPicker.SuggestedStartLocation(PickerLocationId::ComputerFolder);
  openPicker.FileTypeFilter().Append(L"*");

  auto folder = co_await openPicker.PickSingleFolderAsync();
  std::string path = "";
  if (folder) {
    path = winrt::to_string(folder.Path().data());
  }

  callback(path);
}

winrt::fire_and_forget PickFilesAsync(
    std::function<void(std::vector<std::string>)> callback) {
  FileOpenPicker openPicker;
  openPicker.ViewMode(PickerViewMode::List);
  openPicker.SuggestedStartLocation(PickerLocationId::ComputerFolder);
  openPicker.FileTypeFilter().Append(L"*");

  auto folders = co_await openPicker.PickMultipleFilesAsync();
  std::vector<std::string> paths;
  if (folders) {
    for (auto folder : folders) {
      paths.push_back(winrt::to_string(folder.Path()));
    }
  }

  callback(paths);
}

winrt::fire_and_forget PickFileAsync(
    std::function<void(std::string)> callback) {
  FileOpenPicker openPicker;
  openPicker.ViewMode(PickerViewMode::List);
  openPicker.SuggestedStartLocation(PickerLocationId::ComputerFolder);
  openPicker.FileTypeFilter().Append(L"*");

  auto folder = co_await openPicker.PickSingleFileAsync();
  std::string path = "";
  if (folder) {
    path = winrt::to_string(folder.Path().data());
  }

  callback(path);
}

bool HasGamePath() {
  std::lock_guard lock(m_game_path_mutex);
  return !m_game_path.empty();
}

std::string ConsumeGamePath() {
  std::lock_guard lock(m_game_path_mutex);
  return std::exchange(m_game_path, {});
}


std::string GetLocalCache() {
  return winrt::to_string(winrt::Windows::Storage::ApplicationData::Current()
                              .LocalCacheFolder()
                              .Path());
}

std::string GetLocalState() {
  return winrt::to_string(
      winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
}

std::string GetPackageVersion() {
  const auto version =
      winrt::Windows::ApplicationModel::Package::Current().Id().Version();
  return fmt::format("{}.{}.{}.{}", version.Major, version.Minor,
                     version.Build, version.Revision);
}

std::string GetPublisherGames() {
  try {
    return winrt::to_string(winrt::Windows::Storage::ApplicationData::Current()
                                .GetPublisherCacheFolder(L"games")
                                .Path());
  } catch (...) {
    return "";
  }
}

// Game Vault: Xbox 360 saves kept outside Xenia (see UWPUtil.h). A profile folder (content\<16 hex digits>) holds
// the profile itself (FFFE07D1) and each game's saves (<title id>\00000001); downloadable content is left out.
static bool IsProfileFolder(const std::filesystem::path& p) {
  const std::string name = xe::path_to_utf8(p.filename());
  return name.size() == 16 && name != "0000000000000000" &&
         std::all_of(name.begin(), name.end(), [](char c) { return std::isxdigit(uint8_t(c)) != 0; });
}

static bool IsSaveFile(const std::filesystem::path& rel) {
  // <title id>\<content type>\...: the profile (FFFE07D1) whole, other titles only their saved games
  auto it = rel.begin();
  if (it == rel.end()) return false;
  const std::string title = xe::path_to_utf8(*it);
  if (++it == rel.end()) return false;
  return title == "FFFE07D1" || xe::path_to_utf8(*it) == "00000001";
}

// Folders down to `p`. std::filesystem::create_directories looks at every folder from the drive down and fails
// on the Xbox, where an app may not look at the folders above its own (Users\<user>\AppData\Local\Publishers):
// a folder it may not look at counts as there.
static bool MakeFolders(const std::filesystem::path& p) {
  const DWORD a = GetFileAttributesW(p.c_str());
  if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if (GetLastError() == ERROR_ACCESS_DENIED || p == p.parent_path()) return true;
  if (!MakeFolders(p.parent_path())) return false;
  return CreateDirectoryW(p.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// The first thing that went wrong in a pass, in the log (a backup that silently does nothing helps nobody)
static void SaveError(bool& told, const char* what, const std::filesystem::path& p, DWORD code) {
  if (told) return;
  told = true;
  XELOGW("Game Vault: saves: could not {} {} (error {})", what, xe::path_to_utf8(p), code);
}

// Copies the files of `from` that `to` lacks or has an older/different copy of; true when something was copied
static bool MirrorSaves(const std::filesystem::path& from, const std::filesystem::path& to, bool saves_only) {
  std::error_code ec;
  bool copied = false, told = false;
  for (auto it = std::filesystem::recursive_directory_iterator(from, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    // lexically: std::filesystem::relative looks at the folders above (not allowed on the Xbox, see MakeFolders)
    const auto rel = it->path().lexically_relative(from);
    if (rel.empty() || (saves_only && !IsSaveFile(rel))) continue;
    const uint64_t size = it->file_size(ec);
    if (ec || size > (256ull << 20)) continue;
    const auto dst = to / rel;
    std::error_code e2;
    if (std::filesystem::exists(dst, e2) && std::filesystem::file_size(dst, e2) == size &&
        std::filesystem::last_write_time(dst, e2) >= it->last_write_time(ec)) {
      continue;
    }
    if (!MakeFolders(dst.parent_path())) {
      SaveError(told, "make", dst.parent_path(), GetLastError());
      continue;
    }
    const HRESULT hr = CopyFile2(it->path().c_str(), dst.c_str(), nullptr);
    if (SUCCEEDED(hr)) {
      copied = true;
    } else {
      SaveError(told, "copy to", dst, DWORD(hr));
    }
  }
  if (ec) SaveError(told, "read", from, DWORD(ec.value()));
  return copied;
}

static void BackUpProfiles(const std::filesystem::path& content_root, const std::filesystem::path& shared,
                           bool first) {
  std::error_code ec;
  int profiles = 0, n = 0;
  for (auto& d : std::filesystem::directory_iterator(content_root, ec)) {
    if (d.is_directory(ec) && IsProfileFolder(d.path())) {
      profiles++;
      if (MirrorSaves(d.path(), shared / d.path().filename(), true)) n++;
    }
  }
  if (n || first) {
    XELOGI("Game Vault: saves: {} profile(s) here, {} with new or changed saves copied to the shared folder{}",
           profiles, n, ec ? " (could not list the content folder)" : "");
  }
}

void SavesAtStart(const std::filesystem::path& content_root) {
  const std::string games = GetPublisherGames();
  if (games.empty()) return;
  const auto shared = std::filesystem::path(xe::to_path(games)) / "xbox 360 saves";
  const auto local = std::filesystem::path(xe::to_path(GetLocalState()));
  const auto pending = local / "gv_start_pending.txt", restored = local / "gv_saves_restored.txt",
             no_restore = local / "gv_no_restore.txt";
  std::error_code ec;
  MakeFolders(content_root);
  XELOGI("Game Vault: saves are copied to {}", xe::path_to_utf8(shared));

  // Start guard: the file is there from start until 10 s of running (bad saved data stopped B1GFOOT524's Xenia at the
  // logo within a second). Three starts in a row that died that early right after saves came back from the backup:
  // the restored profiles go aside and are not brought back again, so a broken backup can never keep Xenia from
  // starting (a fresh install then makes a new profile). Three, not two: a start that the player or another
  // launch closes quickly looks the same.
  int early = 0;
  if (std::ifstream in(pending); in) in >> early;
  if (early >= 3 && std::filesystem::exists(restored, ec)) {
    const auto aside = content_root.parent_path() / "content set aside";
    for (auto& d : std::filesystem::directory_iterator(content_root, ec)) {
      if (d.is_directory(ec) && IsProfileFolder(d.path())) {
        MakeFolders(aside);
        std::filesystem::rename(d.path(), aside / d.path().filename(), ec);
      }
    }
    std::filesystem::remove(restored, ec);
    std::ofstream(no_restore) << "the restored saves stopped Xenia from starting 3 times\n";
    XELOGW("Game Vault: Xenia stopped early 3 times after the saves came back; they were moved to 'content set aside'");
    early = 0;
  }
  std::ofstream(pending) << (early + 1);

  // Fresh install: no profile here, but the shared folder has some -> they come back
  bool have = false;
  for (auto& d : std::filesystem::directory_iterator(content_root, ec)) {
    have |= d.is_directory(ec) && IsProfileFolder(d.path());
  }
  if (!have && !std::filesystem::exists(no_restore, ec)) {
    int n = 0;
    for (auto& d : std::filesystem::directory_iterator(shared, ec)) {
      if (d.is_directory(ec) && IsProfileFolder(d.path())) {
        MirrorSaves(d.path(), content_root / d.path().filename(), false);
        n++;
      }
    }
    if (n) {
      std::ofstream(restored) << n;
      XELOGI("Game Vault: {} profile(s) and their saves brought back from the shared folder", n);
    }
  }

  // While Xenia runs: after 10 s the start counts as good (the restored saves too: the guard is only for the starts
  // right after they came back); the saves are copied out at 30 s and every minute after
  std::thread([content_root, shared, pending, restored]() {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::error_code e;
    std::filesystem::remove(pending, e);
    std::filesystem::remove(restored, e);
    std::this_thread::sleep_for(std::chrono::seconds(20));
    for (bool first = true;; first = false) {
      BackUpProfiles(content_root, shared, first);
      std::this_thread::sleep_for(std::chrono::seconds(60));
    }
  }).detach();
}

int GetCoreDPI() { return m_DPI; }

void SetAutomaticLaunch(std::string game_path) {
  std::lock_guard lock(m_game_path_mutex);
  m_game_path = std::move(game_path);
}
void SetDPI(int DPI) { m_DPI = DPI; }
bool IsUIOpen() {
  return UIInputCaptureState::IsOpen(m_ui_input_capture.snapshot());
}
void SetUIOpen(bool is_open) { m_ui_input_capture.SetOpen(is_open); }
static std::atomic<bool> s_show_fps = false;
void SetShowFps(bool on) { s_show_fps = on; }
bool GetShowFps() { return s_show_fps; }
uint64_t GetUIInputCaptureState() { return m_ui_input_capture.snapshot(); }

winrt::fire_and_forget LaunchUriAsync(std::string url) {
  auto uri = winrt::Windows::Foundation::Uri(winrt::to_hstring(url));
  co_await winrt::Windows::System::Launcher::LaunchUriAsync(uri);
}

void LaunchUri(const std::string& url) { LaunchUriAsync(url); }
}  // namespace UWP
