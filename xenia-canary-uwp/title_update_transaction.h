// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UWP_TITLE_UPDATE_TRANSACTION_H_
#define XENIA_UWP_TITLE_UPDATE_TRANSACTION_H_

#include <chrono>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include <windows.h>
#if XE_PLATFORM_WINRT
#include <fileapifromapp.h>
#endif
#endif

namespace UWP {

// Backups remain available for switching versions after a successful install.
class TitleUpdateTransaction {
 public:
  TitleUpdateTransaction(std::filesystem::path title_root, std::string media_id)
      : root_(NormalizeRoot(title_root)) {
    if (media_id.size() != 8 ||
        media_id.find_first_not_of("0123456789ABCDEF") != std::string::npos) {
      throw std::runtime_error("Invalid update backup media ID.");
    }
    backup_ = root_ / "TitleUpdateBackups" / media_id /
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count());
    CheckedBackup(backup_);
  }
  ~TitleUpdateTransaction() {
    if (!finished_) {
      Restore();
    }
  }

  const std::filesystem::path& backup_root() const { return backup_; }

  void Backup(const std::filesystem::path& path) {
    const auto source = CheckedPath(path);
    if (!std::filesystem::exists(source)) {
      return;
    }
    for (const auto& moved : moved_) {
      if (moved.first == source) {
        return;
      }
    }
    const auto target = backup_ / source.lexically_relative(root_);
    CheckedBackup(target);
    std::filesystem::create_directories(target.parent_path());
    CheckedBackup(target);
    CheckedPath(source);
    auto record = std::make_pair(source, target);
    moved_.reserve(moved_.size() + 1);
    std::filesystem::rename(source, target);
    moved_.push_back(std::move(record));
  }

  void ExpectOutput(const std::filesystem::path& path) {
    const auto output = CheckedPath(path);
    if (std::filesystem::exists(output)) {
      throw std::runtime_error(
          "Another installed package already uses this filename.");
    }
    outputs_.push_back(output);
  }

  bool TryActivate(const std::filesystem::path& source,
                   const std::filesystem::path& destination,
                   const std::filesystem::path& cache_root) {
    Activation record{CheckedSavedPath(source, cache_root),
                      CheckedPath(destination), NormalizeRoot(cache_root)};
    if (!std::filesystem::is_regular_file(record.source)) {
      throw std::runtime_error("The saved update is not a package file.");
    }
    if (std::filesystem::exists(record.destination)) {
      throw std::runtime_error(
          "Another installed package already uses this filename.");
    }
    std::filesystem::create_directories(record.destination.parent_path());
    CheckedSavedPath(record.source, record.cache_root);
    CheckedPath(record.destination);
    if (std::filesystem::exists(record.destination)) {
      throw std::runtime_error(
          "Another installed package already uses this filename.");
    }
    activated_.reserve(activated_.size() + 1);
    const auto error = RenamePackage(record.source, record.destination);
    if (error == std::errc::cross_device_link
#if XE_PLATFORM_WIN32
        || (error.category() == std::system_category() &&
            error.value() == ERROR_NOT_SAME_DEVICE)
#endif
    ) {
      return false;
    }
    if (error) {
      throw std::filesystem::filesystem_error(
          "Could not activate the saved update", record.source,
          record.destination, error);
    }
    activated_.push_back(std::move(record));
    return true;
  }

  void Commit() { finished_ = true; }

  std::string Restore() noexcept {
    if (finished_) {
      return {};
    }
    std::string error;
    while (!activated_.empty()) {
      const auto& activation = activated_.back();
      try {
        const auto source =
            CheckedSavedPath(activation.source, activation.cache_root);
        const auto destination = CheckedPath(activation.destination);
        if (std::filesystem::exists(source)) {
          throw std::runtime_error(
              "The saved update's original filename is already in use.");
        }
        std::filesystem::create_directories(source.parent_path());
        CheckedSavedPath(source, activation.cache_root);
        CheckedPath(destination);
        const auto rename_error = RenamePackage(destination, source);
        if (rename_error) {
          throw std::filesystem::filesystem_error(
              "Could not restore the saved update", destination, source,
              rename_error);
        }
        activated_.pop_back();
      } catch (const std::exception& e) {
        // Preserve the only copy if it cannot return to saved storage.
        return e.what();
      }
    }
    for (const auto& output : outputs_) {
      try {
        std::filesystem::remove_all(CheckedPath(output));
      } catch (const std::exception& e) {
        error = e.what();
      }
    }
    outputs_.clear();
    for (auto it = moved_.rbegin(); it != moved_.rend(); ++it) {
      try {
        CheckedBackup(it->second);
        if (!std::filesystem::exists(it->second)) {
          continue;
        }
        const auto original = CheckedPath(it->first);
        std::filesystem::rename(it->second, original);
      } catch (const std::exception& e) {
        error = e.what();
      }
    }
    finished_ = error.empty();
    return error;
  }

