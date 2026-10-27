#include "UWPUtil.h"
#include "content_execution_info.h"
#include "download_http.h"
#include "patch_archive.h"
#include "guest_input_capture.h"
#include "title_update_catalogue.h"

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/config.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/emulator.h"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/kernel/xam/xcontent/xcontent_package_container.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <utility>

#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <chrono>
#include <thread>
#include <unordered_map>

#include "third_party/fmt/include/fmt/format.h"

#include <agents.h>
#include <ppl.h>
#include <ppltasks.h>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/windows.graphics.display.core.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>

#include "third_party/zarchive/include/zarchive/zarchivereader.h"

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

bool EnsureProfile(xe::Emulator* emu) {
  if (!emu || !emu->kernel_state() || !emu->kernel_state()->xam_state()) {
    return false;
  }
  auto* profiles = emu->kernel_state()->xam_state()->profile_manager();
  if (!profiles) {
    return false;
  }
  if (profiles->GetAccountCount() > 0) {
    // a profile from before, but nobody signed in (no logged_profile_slot_0_xuid in the config): games then say
    // "You are not signed in" and cannot save (GTA IV on the owner console). The first one is signed in.
    if (!profiles->IsAnyProfileSignedIn()) {
      uint64_t xuid = profiles->GetAccounts()->begin()->first;
      profiles->Login(xuid, 0);
      XELOGI("Game Vault: signed in profile {:016X}", xuid);
    }
    return true;
  }
  bool made = profiles->CreateProfile("Player", true, false);
  XELOGI("Game Vault: no profile yet, made \"Player\": {}", made ? "yes" : "no");
  return made;
}

void SelectGameFromWinRT(xe::Emulator* emu) {
  auto game_path = ConsumeGamePath();
  if (game_path.empty()) {
    PickGame(emu);
  } else {
    EnsureProfile(emu);
    emu->LaunchPath(std::filesystem::u8path(game_path));
  }
}

void SelectFolder(std::function<void(std::string)> callback) {
  PickFolderAsync(callback);
}

winrt::fire_and_forget PickStorageFolderAsync(
    std::wstring access_token,
    std::function<void(std::string, std::string)> callback) {
  static std::atomic_bool picker_open{false};
  if (picker_open.exchange(true)) {
    callback({}, "A folder picker is already open.");
    co_return;
  }
  std::string path;
  std::string error;
  try {
    co_await winrt::resume_foreground(
        winrt::Windows::ApplicationModel::Core::CoreApplication::MainView()
            .CoreWindow().Dispatcher());
    FolderPicker picker;
    picker.ViewMode(PickerViewMode::List);
    picker.SuggestedStartLocation(PickerLocationId::ComputerFolder);
    picker.FileTypeFilter().Append(L"*");
    if (auto folder = co_await picker.PickSingleFolderAsync()) {
      // Keep grants for every additional directory rather than replacing the
      // previous additional directory's token when another folder is added.
      if (access_token == L"xenia-additional-game-folder") {
        access_token += L"-" + std::to_wstring(
            std::hash<std::string>{}(winrt::to_string(folder.Path())));
      }
      winrt::Windows::Storage::AccessCache::StorageApplicationPermissions::
          FutureAccessList().AddOrReplace(access_token, folder);
      path = winrt::to_string(folder.Path());
    }
  } catch (const winrt::hresult_canceled&) {
    // Cancelling the picker leaves the existing storage selection untouched.
  } catch (const winrt::hresult_error& e) {
    error = winrt::to_string(e.message());
    XELOGW("Storage folder picker failed: {}", error);
  } catch (const std::exception& e) {
    error = e.what();
    XELOGW("Storage folder picker failed: {}", error);
  }
  picker_open.store(false);
  callback(std::move(path), std::move(error));
}

void SelectStorageFolder(
    const std::wstring& access_token,
    std::function<void(std::string, std::string)> callback) {
  PickStorageFolderAsync(access_token, std::move(callback));
}

void SelectFile(std::function<void(std::string)> callback) {
  PickFileAsync(callback);
}

void SelectFiles(std::function<void(std::vector<std::string>)> callback) {
  PickFilesAsync(callback);
}

bool TestPathPermissions(std::string path) {
  static std::atomic_uint64_t next_probe{0};
  CREATEFILE2_EXTENDED_PARAMETERS parameters{};
  parameters.dwSize = sizeof(parameters);
  parameters.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
  parameters.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE;
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    const auto probe_path =
        std::filesystem::u8path(path) / fmt::format(".xenia-write-test-{}-{}",
                                                    GetCurrentProcessId(),
                                                    next_probe.fetch_add(1));
    HANDLE probe = CreateFile2(probe_path.c_str(), GENERIC_WRITE, 0, CREATE_NEW,
                               &parameters);
    if (probe != INVALID_HANDLE_VALUE) {
      CloseHandle(probe);
      return true;
    }
    if (GetLastError() != ERROR_FILE_EXISTS &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
      return false;
    }
  }
  return false;
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

static std::atomic<bool> s_download_in_progress = false;
static std::atomic<float> s_download_progress = 0.0f;
static std::unordered_map<std::string, UWP::UnityGameMetadata>
    s_unity_metadata_cache;
static std::mutex s_unity_metadata_mutex;

bool IsDownloadInProgress() { return s_download_in_progress; }
float GetDownloadProgress() { return s_download_progress; }

winrt::fire_and_forget DownloadAndExtractZipAsync(
    std::string url, std::string dest_folder,
    std::function<void(bool, std::string)> callback) {
  winrt::apartment_context caller;
  if (s_download_in_progress.exchange(true)) {
    callback(false, "Another download is already in progress.");
    co_return;
  }
  s_download_progress = 0.0f;
  size_t count = 0;
  std::string error;
  try {
    co_await winrt::resume_background();
    PatchArchiveWorkspace download(std::filesystem::u8path(GetLocalCache()));
    const auto archive = download.path() / "patches.zip";
    winrt::Windows::Web::Http::HttpClient client;
    client.DefaultRequestHeaders().Append(L"User-Agent", L"xenia-uwp/1.0");
    auto response = co_await client.GetAsync(
        winrt::Windows::Foundation::Uri(winrt::to_hstring(url)),
        winrt::Windows::Web::Http::HttpCompletionOption::ResponseHeadersRead);
    co_await SaveHttpResponse(
        response, archive,
        [](uint64_t received, std::optional<uint64_t> total) {
          if (total && *total) {
            s_download_progress = float(std::min(0.8, 0.8 * double(received) / *total));
          }
        });
    s_download_progress = 0.8f;
    count = ExtractPatchArchive(archive, std::filesystem::u8path(dest_folder));
    s_download_progress = 1.0f;
  } catch (const winrt::hresult_error& e) {
    error = winrt::to_string(e.message());
  } catch (const std::exception& e) {
    error = e.what();
  } catch (...) {
    error = "Unknown patch download error.";
  }
  s_download_in_progress = false;
  co_await caller;
  callback(error.empty(), error.empty()
                              ? std::to_string(count) + " patch files installed."
                              : "Patch download failed: " + error);
}

