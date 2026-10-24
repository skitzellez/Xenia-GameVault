/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_FILESYSTEM_H_
#define XENIA_BASE_FILESYSTEM_H_

#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/base/literals.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"

namespace xe {

using namespace xe::literals;

std::string path_to_utf8(const std::filesystem::path& path);
std::u16string path_to_utf16(const std::filesystem::path& path);
std::filesystem::path to_path(const std::string_view source);
std::filesystem::path to_path(const std::u16string_view source);

namespace filesystem {

// Get executable path.
std::filesystem::path GetExecutablePath();

// Get executable folder.
std::filesystem::path GetExecutableFolder();

// Get user folder.
std::filesystem::path GetUserFolder();

// Creates the parent folder of the specified path if needed.
// This can be used to ensure the destination path for a new file exists before
// attempting to create it.
bool CreateParentFolder(const std::filesystem::path& path);

// Creates folder on specified path.
// If folder already exists it returns success (no error).
std::error_code CreateFolder(const std::filesystem::path& path);

// Creates an empty file at the given path, overwriting if it exists.
bool CreateEmptyFile(const std::filesystem::path& path);

// Opens the file at the given path with the specified mode.
// This behaves like fopen and the returned handle can be used with stdio.
FILE* OpenFile(const std::filesystem::path& path, const std::string_view mode);

// Game Vault (Xenia for Game Vault): a game file the launcher shared with us (its token came with the launch
// link), already open for reading. On an Xbox the console's file broker may never answer for a USB drive, so
// the game is read through this handle instead of being opened by path. Reads only.
void SetSharedFile(const std::filesystem::path& path, void* handle);
// A new read-only handle to that file when path names it, else nullptr. The caller closes it.
void* DuplicateSharedFile(const std::filesystem::path& path);
// True when path names the shared game file or a network game (it then needs no check on the drive).
bool IsGameVaultFile(const std::filesystem::path& path);
// The size of the shared file when path names it; otherwise std::filesystem::file_size(path).
uint64_t FileSizeShared(const std::filesystem::path& path, std::error_code& error);

// Game Vault (Xenia for Game Vault 1.1.7.112 on): a game played straight from a PC's shared folder or a NAS,
// with no copy on the console first. The app reads it over the network itself (apps on the Xbox may not open
// \\PC\share paths) and registers the reader under a stand-in path; FileSizeShared and FileHandle::OpenExisting
// read the game through it. Reads only.
class NetFile {
 public:
  virtual ~NetFile() = default;
  virtual uint64_t size() const = 0;
  // the whole range or false
  virtual bool Read(uint64_t offset, void* buffer, size_t length) = 0;
};
void SetNetFile(const std::filesystem::path& path, std::shared_ptr<NetFile> file);
// the reader when path names the network game, else nullptr
std::shared_ptr<NetFile> GetNetFile(const std::filesystem::path& path);

// Wrapper for the 64-bit version of fseek, returns true on success.
bool Seek(FILE* file, int64_t offset, int origin);

// Wrapper for the 64-bit version of ftell, returns a positive value on success.
int64_t Tell(FILE* file);

// Reduces the size of a stdio file opened for writing. The file pointer is
// clamped. If this returns false, the size of the file and the file pointer are
// undefined.
bool TruncateStdioFile(FILE* file, uint64_t length);

// Copy file from source to destination in defined chunks and optional progress
// tracking.
bool Copy(const std::filesystem::path& source,
          const std::filesystem::path& destination, uint64_t& progress,
          size_t copy_chunk = 5_MiB);

struct FileAccess {
  // Implies kFileReadData.
  static constexpr uint32_t kGenericRead = 0x80000000;
  // Implies kFileWriteData.
  static constexpr uint32_t kGenericWrite = 0x40000000;
  static constexpr uint32_t kGenericExecute = 0x20000000;
  static constexpr uint32_t kGenericAll = 0x10000000;
  static constexpr uint32_t kFileReadData = 0x00000001;
  static constexpr uint32_t kFileWriteData = 0x00000002;
  static constexpr uint32_t kFileAppendData = 0x00000004;
};

class FileHandle {
 public:
  // Opens the file, failing if it doesn't exist.
  // The desired_access bitmask denotes the permissions on the file.
  static std::unique_ptr<FileHandle> OpenExisting(
      const std::filesystem::path& path, uint32_t desired_access);

  virtual ~FileHandle() = default;

  const std::filesystem::path& path() const { return path_; }

  // Reads the requested number of bytes from the file starting at the given
  // offset. The total number of bytes read is returned only if the complete
  // read succeeds.
  virtual bool Read(size_t file_offset, void* buffer, size_t buffer_length,
                    size_t* out_bytes_read) = 0;

  // Writes the given buffer to the file starting at the given offset.
  // The total number of bytes written is returned only if the complete
  // write succeeds.
  virtual bool Write(size_t file_offset, const void* buffer,
                     size_t buffer_length, size_t* out_bytes_written) = 0;

  // Set length of the file in bytes.
  virtual bool SetLength(size_t length) = 0;

  // Flushes any pending write buffers to the underlying filesystem.
  virtual void Flush() = 0;

 protected:
  explicit FileHandle(const std::filesystem::path& path) : path_(path) {}

  std::filesystem::path path_;
};

struct FileInfo {
  enum class Type {
    kFile,
    kDirectory,
  };
  Type type;
  std::filesystem::path name;
  std::filesystem::path path;
  size_t total_size;
  uint64_t create_timestamp;
  uint64_t access_timestamp;
  uint64_t write_timestamp;
};

std::optional<FileInfo> GetInfo(const std::filesystem::path& path);
std::vector<FileInfo> ListFiles(const std::filesystem::path& path);
std::vector<FileInfo> ListDirectories(const std::filesystem::path& path);
std::vector<FileInfo> FilterByName(const std::vector<FileInfo>& files,
                                   const std::regex pattern);
std::vector<FileInfo> FindFileWithName(const std::filesystem::path& path,
                                       std::string_view name,
                                       bool recursive = false);

bool SetAttributes(const std::filesystem::path& path, uint64_t attributes);

#if XE_PLATFORM_ANDROID
void AndroidInitialize();
void AndroidShutdown();
bool IsAndroidContentUri(const std::string_view source);
int OpenAndroidContentFileDescriptor(const std::string_view uri,
                                     const char* mode);
#endif  // XE_PLATFORM_ANDROID

}  // namespace filesystem
}  // namespace xe

#endif  // XENIA_BASE_FILESYSTEM_H_