 private:
  static std::error_code RenamePackage(
      const std::filesystem::path& source,
      const std::filesystem::path& destination) {
#if XE_PLATFORM_WIN32
    // MSVC's filesystem::rename permits copying across volumes.
    if (MoveFileExW(source.c_str(), destination.c_str(), 0)) {
      return {};
    }
    return {static_cast<int>(GetLastError()), std::system_category()};
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    return error;
#endif
  }

  static std::filesystem::path NormalizeRoot(
      const std::filesystem::path& path) {
    if (path.empty()) {
      throw std::runtime_error("Invalid update storage root.");
    }
    auto root = std::filesystem::absolute(path).lexically_normal();
    if (!root.has_filename()) {
      root = root.parent_path();
    }
    if (root == root.root_path()) {
      throw std::runtime_error("Invalid update storage root.");
    }
    return root;
  }

  std::filesystem::path CheckedPath(const std::filesystem::path& path) const {
    CheckComponent(path.filename());
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto parent = absolute.parent_path();
    if (parent != root_ / "000B0000" &&
        parent != root_ / "Headers" / "000B0000") {
      throw std::runtime_error(
          "Update operation is outside the selected title's update folder.");
    }
    CheckDescendants(absolute);
    return absolute;
  }

  std::filesystem::path CheckedSavedPath(
      const std::filesystem::path& path,
      const std::filesystem::path& cache_root) const {
    CheckComponent(path.filename());
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto cache = NormalizeRoot(cache_root);
    if (absolute.parent_path() == cache) {
      CheckDescendants(cache, absolute);
      return absolute;
    }
    const auto folder = absolute.parent_path();
    const auto timestamp = folder.parent_path().filename().string();
    const auto media = folder.parent_path().parent_path().filename().string();
    if (folder.filename() != "000B0000" || timestamp.empty() ||
        timestamp.find_first_not_of("0123456789") != std::string::npos ||
        media.size() != 8 ||
        media.find_first_not_of("0123456789ABCDEF") != std::string::npos ||
        folder.parent_path().parent_path().parent_path() !=
            root_ / "TitleUpdateBackups") {
      throw std::runtime_error(
          "The saved update is outside managed update storage.");
    }
    CheckDescendants(absolute);
    return absolute;
  }

  void CheckedBackup(const std::filesystem::path& path) const {
    const auto relative = path.lexically_relative(backup_);
    if (path != backup_ && (relative.empty() || relative.is_absolute() ||
                            *relative.begin() == "..")) {
      throw std::runtime_error(
          "Update backup is outside the selected transaction's folder.");
    }
    CheckDescendants(path);
  }

  void CheckDescendants(const std::filesystem::path& path) const {
    CheckDescendants(root_, path);
  }

  static void CheckDescendants(const std::filesystem::path& root,
                               const std::filesystem::path& path) {
    // The supplied title root is the storage boundary. UWP can deny access to
    // its ancestors, so inspect only this root and paths inside it.
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) {
      throw std::runtime_error("Invalid update storage path.");
    }
    auto current = root;
    CheckEntry(current, true);
    for (auto it = relative.begin(); it != relative.end(); ++it) {
      const auto& component = *it;
      CheckComponent(component);
      current /= component;
      CheckEntry(current, std::next(it) != relative.end());
    }
  }

  static void CheckComponent(const std::filesystem::path& component) {
    if (component == "." || component == ".." || component.empty()) {
      throw std::runtime_error("Invalid update storage path.");
    }
#if XE_PLATFORM_WIN32
    const auto& name = component.native();
    if (name.find(L':') != std::wstring::npos || name.back() == L'.' ||
        name.back() == L' ') {
      throw std::runtime_error("Invalid update storage filename.");
    }
#endif
  }

  static void CheckEntry(const std::filesystem::path& path,
                         bool require_directory) {
#if XE_PLATFORM_WIN32
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
#if XE_PLATFORM_WINRT
    const auto found = GetFileAttributesExFromAppW(
        path.c_str(), GetFileExInfoStandard, &attributes);
#else
    const auto found =
        GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes);
#endif
    if (!found) {
      const auto error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return;
      }
      throw std::filesystem::filesystem_error(
          "Could not inspect update storage", path,
          std::error_code(error, std::system_category()));
    }
    const bool redirected =
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    const bool directory =
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
      return;
    }
    if (error) {
      throw std::filesystem::filesystem_error(
          "Could not inspect update storage", path, error);
    }
    const bool redirected = std::filesystem::is_symlink(status);
    const bool directory = std::filesystem::is_directory(status);
#endif
    if (redirected || (require_directory && !directory)) {
      throw std::runtime_error(
          "Update storage contains a redirected or invalid directory.");
    }
  }

  std::filesystem::path root_;
  std::filesystem::path backup_;
  struct Activation {
    std::filesystem::path source;
    std::filesystem::path destination;
    std::filesystem::path cache_root;
  };
  std::vector<Activation> activated_;
  std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moved_;
  std::vector<std::filesystem::path> outputs_;
  bool finished_ = false;
};

}  // namespace UWP
#endif  // XENIA_UWP_TITLE_UPDATE_TRANSACTION_H_