void UWP::DownloadAndExtractZip(
    const std::string& url, const std::string& dest_folder,
    std::function<void(bool, std::string)> callback) {
  DownloadAndExtractZipAsync(url, dest_folder, callback);
}

static std::string FormatHex(uint32_t value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08X", value);
  return std::string(buf);
}

static std::string FormatTitleId(uint32_t title_id) {
  return FormatHex(title_id);
}

static std::string FormatMediaId(uint32_t media_id) {
  return FormatHex(media_id);
}

static bool TryParseTitleIdPrefix(const std::string& text,
                                  std::string* out_title_id) {
  if (text.size() < 8) {
    return false;
  }
  for (size_t i = 0; i < 8; ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(text[i]))) {
      return false;
    }
  }
  std::string id = text.substr(0, 8);
  std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  *out_title_id = id;
  return true;
}

static bool EqualsCaseInsensitive(const std::string& lhs,
                                  const std::string& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
        std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

static bool FindDefaultXexInZar(ZArchiveReader* reader,
                                ZArchiveNodeHandle dir_handle,
                                const std::string& current_path,
                                ZArchiveNodeHandle* out_xex_handle) {
  if (!reader || dir_handle == ZARCHIVE_INVALID_NODE || !out_xex_handle) {
    return false;
  }

  const uint32_t entry_count = reader->GetDirEntryCount(dir_handle);
  for (uint32_t i = 0; i < entry_count; ++i) {
    ZArchiveReader::DirEntry entry = {};
    if (!reader->GetDirEntry(dir_handle, i, entry)) {
      continue;
    }

    std::string name(entry.name.data(), entry.name.size());
    std::string full_path =
        current_path.empty() ? name : current_path + "/" + name;

    if (entry.isFile && EqualsCaseInsensitive(name, "default.xex")) {
      const auto handle = reader->LookUp(full_path);
      if (handle != ZARCHIVE_INVALID_NODE && reader->IsFile(handle)) {
        *out_xex_handle = handle;
        return true;
      }
    }

    if (entry.isDirectory) {
      const auto child_handle = reader->LookUp(full_path, false, true);
      if (child_handle != ZARCHIVE_INVALID_NODE &&
          FindDefaultXexInZar(reader, child_handle, full_path,
                              out_xex_handle)) {
        return true;
      }
    }
  }

  return false;
}

static bool TryExtractExecutionInfoFromXexData(
    const uint8_t* data, size_t data_size,
    xe::xex2_opt_execution_info* out_info) {
  if (!data || data_size < sizeof(xe::xex2_header)) {
    return false;
  }

  const auto* header = reinterpret_cast<const xe::xex2_header*>(data);
  const uint32_t magic = header->magic;
  if (magic != xe::cpu::kXEX2Signature && magic != xe::cpu::kXEX1Signature &&
      magic != xe::cpu::kXEX25Signature) {
    return false;
  }

  const uint32_t header_count = header->header_count;
  if (header_count == 0) {
    return false;
  }

  const size_t headers_size = 0x18 + (size_t(header_count) * 8);
  if (headers_size > data_size) {
    return false;
  }

  for (uint32_t i = 0; i < header_count; ++i) {
    const auto& opt_header = header->headers[i];
    const uint32_t key = opt_header.key;
    if (key != xe::XEX_HEADER_EXECUTION_INFO) {
      continue;
    }

    // XEX behavior mirrors XexModule::GetOptHeader.
    if ((key & 0xFF) == 0x01) {
      const auto* info = reinterpret_cast<const xe::xex2_opt_execution_info*>(
          &opt_header.value);
      *out_info = *info;
      return true;
    }

    const uint32_t offset = opt_header.offset;
    if (offset + sizeof(xe::xex2_opt_execution_info) > data_size) {
      return false;
    }

    const auto* info =
        reinterpret_cast<const xe::xex2_opt_execution_info*>(data + offset);
    *out_info = *info;
    return true;
  }

  return false;
}

static bool TryExtractExecutionInfoFromXexFile(
    const std::filesystem::path& path, xe::xex2_opt_execution_info* out_info) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }

  std::vector<uint8_t> data(1024 * 1024);
  const size_t bytes_read = fread(data.data(), 1, data.size(), file);
  fclose(file);
  if (!bytes_read) {
    return false;
  }

  return TryExtractExecutionInfoFromXexData(data.data(), bytes_read, out_info);
}

static bool TryReadXexExecutionInfoFromZar(
    const std::filesystem::path& path, xe::xex2_opt_execution_info* out_info) {
  std::unique_ptr<ZArchiveReader> reader(ZArchiveReader::OpenFromFile(path));
  if (!reader) {
    return false;
  }

  ZArchiveNodeHandle xex_handle = ZARCHIVE_INVALID_NODE;

  // Common lookup paths first.
  const char* direct_candidates[] = {"default.xex", "/default.xex",
                                     "\\default.xex"};
  for (const char* candidate : direct_candidates) {
    const auto handle = reader->LookUp(candidate);
    if (handle != ZARCHIVE_INVALID_NODE && reader->IsFile(handle)) {
      xex_handle = handle;
      break;
    }
  }

  if (xex_handle == ZARCHIVE_INVALID_NODE) {
    auto root = reader->LookUp("/", false, true);

    if (root == ZARCHIVE_INVALID_NODE) {
      root = reader->LookUp("", false, true);
    }
    if (root == ZARCHIVE_INVALID_NODE) {
      return false;
    }
    if (!FindDefaultXexInZar(reader.get(), root, "", &xex_handle)) {
      return false;
    }
  }

  const uint64_t xex_size = reader->GetFileSize(xex_handle);
  if (!xex_size) {
    return false;
  }
  const size_t to_read =
      static_cast<size_t>(std::min<uint64_t>(xex_size, 1024 * 1024));
  std::vector<uint8_t> xex_data(to_read);
  const uint64_t bytes_read =
      reader->ReadFromFile(xex_handle, 0, to_read, xex_data.data());
  if (!bytes_read) {
    return false;
  }

  return TryExtractExecutionInfoFromXexData(
      xex_data.data(), static_cast<size_t>(bytes_read), out_info);
}

