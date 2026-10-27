// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UWP_GAME_STORAGE_H_
#define XENIA_UWP_GAME_STORAGE_H_

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace UWP {

inline constexpr char kDefaultGameDirectory[] = "E:/Games/xbox360";
inline constexpr char kDevelopmentGameDirectory[] =
    "D:/DevelopmentFiles/Games/xbox360";

inline std::string ComparableStoragePath(std::string_view path) {
  const auto normalized = std::filesystem::u8path(path.begin(), path.end())
                              .lexically_normal().generic_u8string();
  std::string result(normalized.begin(), normalized.end());
  std::replace(result.begin(), result.end(), '/', '\\');
  while (result.size() > 3 && result.back() == '\\') {
    result.pop_back();
  }
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

inline bool SameStoragePath(std::string_view first, std::string_view second) {
  return ComparableStoragePath(first) == ComparableStoragePath(second);
}

// Replacing the primary location preserves the additional game folders. An
// existing additional location becomes primary without leaving a duplicate.
inline std::vector<std::string> SelectGameStoragePath(
    std::vector<std::string> paths, const std::string& selected, bool primary) {
  if (selected.empty()) {
    return paths;
  }
  if (primary && !paths.empty()) {
    paths.erase(paths.begin());
  }
  if (primary) {
    std::erase_if(paths, [&](const std::string& path) {
      return SameStoragePath(path, selected);
    });
    paths.insert(paths.begin(), selected);
  } else if (std::none_of(paths.begin(), paths.end(), [&](const auto& path) {
               return SameStoragePath(path, selected);
             })) {
    paths.push_back(selected);
  }
  return paths;
}

}  // namespace UWP
#endif  // XENIA_UWP_GAME_STORAGE_H_
