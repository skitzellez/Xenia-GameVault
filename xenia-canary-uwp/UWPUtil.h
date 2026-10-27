#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace xe {
class Emulator;
}

namespace UWP {
struct TitleUpdateCandidate {
  std::string id;
  std::string media_id;
  std::string content_id;
  std::string download_url;
  uint32_t version = 0;
  uint64_t size_kb = 0;
  uint64_t size_bytes = 0;
  std::string upload_date;
  std::string name;
  std::string base_version;
};

struct TitleUpdateGameInfo {
  std::string title_id;
  std::string media_id;
  std::string base_version;
};

struct UnityGameMetadata {
  bool request_started = false;
  bool request_finished = false;
  bool metadata_ready = false;
  std::string title_id;
  std::string display_name;
  std::string title_type;
  std::string description_short;
  std::string description_full;
  std::string newest_content;
  int covers = 0;
  int updates = 0;
  std::string icon_path;
  std::string cover_path;
  std::string banner_path;
  std::string publisher;
  std::string developer;
  std::vector<std::string> genre;
  std::string release_date;
  std::string user_rating;
  std::string background_path;
  std::vector<std::string> gallery_paths;
};

void SelectGameFromWinRT(xe::Emulator* emu);
// Game Vault: no profile yet -> one called "Player" is made and signed in, so games can save without the player
// meeting the "No Profiles Found" box first. True when there is a profile now.
bool EnsureProfile(xe::Emulator* emu);
bool HasGamePath();
std::string ConsumeGamePath();
void SelectFolder(std::function<void(std::string)> callback);
// Custom storage selections retain picker-granted access across restarts.
// Cancellation returns an empty path without an error.
void SelectStorageFolder(
    const std::wstring& access_token,
    std::function<void(std::string path, std::string error)> callback);
void SelectFile(std::function<void(std::string)> callback);
void SelectFiles(std::function<void(std::vector<std::string>)> callback);
bool TestPathPermissions(std::string path);
std::string GetLocalCache();
std::string GetLocalState();
// Installed package version as "major.minor.build.revision".
std::string GetPackageVersion();
// Game Vault: the "games" folder the publisher's apps share (Game Vault reads its "logs"); "" when missing
std::string GetPublisherGames();
// Game Vault: the profiles and their saves (content\<xuid>) are copied to the shared folder ("xbox 360 saves"),
// which outlives an uninstall of Xenia and which Game Vault copies to the USB drive. A fresh install (no profile)
// gets them back. Called once at start, before the profiles are read.
void SavesAtStart(const std::filesystem::path& content_root);
int GetCoreDPI();
void SetAutomaticLaunch(std::string game_path);
void SetDPI(int DPI);
bool IsUIOpen();
void SetUIOpen(bool is_open);
// Game Vault's "Show frame rate" option (fps=1 in its link): the FPS counter in the corner
void SetShowFps(bool on);
bool GetShowFps();
uint64_t GetUIInputCaptureState();
void LaunchUri(const std::string& url);
void DownloadAndExtractZip(const std::string& url,
                           const std::string& dest_folder,
                           std::function<void(bool, std::string)> callback);
bool IsDownloadInProgress();
float GetDownloadProgress();
std::string GetTitleIdFromPath(const std::string& game_path);
void DownloadPatchesForGame(const std::string& title_id,
                            const std::string& dest_folder,
                            std::function<void(bool, std::string)> callback);
void DownloadPluginsForGame(const std::string& title_id,
                            const std::string& dest_folder,
                            std::function<void(bool, std::string)> callback);
void DownloadConfigForGame(const std::string& title_id,
                           const std::string& dest_folder,
                           std::function<void(bool, std::string)> callback);
std::string GetMediaIdFromPath(const std::string& game_path);
void EnsureUnityMetadataFetch(const std::string& title_id);
bool TryGetUnityMetadata(const std::string& title_id,
                         UnityGameMetadata* out_metadata);
void ListTitleUpdatesForGame(
    const std::string& title_id, const std::string& media_id,
    const std::string& base_version,
    std::function<void(bool, std::vector<TitleUpdateCandidate>, std::string)>
        callback);
TitleUpdateGameInfo GetTitleUpdateGameInfo(const std::string& game_path);
void DownloadTitleUpdateForGame(
    const std::string& title_id, const std::string& media_id,
    const TitleUpdateCandidate& candidate,
    const std::string& dest_folder,
    std::function<void(bool, std::vector<std::string>, std::string)> callback);
}  // namespace UWP