static xe::vfs::Entry* FindDefaultXexInDiscEntryTree(xe::vfs::Entry* entry) {
  if (!entry) {
    return nullptr;
  }

  if (!(entry->attributes() & xe::vfs::kFileAttributeDirectory) &&
      EqualsCaseInsensitive(entry->name(), "default.xex")) {
    return entry;
  }

  for (const auto& child : entry->children()) {
    if (auto* found = FindDefaultXexInDiscEntryTree(child.get())) {
      return found;
    }
  }

  return nullptr;
}

static bool TryReadXexExecutionInfoFromDevice(
    xe::vfs::Device& device, xe::xex2_opt_execution_info* out_info) {
  if (!device.Initialize()) {
    return false;
  }

  xe::vfs::Entry* xex_entry = device.ResolvePath("default.xex");
  if (!xex_entry) {
    xex_entry = FindDefaultXexInDiscEntryTree(device.ResolvePath(""));
  }
  if (!xex_entry) {
    return false;
  }

  xe::vfs::File* xex_file = nullptr;
  auto open_status =
      xex_entry->Open(xe::filesystem::FileAccess::kGenericRead, &xex_file);
  if (XFAILED(open_status) || !xex_file) {
    return false;
  }

  std::vector<uint8_t> xex_data(1024 * 1024);
  size_t bytes_read = 0;
  auto read_status = xex_file->ReadSync(
      std::span<uint8_t>(xex_data.data(), xex_data.size()), 0, &bytes_read);
  xex_file->Destroy();
  if (XFAILED(read_status) || !bytes_read) {
    return false;
  }

  return TryExtractExecutionInfoFromXexData(xex_data.data(), bytes_read,
                                            out_info);
}

static bool TryReadXexExecutionInfoFromDiscImage(
    const std::filesystem::path& path, xe::xex2_opt_execution_info* out_info) {
  xe::vfs::DiscImageDevice device("", path);
  return TryReadXexExecutionInfoFromDevice(device, out_info);
}

static std::optional<xe::xex2_opt_execution_info> ReadContainerXexExecutionInfo(
    const std::filesystem::path& path) {
  try {
    xe::vfs::VirtualFileSystem file_system;
    xe::kernel::xam::ContentPackageContainer package(&file_system, "", path);
    auto device = package.MountPackage();
    xe::xex2_opt_execution_info info{};
    if (device && TryReadXexExecutionInfoFromDevice(*device, &info)) {
      return info;
    }
  } catch (const std::exception& error) {
    XELOGE("Could not read game executable metadata: {}", error.what());
  }
  return std::nullopt;
}

static std::string GetTitleAndMediaFromPath(const std::string& game_path,
                                           uint32_t* out_media_id,
                                           std::string* out_base_version = nullptr) {
  const std::filesystem::path path = std::filesystem::u8path(game_path);
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  if (out_base_version) {
    out_base_version->clear();
  }

  switch (xe::GetFileSignature(path)) {
    case xe::Emulator::FileSignatureType::XEX25:
    case xe::Emulator::FileSignatureType::XEX1:
    case xe::Emulator::FileSignatureType::XEX2: {
      xe::xex2_opt_execution_info info = {};
      if (TryExtractExecutionInfoFromXexFile(path, &info)) {
        if (out_base_version) {
          *out_base_version =
              fmt::format("{:08X}", uint32_t(info.base_version_value));
        }
        if (out_media_id) {
          *out_media_id = info.media_id;
        }
        title_id = info.title_id;
        return FormatTitleId(title_id);
      }
      XELOGE("[UWP] Failed to extract XEX execution info from '{}'.",
             game_path);
      break;
    }

    case xe::Emulator::FileSignatureType::CON:
    case xe::Emulator::FileSignatureType::LIVE:
    case xe::Emulator::FileSignatureType::PIRS: {
      // Converted containers may omit the executable's version fields.
      const auto executable = out_base_version
                                  ? ReadContainerXexExecutionInfo(path)
                                  : std::nullopt;
      const auto info = executable ? executable : ReadContentExecutionInfo(path);
      if (info) {
        if (out_base_version && executable) {
          *out_base_version =
              fmt::format("{:08X}", uint32_t(info->base_version_value));
        }
        title_id = info->title_id;
        if (out_media_id) {
          *out_media_id = info->media_id;
        }
      }
      if (title_id) {
        return FormatTitleId(title_id);
      }
      XELOGE("[UWP] Failed to read content execution info from '{}'.", game_path);
      break;
    }
    case xe::Emulator::FileSignatureType::ZAR: {
      xe::xex2_opt_execution_info info = {};
      if (TryReadXexExecutionInfoFromZar(path, &info)) {
        if (out_base_version) {
          *out_base_version =
              fmt::format("{:08X}", uint32_t(info.base_version_value));
        }
        if (out_media_id) {
          *out_media_id = info.media_id;
        }
        title_id = info.title_id;
        return FormatTitleId(title_id);
      }
      XELOGE("[UWP] Failed to read default.xex execution info from ZAR '{}'.",
             game_path);
      break;
    }
    case xe::Emulator::FileSignatureType::XISO: {
      xe::xex2_opt_execution_info info = {};
      if (TryReadXexExecutionInfoFromDiscImage(path, &info)) {
        if (out_base_version) {
          *out_base_version =
              fmt::format("{:08X}", uint32_t(info.base_version_value));
        }
        if (out_media_id) {
          *out_media_id = info.media_id;
        }
        title_id = info.title_id;
        return FormatTitleId(title_id);
      }
      XELOGE("[UWP] Failed to read default.xex execution info from XISO '{}'.",
             game_path);
      break;
    }
    default:
      break;
  }

  std::string maybe_title_id;
  const std::string stem = xe::path_to_utf8(path.stem());
  if (TryParseTitleIdPrefix(stem, &maybe_title_id)) {
    return maybe_title_id;
  }

  const std::string file_name = xe::path_to_utf8(path.filename());
  if (TryParseTitleIdPrefix(file_name, &maybe_title_id)) {
    return maybe_title_id;
  }

  if (out_media_id) {
    *out_media_id = media_id;
  }
  XELOGE("[UWP] Failed to resolve title/media from '{}'.", game_path);
  return "";
}

