// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UWP_GAME_SCANNER_H_
#define XENIA_UWP_GAME_SCANNER_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xenia/base/string.h"
#include "xenia/kernel/xam/xcontent/xcontent.h"

namespace UWP {

enum class GameFileKind { kUnsupported, kXex, kContainer, kDisc };

struct ScannedGame {
  std::filesystem::path path;
  std::string name;
};

class GameScanner {
 public:
  using Classifier = std::function<GameFileKind(const std::filesystem::path&)>;
  using ErrorHandler =
      std::function<void(const std::filesystem::path&, const std::string&)>;

  explicit GameScanner(Classifier classifier, ErrorHandler on_error = {})
      : classifier_(std::move(classifier)), on_error_(std::move(on_error)) {}

  const std::vector<ScannedGame>& games() const { return games_; }

  void Scan(const std::filesystem::path& directory) {
    if (!directories_.insert(PathKey(directory)).second) {
      return;
    }
    std::error_code ec;
    std::filesystem::directory_iterator current(
        directory, std::filesystem::directory_options::skip_permission_denied,
        ec);
    if (ec) {
      Report(directory, ec.message());
      return;
    }

    std::vector<std::filesystem::path> subdirectories;
    std::vector<std::filesystem::path> executables;
    const std::filesystem::directory_iterator end;
    while (current != end) {
      const auto entry = *current;
      if (entry.is_directory(ec)) {
        subdirectories.push_back(entry.path());
      } else if (!ec && entry.is_regular_file(ec)) {
        try {
          switch (classifier_(entry.path())) {
            case GameFileKind::kXex:
              if (IsExecutable(entry.path())) {
                executables.push_back(entry.path());
              }
              break;
            case GameFileKind::kContainer:
              if (auto name = ReadContainerName(entry.path())) {
                Add(entry.path(), std::move(*name));
              }
              break;
            case GameFileKind::kDisc:
              Add(entry.path(), Utf8(entry.path().stem()));
              break;
            default:
              break;
          }
        } catch (const std::exception& error) {
          Report(entry.path(), error.what());
        }
      }
      if (ec) {
        Report(entry.path(), ec.message());
      }
      current.increment(ec);
      if (ec) {
        Report(directory, ec.message());
        break;
      }
    }

    // A default executable suppresses siblings, not child games or other discs.
    const auto default_xex = std::find_if(
        executables.begin(), executables.end(), [](const auto& path) {
          return LowerAscii(Utf8(path.filename())) == "default.xex";
        });
    if (default_xex != executables.end()) {
      auto name = Utf8(default_xex->parent_path().filename());
      Add(*default_xex, name.empty() ? Utf8(default_xex->stem()) : name);
    } else {
      for (const auto& executable : executables) {
        Add(executable, Utf8(executable.stem()));
      }
    }
    for (const auto& child : subdirectories) {
      Scan(child);
    }
  }

 private:
  static std::string Utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
  }

  static std::string LowerAscii(std::string value) {
    for (auto& ch : value) {
      if (ch >= 'A' && ch <= 'Z') {
        ch += 'a' - 'A';
      }
    }
    return value;
  }

  static std::string PathKey(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
      normalized = path.lexically_normal();
    }
    auto key = Utf8(normalized);
    std::replace(key.begin(), key.end(), '/', '\\');
    return LowerAscii(std::move(key));
  }

  template <typename T>
  static bool ReadAt(std::ifstream& file, std::streamoff offset, T& value) {
    file.clear();
    file.seekg(offset);
    return bool(file.read(reinterpret_cast<char*>(&value), sizeof(value)));
  }

  static bool IsExecutable(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    xe::xex2_header header{};
    if (!file.read(reinterpret_cast<char*>(&header),
                   offsetof(xe::xex2_header, headers))) {
      return false;
    }
    constexpr uint32_t excluded =
        xe::XEX_MODULE_DLL_MODULE | xe::XEX_MODULE_MODULE_PATCH |
        xe::XEX_MODULE_PATCH_FULL | xe::XEX_MODULE_PATCH_DELTA;
    const auto value = static_cast<uint32_t>(header.module_flags);
    return (value & xe::XEX_MODULE_TITLE) && !(value & excluded);
  }

  static std::optional<std::string> ReadContainerName(
      const std::filesystem::path& path) {
    using namespace xe::kernel::xam;
    std::ifstream file(path, std::ios::binary);
    xe::be<xe::XContentType> type;
    constexpr auto metadata_offset =
        offsetof(XContentContainerHeader, content_metadata);
    if (!ReadAt(file,
                metadata_offset + offsetof(XContentMetadata, content_type),
                type)) {
      return std::nullopt;
    }
    // Match the desktop content browser's bootable package types.
    if (type != xe::XContentType::kArcadeTitle &&
        type != xe::XContentType::kGameDemo &&
        type != xe::XContentType::kInstalledGame &&
        type != xe::XContentType::kXbox360Title) {
      return std::nullopt;
    }

    std::array<xe::be<uint16_t>, 128> display_name{};
    if (ReadAt(file,
               metadata_offset + offsetof(XContentMetadata, display_name_raw),
               display_name)) {
      std::u16string title;
      for (const auto ch : display_name) {
        if (!ch) {
          break;
        }
        title.push_back(static_cast<uint16_t>(ch));
      }
      if (!title.empty()) {
        try {
          return xe::to_utf8(title);
        } catch (const std::exception&) {
          // Invalid optional display text must not hide a bootable package.
        }
      }
    }
    return Utf8(path.stem());
  }

  void Add(const std::filesystem::path& path, std::string name) {
    if (files_.insert(PathKey(path)).second) {
      games_.push_back({path, std::move(name)});
    }
  }

  void Report(const std::filesystem::path& path, const std::string& message) {
    if (on_error_) {
      on_error_(path, message);
    }
  }

  Classifier classifier_;
  ErrorHandler on_error_;
  std::unordered_set<std::string> directories_;
  std::unordered_set<std::string> files_;
  std::vector<ScannedGame> games_;
};

}  // namespace UWP
#endif  // XENIA_UWP_GAME_SCANNER_H_
