/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_devices/svod_host_file.h"

#include <algorithm>
#include <cstring>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"

namespace xe::vfs {

std::unique_ptr<SvodHostFile> SvodHostFile::Open(
    const std::filesystem::path& path, Storage storage) {
  auto file = std::unique_ptr<SvodHostFile>(new SvodHostFile(path));
  // Game Vault: a fragment read over the network is streamed, never mapped
  if (xe::filesystem::IsNetFolderPath(path)) {
    storage = Storage::kFile;
  }
  if (storage == Storage::kFile) {
    file->file_handle_ = filesystem::FileHandle::OpenExisting(
        path, filesystem::FileAccess::kGenericRead);
    if (!file->file_handle_) {
      XELOGE("Failed to open SVOD fragment {} for reading.", path);
      return nullptr;
    }
    std::error_code error;
    file->size_ = xe::filesystem::FileSizeShared(path, error);
    if (error) {
      XELOGE("Failed to read SVOD fragment size for {}: {}", path,
             error.message());
      return nullptr;
    }
    if (!file->size_) {
      XELOGE("SVOD fragment {} is empty.", path);
      return nullptr;
    }
  } else {
    file->mapping_ = MappedMemory::Open(path, MappedMemory::Mode::kRead);
    if (!file->mapping_) {
      return nullptr;
    }
    file->size_ = file->mapping_->size();
  }
  return file;
}

size_t SvodHostFile::Read(size_t offset, void* buffer, size_t length) const {
  if (offset >= size_ || !length) {
    return 0;
  }
  const size_t bytes_to_read = std::min(length, size_ - offset);
  if (mapping_) {
    std::memcpy(buffer, mapping_->data() + offset, bytes_to_read);
    return bytes_to_read;
  }

  // A fragment is shared by every guest file whose blocks reference it.
  // Serialize positional reads even on hosts with a shared file cursor.
  std::lock_guard lock(read_mutex_);
  size_t bytes_read = 0;
  if (!file_handle_->Read(offset, buffer, bytes_to_read, &bytes_read)) {
    XELOGE("Failed to read SVOD fragment {} at offset 0x{:X} ({} bytes).",
           path_, offset, bytes_to_read);
    return 0;
  }
  if (bytes_read != bytes_to_read) {
    XELOGE("Short read from SVOD fragment {} at offset 0x{:X}: {} of {} bytes.",
           path_, offset, bytes_read, bytes_to_read);
  }
  return bytes_read;
}

}  // namespace xe::vfs