std::string UWP::GetTitleIdFromPath(const std::string& game_path) {
  return GetTitleAndMediaFromPath(game_path, nullptr);
}

std::string UWP::GetMediaIdFromPath(const std::string& game_path) {
  uint32_t media_id = 0;
  GetTitleAndMediaFromPath(game_path, &media_id);
  if (!media_id) {
    return "";
  }
  return FormatMediaId(media_id);
}

UWP::TitleUpdateGameInfo UWP::GetTitleUpdateGameInfo(
    const std::string& game_path) {
  TitleUpdateGameInfo info;
  uint32_t media_id = 0;
  info.title_id = GetTitleAndMediaFromPath(game_path, &media_id,
                                         &info.base_version);
  if (media_id) {
    info.media_id = FormatMediaId(media_id);
  }
  return info;
}

static std::vector<std::string> ExtractDownloadUrlsForTitleId(
    const std::string& json, const std::string& title_id) {
  std::vector<std::string> urls;
  try {
    auto parsed =
        winrt::Windows::Data::Json::JsonValue::Parse(winrt::to_hstring(json));
    if (parsed.ValueType() !=
        winrt::Windows::Data::Json::JsonValueType::Array) {
      return urls;
    }

    auto arr = parsed.GetArray();
    for (const auto& item : arr) {
      if (item.ValueType() !=
          winrt::Windows::Data::Json::JsonValueType::Object) {
        continue;
      }

      auto obj = item.GetObject();
      if (!obj.HasKey(L"name") || !obj.HasKey(L"download_url")) {
        continue;
      }

      const std::string name = winrt::to_string(obj.GetNamedString(L"name"));
      if (name.size() < 8 ||
          _strnicmp(name.c_str(), title_id.c_str(), 8) != 0) {
        continue;
      }

      const std::string download_url =
          winrt::to_string(obj.GetNamedString(L"download_url"));
      if (!download_url.empty()) {
        urls.push_back(download_url);
      }
    }
  } catch (...) {
    return {};
  }

  return urls;
}

winrt::fire_and_forget DownloadFilesFromUrlsAsync(
    std::vector<std::string> urls, std::string dest_folder,
    std::function<void(bool, std::string)> callback) {
  // Continues the operation reserved by FetchAndDownloadForTitleIdAsync.
  s_download_progress = 0.0f;
  size_t downloaded = 0;
  std::string error;
  try {
    winrt::Windows::Web::Http::HttpClient client;
    client.DefaultRequestHeaders().Append(L"User-Agent", L"xenia-uwp/1.0");
    for (size_t i = 0; i < urls.size(); ++i) {
      const auto uri = winrt::Windows::Foundation::Uri(winrt::to_hstring(urls[i]));
      const auto url_path = winrt::to_string(uri.Path());
      const auto name = winrt::to_string(
          winrt::Windows::Foundation::Uri::UnescapeComponent(
              winrt::to_hstring(url_path.substr(url_path.rfind('/') + 1))));
      if (name.empty() || name == "." || name == ".." ||
          name.find_first_of("/\\:") != std::string::npos ||
          name.find('\0') != std::string::npos) {
        throw std::runtime_error("The server supplied an invalid download filename.");
      }
      auto response = co_await client.GetAsync(
          uri, winrt::Windows::Web::Http::HttpCompletionOption::ResponseHeadersRead);
      const auto output = std::filesystem::u8path(dest_folder) /
                          std::filesystem::u8path(name);
      co_await SaveHttpResponse(
          response, output,
          [i, count = urls.size()](uint64_t received, std::optional<uint64_t> total) {
            const double fraction = total && *total
                                        ? std::min(0.99, double(received) / *total)
                                        : 0.0;
            s_download_progress = float((i + fraction) / count);
          });
      ++downloaded;
      s_download_progress = float(downloaded) / float(urls.size());
    }
  } catch (const winrt::hresult_error& e) {
    error = winrt::to_string(e.message());
  } catch (const std::exception& e) {
    error = e.what();
  } catch (...) {
    error = "Unknown download error.";
  }
  s_download_in_progress = false;
  const bool success = error.empty() && downloaded != 0;
  callback(success, success ? std::to_string(downloaded) + " file(s) downloaded."
                            : "Download failed: " + error);
}
winrt::fire_and_forget FetchAndDownloadForTitleIdAsync(
    std::string api_url, std::string title_id, std::string dest_folder,
    std::function<void(bool, std::string)> callback) {
  if (s_download_in_progress.exchange(true)) {
    callback(false, "Another download is already in progress.");
    co_return;
  }
  s_download_progress = 0.0f;
  try {
    winrt::Windows::Web::Http::HttpClient client;
    // GitHub API requires User-Agent
    client.DefaultRequestHeaders().Append(L"User-Agent", L"xenia-uwp/1.0");

    auto uri = winrt::Windows::Foundation::Uri(winrt::to_hstring(api_url));
    auto response = co_await client.GetAsync(uri);
    response.EnsureSuccessStatusCode();

    auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(
        co_await response.Content().ReadAsBufferAsync());
    std::vector<uint8_t> buf(reader.UnconsumedBufferLength());
    reader.ReadBytes(winrt::array_view<uint8_t>(buf));
    std::string json(buf.begin(), buf.end());

    auto urls = ExtractDownloadUrlsForTitleId(json, title_id);
    if (urls.empty()) {
      s_download_in_progress = false;
      callback(false, "");
      co_return;
    }

    // Download each matched file
    DownloadFilesFromUrlsAsync(std::move(urls), dest_folder, callback);
  } catch (...) {
    s_download_in_progress = false;
    callback(false, "");
  }
}

void UWP::DownloadPatchesForGame(
    const std::string& title_id, const std::string& dest_folder,
    std::function<void(bool, std::string)> callback) {
  const std::string api_url =
      "https://api.github.com/repos/xenia-canary/game-patches/contents/"
      "patches";
  FetchAndDownloadForTitleIdAsync(api_url, title_id, dest_folder, callback);
}

void UWP::DownloadPluginsForGame(
    const std::string& title_id, const std::string& dest_folder,
    std::function<void(bool, std::string)> callback) {
  // Plugins repo: xenia-canary/game-plugins (same naming convention)
  const std::string api_url =
      "https://api.github.com/repos/xenia-canary/game-plugins/contents/"
      "plugins";
  FetchAndDownloadForTitleIdAsync(api_url, title_id, dest_folder, callback);
}

winrt::fire_and_forget DownloadConfigForGameAsync(
    std::string title_id, std::string dest_folder,
    std::function<void(bool, std::string)> callback) {
  if (s_download_in_progress.exchange(true)) {
    callback(false, "Another download is already in progress.");
    co_return;
  }
  s_download_progress = 0.0f;
  try {
    std::string normalized = title_id;
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    auto url = fmt::format(
        "https://raw.githubusercontent.com/xenia-manager/optimized-settings/"
        "main/settings/{}.toml",
        normalized);

    winrt::Windows::Web::Http::HttpClient client;
    client.DefaultRequestHeaders().Append(L"User-Agent", L"xenia-uwp/1.0");

    auto uri = winrt::Windows::Foundation::Uri(winrt::to_hstring(url));
    auto response = co_await client.GetAsync(uri);
    if (response.StatusCode() ==
        winrt::Windows::Web::Http::HttpStatusCode::NotFound) {
      s_download_in_progress = false;
      callback(false, "not_found");
      co_return;
    }
    response.EnsureSuccessStatusCode();

    auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(
        co_await response.Content().ReadAsBufferAsync());
    std::vector<uint8_t> buf(reader.UnconsumedBufferLength());
    reader.ReadBytes(winrt::array_view<uint8_t>(buf));
    std::string toml(buf.begin(), buf.end());

    // The repository publishes each game's settings as a game config already.
    try {
      toml::parse(toml);
    } catch (const toml::parse_error& e) {
      XELOGE(
          "[UWP] DownloadConfigForGameAsync: optimized settings for {} are "
          "not valid TOML: {}",
          normalized, e.description());
      s_download_in_progress = false;
      callback(false, "invalid_toml");
      co_return;
    }

    std::filesystem::create_directories(dest_folder);
    std::filesystem::path out_path = std::filesystem::path(dest_folder) /
                                     (normalized + std::string(".config.toml"));

    bool already_installed = false;
    if (std::filesystem::exists(out_path)) {
      std::ifstream existing_ifs(out_path, std::ios::binary);
      std::string existing((std::istreambuf_iterator<char>(existing_ifs)),
                           std::istreambuf_iterator<char>());
      if (existing == toml) {
        already_installed = true;
      }
    }

    std::ofstream ofs(out_path, std::ios::binary);
    ofs.write(toml.data(), toml.size());
    ofs.close();

    s_download_in_progress = false;
    s_download_progress = 1.0f;
    callback(true, already_installed ? "already_installed" : "downloaded");
  } catch (const winrt::hresult_error& e) {
    XELOGE(
        "[UWP] DownloadConfigForGameAsync: hresult_error title_id={} "
        "code=0x{:08X} message='{}'",
        title_id, static_cast<uint32_t>(e.code().value),
        winrt::to_string(e.message()));
    s_download_in_progress = false;
    callback(false, "network_error");
  } catch (const std::exception& e) {
    XELOGE(
        "[UWP] DownloadConfigForGameAsync: exception title_id={} message='{}'",
        title_id, e.what());
    s_download_in_progress = false;
    callback(false, "exception");
  } catch (...) {
    XELOGE("[UWP] DownloadConfigForGameAsync: unknown exception title_id={}",
           title_id);
    s_download_in_progress = false;
    callback(false, "unknown_error");
  }
}

void UWP::DownloadConfigForGame(
    const std::string& title_id, const std::string& dest_folder,
    std::function<void(bool, std::string)> callback) {
  DownloadConfigForGameAsync(title_id, dest_folder, std::move(callback));
}

static std::string NormalizeId(std::string value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (char c : value) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      normalized.push_back(
          static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
  }
  return normalized;
}

static std::string TrimAsciiWhitespace(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

static std::string JsonValueToString(
    const winrt::Windows::Data::Json::JsonValue& value) {
  switch (value.ValueType()) {
    case winrt::Windows::Data::Json::JsonValueType::String:
      return winrt::to_string(value.GetString());
    case winrt::Windows::Data::Json::JsonValueType::Number: {
      double number = value.GetNumber();
      double rounded = std::round(number);
      if (std::fabs(number - rounded) < 1e-6) {
        return fmt::format("{:.0f}", rounded);
      }
      return fmt::format("{}", number);
    }
    default:
      return "";
  }
}

static std::string JsonObjectString(
    const winrt::Windows::Data::Json::JsonObject& object, const wchar_t* key) {
  if (!object.HasKey(key)) {
    return "";
  }
  return JsonValueToString(object.GetNamedValue(key));
}

static int JsonObjectInt(const winrt::Windows::Data::Json::JsonObject& object,
                         const wchar_t* key) {
  if (!object.HasKey(key)) {
    return 0;
  }
  auto value = object.GetNamedValue(key);
  if (value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Number) {
    return static_cast<int>(std::llround(value.GetNumber()));
  }
  if (value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String) {
    try {
      return std::stoi(winrt::to_string(value.GetString()));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

static std::string PickBestCoverId(
    const winrt::Windows::Data::Json::JsonArray& covers) {
  std::string best_cover_id;
  int best_score = -1;
  for (const auto& cover_value : covers) {
    if (cover_value.ValueType() !=
        winrt::Windows::Data::Json::JsonValueType::Object) {
      continue;
    }
    auto cover_obj = cover_value.GetObject();
    std::string cover_id = JsonObjectString(cover_obj, L"CoverID");
    if (cover_id.empty()) {
      continue;
    }
    int score = JsonObjectInt(cover_obj, L"Rating");
    if (JsonObjectString(cover_obj, L"Official") == "1") {
      score += 1000;
    }
    if (best_cover_id.empty() || score > best_score) {
      best_cover_id = cover_id;
      best_score = score;
    }
  }
  return best_cover_id;
}

winrt::fire_and_forget FetchUnityMetadataAsync(std::string title_id) {
  const std::string normalized_title_id = NormalizeId(title_id);
  if (normalized_title_id.empty()) {
    co_return;
  }

  UWP::UnityGameMetadata metadata;
  {
    std::lock_guard<std::mutex> lock(s_unity_metadata_mutex);
    auto it = s_unity_metadata_cache.find(normalized_title_id);
    if (it != s_unity_metadata_cache.end()) {
      metadata = it->second;
    }
    metadata.title_id = normalized_title_id;
    metadata.request_started = true;
    metadata.request_finished = false;
    s_unity_metadata_cache[normalized_title_id] = metadata;
  }

  try {
    winrt::Windows::Web::Http::HttpClient client;
    client.DefaultRequestHeaders().Append(L"User-Agent", L"xenia-uwp/1.0");

    const std::string x360db_url = fmt::format(
        "https://raw.githubusercontent.com/xenia-manager/x360db/main/titles/{}/"
        "info.json",
        normalized_title_id);
    auto x360db_response = co_await client.GetAsync(
        winrt::Windows::Foundation::Uri(winrt::to_hstring(x360db_url)));
    if (x360db_response.IsSuccessStatusCode()) {
      auto x360db_buffer =
          co_await x360db_response.Content().ReadAsBufferAsync();

      auto x360db_reader =
          winrt::Windows::Storage::Streams::DataReader::FromBuffer(
              x360db_buffer);
      std::vector<uint8_t> x360db_bytes(x360db_buffer.Length());
      x360db_reader.ReadBytes(winrt::array_view<uint8_t>(x360db_bytes));
      std::string x360db_json(x360db_bytes.begin(), x360db_bytes.end());

      try {
        auto parsed = winrt::Windows::Data::Json::JsonValue::Parse(
            winrt::to_hstring(x360db_json));
        if (parsed.ValueType() ==
            winrt::Windows::Data::Json::JsonValueType::Object) {
          auto root = parsed.GetObject();

          if (root.HasKey(L"title")) {
            auto title_obj = root.GetNamedObject(L"title");
            metadata.display_name = JsonObjectString(title_obj, L"full");
            if (metadata.display_name.empty()) {
              metadata.display_name = JsonObjectString(title_obj, L"reduced");
            }
          }

          metadata.publisher = JsonObjectString(root, L"publisher");
          metadata.developer = JsonObjectString(root, L"developer");
          metadata.release_date = JsonObjectString(root, L"release_date");
          metadata.user_rating = JsonObjectString(root, L"user_rating");

          if (root.HasKey(L"genre")) {
            auto genre_array = root.GetNamedArray(L"genre");
            metadata.genre.clear();
            for (const auto& genre_value : genre_array) {
              if (genre_value.ValueType() ==
                  winrt::Windows::Data::Json::JsonValueType::String) {
                metadata.genre.push_back(
                    winrt::to_string(genre_value.GetString()));
              }
            }
          }

          metadata.description_full.clear();
          metadata.description_short.clear();
          if (root.HasKey(L"description")) {
            auto desc_obj = root.GetNamedObject(L"description");
            metadata.description_full = JsonObjectString(desc_obj, L"full");
            metadata.description_short = JsonObjectString(desc_obj, L"short");
          }

          const auto metadata_root =
              std::filesystem::path(UWP::GetLocalCache()) / "x360db_metadata";
          const auto icons_root = metadata_root / "icons";
          const auto covers_root = metadata_root / "covers";
          const auto banners_root = metadata_root / "banners";
          const auto backgrounds_root = metadata_root / "backgrounds";
          const auto galleries_root = metadata_root / "galleries";
          std::filesystem::create_directories(icons_root);
          std::filesystem::create_directories(covers_root);
          std::filesystem::create_directories(banners_root);
          std::filesystem::create_directories(backgrounds_root);
          std::filesystem::create_directories(galleries_root);

          if (root.HasKey(L"artwork")) {
            auto artwork = root.GetNamedObject(L"artwork");

            std::string icon_url = JsonObjectString(artwork, L"icon");
            if (!icon_url.empty()) {
              auto icon_path =
                  icons_root / fmt::format("{}.png", normalized_title_id);
              if (!std::filesystem::exists(icon_path)) {
                auto icon_response =
                    co_await client.GetAsync(winrt::Windows::Foundation::Uri(
                        winrt::to_hstring(icon_url)));
                if (icon_response.IsSuccessStatusCode()) {
                  auto icon_buffer =
                      co_await icon_response.Content().ReadAsBufferAsync();
                  if (icon_buffer.Length() > 0) {
                    auto icon_reader = winrt::Windows::Storage::Streams::
                        DataReader::FromBuffer(icon_buffer);
                    std::vector<uint8_t> icon_bytes(icon_buffer.Length());
                    icon_reader.ReadBytes(
                        winrt::array_view<uint8_t>(icon_bytes));
                    std::ofstream icon_ofs(icon_path, std::ios::binary);
                    icon_ofs.write(
                        reinterpret_cast<const char*>(icon_bytes.data()),
                        icon_bytes.size());
                  }
                }
              }
              if (std::filesystem::exists(icon_path)) {
                metadata.icon_path = xe::path_to_utf8(icon_path);
              }
            }

            std::string boxart_url = JsonObjectString(artwork, L"boxart");
            if (!boxart_url.empty()) {
              auto cover_path =
                  covers_root / fmt::format("{}.png", normalized_title_id);
              if (!std::filesystem::exists(cover_path)) {
                auto cover_response =
                    co_await client.GetAsync(winrt::Windows::Foundation::Uri(
                        winrt::to_hstring(boxart_url)));
                if (cover_response.IsSuccessStatusCode()) {
                  auto cover_buffer =
                      co_await cover_response.Content().ReadAsBufferAsync();
                  if (cover_buffer.Length() > 0) {
                    auto cover_reader = winrt::Windows::Storage::Streams::
                        DataReader::FromBuffer(cover_buffer);
                    std::vector<uint8_t> cover_bytes(cover_buffer.Length());
                    cover_reader.ReadBytes(
                        winrt::array_view<uint8_t>(cover_bytes));
                    std::ofstream cover_ofs(cover_path, std::ios::binary);
                    cover_ofs.write(
                        reinterpret_cast<const char*>(cover_bytes.data()),
                        cover_bytes.size());
                  }
                }
              }
              if (std::filesystem::exists(cover_path)) {
                metadata.cover_path = xe::path_to_utf8(cover_path);
              }
            }

            std::string banner_url = JsonObjectString(artwork, L"banner");
            if (!banner_url.empty()) {
              auto banner_path =
                  banners_root / fmt::format("{}.png", normalized_title_id);
              if (!std::filesystem::exists(banner_path)) {
                auto banner_response =
                    co_await client.GetAsync(winrt::Windows::Foundation::Uri(
                        winrt::to_hstring(banner_url)));
                if (banner_response.IsSuccessStatusCode()) {
                  auto banner_buffer =
                      co_await banner_response.Content().ReadAsBufferAsync();
                  if (banner_buffer.Length() > 0) {
                    auto banner_reader = winrt::Windows::Storage::Streams::
                        DataReader::FromBuffer(banner_buffer);
                    std::vector<uint8_t> banner_bytes(banner_buffer.Length());
                    banner_reader.ReadBytes(
                        winrt::array_view<uint8_t>(banner_bytes));
                    std::ofstream banner_ofs(banner_path, std::ios::binary);
                    banner_ofs.write(
                        reinterpret_cast<const char*>(banner_bytes.data()),
                        banner_bytes.size());
                  }
                }
              }
              if (std::filesystem::exists(banner_path)) {
                metadata.banner_path = xe::path_to_utf8(banner_path);
              }
            }

            std::string background_url =
                JsonObjectString(artwork, L"background");
            if (!background_url.empty()) {
              auto background_path =
                  backgrounds_root / fmt::format("{}.png", normalized_title_id);
              if (!std::filesystem::exists(background_path)) {
                auto bg_response =
                    co_await client.GetAsync(winrt::Windows::Foundation::Uri(
                        winrt::to_hstring(background_url)));
                if (bg_response.IsSuccessStatusCode()) {
                  auto bg_buffer =
                      co_await bg_response.Content().ReadAsBufferAsync();
                  if (bg_buffer.Length() > 0) {
                    auto bg_reader = winrt::Windows::Storage::Streams::
                        DataReader::FromBuffer(bg_buffer);
                    std::vector<uint8_t> bg_bytes(bg_buffer.Length());
                    bg_reader.ReadBytes(winrt::array_view<uint8_t>(bg_bytes));
                    std::ofstream bg_ofs(background_path, std::ios::binary);
                    bg_ofs.write(reinterpret_cast<const char*>(bg_bytes.data()),
                                 bg_bytes.size());
                  }
                }
              }
              if (std::filesystem::exists(background_path)) {
                metadata.background_path = xe::path_to_utf8(background_path);
              }
            }

            metadata.gallery_paths.clear();
            if (artwork.HasKey(L"gallery")) {
              auto gallery_array = artwork.GetNamedArray(L"gallery");
              size_t gallery_index = 0;
              constexpr size_t kMaxGalleryImages = 4;
              for (const auto& gallery_value : gallery_array) {
                if (gallery_index >= kMaxGalleryImages) {
                  break;
                }
                if (gallery_value.ValueType() !=
                    winrt::Windows::Data::Json::JsonValueType::String) {
                  ++gallery_index;
                  continue;
                }
                std::string gallery_url =
                    winrt::to_string(gallery_value.GetString());
                if (gallery_url.empty()) {
                  ++gallery_index;
                  continue;
                }
                auto gallery_path =
                    galleries_root / fmt::format("{}_{}.png",
                                                 normalized_title_id,
                                                 gallery_index);
                if (!std::filesystem::exists(gallery_path)) {
                  auto gallery_response =
                      co_await client.GetAsync(winrt::Windows::Foundation::Uri(
                          winrt::to_hstring(gallery_url)));
                  if (gallery_response.IsSuccessStatusCode()) {
                    auto gallery_buffer =
                        co_await gallery_response.Content().ReadAsBufferAsync();
                    if (gallery_buffer.Length() > 0) {
                      auto gallery_reader = winrt::Windows::Storage::Streams::
                          DataReader::FromBuffer(gallery_buffer);
                      std::vector<uint8_t> gallery_bytes(
                          gallery_buffer.Length());
                      gallery_reader.ReadBytes(
                          winrt::array_view<uint8_t>(gallery_bytes));
                      std::ofstream gallery_ofs(gallery_path, std::ios::binary);
                      gallery_ofs.write(
                          reinterpret_cast<const char*>(gallery_bytes.data()),
                          gallery_bytes.size());
                    }
                  }
                }
                if (std::filesystem::exists(gallery_path)) {
                  metadata.gallery_paths.push_back(
                      xe::path_to_utf8(gallery_path));
                }
                ++gallery_index;
              }
            }
          }
        }
      } catch (...) {
        XELOGE(
            "[UWP] FetchUnityMetadataAsync: failed to parse x360db response "
            "for {}",
            normalized_title_id);
      }
    }

    metadata.metadata_ready =
        !metadata.display_name.empty() || !metadata.icon_path.empty() ||
        !metadata.cover_path.empty() || !metadata.banner_path.empty() ||
        !metadata.description_short.empty() ||
        !metadata.description_full.empty() || !metadata.publisher.empty() ||
        !metadata.developer.empty();

    metadata.request_finished = true;

    {
      std::lock_guard<std::mutex> lock(s_unity_metadata_mutex);
      s_unity_metadata_cache[normalized_title_id] = metadata;
    }
  } catch (...) {
    XELOGE("[UWP] FetchUnityMetadataAsync: request failed for {}",
           normalized_title_id);
    metadata.request_finished = true;
    {
      std::lock_guard<std::mutex> lock(s_unity_metadata_mutex);
      s_unity_metadata_cache[normalized_title_id] = metadata;
    }
  }
}

void UWP::EnsureUnityMetadataFetch(const std::string& title_id) {
  const std::string normalized_title_id = NormalizeId(title_id);
  if (normalized_title_id.empty()) {
    return;
  }

  bool should_fetch = false;
  {
    std::lock_guard<std::mutex> lock(s_unity_metadata_mutex);
    auto& metadata = s_unity_metadata_cache[normalized_title_id];
    if (!metadata.request_started) {
      metadata.title_id = normalized_title_id;
      metadata.request_started = true;
      metadata.request_finished = false;
      metadata.metadata_ready = false;
      should_fetch = true;
    }
  }

  if (should_fetch) {
    FetchUnityMetadataAsync(normalized_title_id);
  }
}

bool UWP::TryGetUnityMetadata(const std::string& title_id,
                              UnityGameMetadata* out_metadata) {
  if (!out_metadata) {
    return false;
  }

  const std::string normalized_title_id = NormalizeId(title_id);
  if (normalized_title_id.empty()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(s_unity_metadata_mutex);
  auto it = s_unity_metadata_cache.find(normalized_title_id);
  if (it == s_unity_metadata_cache.end()) {
    return false;
  }
  *out_metadata = it->second;
  return true;
}

winrt::fire_and_forget ListTitleUpdatesForGameAsync(
    std::string title_id, std::string media_id, std::string base_version,
    std::function<void(bool, std::vector<TitleUpdateCandidate>, std::string)>
        callback) {
  std::vector<TitleUpdateCandidate> candidates;
  std::string error;
  try {
    const auto normalized_title_id = NormalizeTitleUpdateId(title_id);
    const auto normalized_media_id = NormalizeTitleUpdateId(media_id);
    const auto normalized_base_version =
        NormalizeTitleUpdateBaseVersion(base_version);
    if (normalized_title_id.empty() || normalized_media_id.empty() ||
        normalized_base_version.empty()) {
      error = "The game's title ID, media ID or base version is unavailable. "
              "A compatible update cannot be selected.";
    } else {
      winrt::Windows::Web::Http::HttpClient client;
      client.DefaultRequestHeaders().Append(L"User-Agent", L"xenia-uwp/1.0");
      const std::string url =
          "https://xboxunity.net/api/tu/" + normalized_title_id + "/" +
          normalized_base_version;
      auto response = co_await client.GetAsync(
          winrt::Windows::Foundation::Uri(winrt::to_hstring(url)));
      response.EnsureSuccessStatusCode();
      const auto json = winrt::to_string(co_await response.Content().ReadAsStringAsync());
      candidates = ParseTitleUpdateCandidates(json, normalized_media_id, error,
                                               normalized_title_id,
                                               normalized_base_version);
      XELOGI("Title update lookup: title {} media {} base {} returned {} updates",
             normalized_title_id, normalized_media_id, normalized_base_version,
             candidates.size());
    }
  } catch (const winrt::hresult_error& e) {
    error = "Could not retrieve title updates: " + winrt::to_string(e.message());
  } catch (const std::exception& e) {
    error = "Could not retrieve title updates: " + std::string(e.what());
  } catch (...) {
    error = "Could not retrieve title updates.";
  }
  const bool success = error.empty();
  callback(success, std::move(candidates), std::move(error));
}

void UWP::ListTitleUpdatesForGame(
    const std::string& title_id, const std::string& media_id,
    const std::string& base_version,
    std::function<void(bool, std::vector<TitleUpdateCandidate>, std::string)>
        callback) {
  ListTitleUpdatesForGameAsync(title_id, media_id, base_version,
                              std::move(callback));
}

winrt::fire_and_forget DownloadTitleUpdateForGameAsync(
    std::string title_id, std::string media_id, TitleUpdateCandidate candidate,
    std::string dest_folder,
    std::function<void(bool, std::vector<std::string>, std::string)> callback) {
  if (NormalizeTitleUpdateId(title_id).empty() ||
      !IsMatchingTitleUpdate(candidate, media_id)) {
    callback(false, {}, "The selected update does not match this game's media ID.");
    co_return;
  }
  if (s_download_in_progress.exchange(true)) {
    callback(false, {}, "Another download is already in progress.");
    co_return;
  }
  s_download_progress = 0.0f;
  std::vector<std::string> files;
  std::string error;
  try {
    winrt::Windows::Web::Http::HttpClient client;
    client.DefaultRequestHeaders().Append(L"User-Agent", L"xenia-uwp/1.0");
    std::string url = candidate.download_url.empty()
                          ? "https://assets.xboxunity.net/api/gettu/" + candidate.id
                          : candidate.download_url;
    if (url.starts_with("http://")) {
      url.replace(0, 7, "https://");
    }
    auto response = co_await client.GetAsync(
        winrt::Windows::Foundation::Uri(winrt::to_hstring(url)),
        winrt::Windows::Web::Http::HttpCompletionOption::ResponseHeadersRead);
    const auto output = std::filesystem::u8path(dest_folder) /
                        ("TU_" + candidate.id + ".tu");
    co_await SaveHttpResponse(
        response, output,
        [](uint64_t received, std::optional<uint64_t> total) {
          if (total && *total) {
            s_download_progress = std::min(0.99f, float(double(received) / *total));
          }
        });
    files.push_back(xe::path_to_utf8(output));
    s_download_progress = 1.0f;
  } catch (const winrt::hresult_error& e) {
    error = "Could not download the selected update: " + winrt::to_string(e.message());
  } catch (const std::exception& e) {
    error = "Could not download the selected update: " + std::string(e.what());
  } catch (...) {
    error = "Could not download the selected update.";
  }
  s_download_in_progress = false;
  callback(error.empty(), std::move(files),
           error.empty() ? "The selected title update was downloaded." : error);
}

void UWP::DownloadTitleUpdateForGame(
    const std::string& title_id, const std::string& media_id,
    const TitleUpdateCandidate& candidate, const std::string& dest_folder,
    std::function<void(bool, std::vector<std::string>, std::string)> callback) {
  DownloadTitleUpdateForGameAsync(title_id, media_id, candidate, dest_folder,
                                 std::move(callback));
}
}  // namespace UWP
