/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/host_path_entry.h"

#include <algorithm>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/base/utf8.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_file.h"

namespace xe {
namespace vfs {

HostPathEntry::HostPathEntry(Device* device, Entry* parent,
                             const std::string_view path,
                             const std::filesystem::path& host_path)
    : Entry(device, parent, path), host_path_(host_path) {}

HostPathEntry::~HostPathEntry() = default;

bool HostPathEntry::has_open_files() const {
  auto global_lock = global_critical_region_.Acquire();
  // Deleting a directory frees its children, so their handles count too.
  return open_files_.load() != 0 ||
         std::ranges::any_of(children_, [](const auto& child) {
           return child->has_open_files();
         });
}

void HostPathEntry::ReleaseFile() {
  auto global_lock = global_critical_region_.Acquire();
  if (open_files_.fetch_sub(1) == 1 && delete_on_close() && parent()) {
    Delete();
  }
}

// Game Vault: where a file of a write-layer device lives in the layer (empty: no layer)
static std::filesystem::path LayerPathOf(Device* device,
                                         const std::string& guest_path) {
  auto* host = dynamic_cast<HostPathDevice*>(device);
  if (!host || host->write_layer().empty()) return {};
  return host->write_layer() /
         xe::to_path(xe::utf8::fix_path_separators(guest_path));
}

HostPathEntry* HostPathEntry::Create(Device* device, Entry* parent,
                                     const std::filesystem::path& full_path,
                                     xe::filesystem::FileInfo file_info) {
  auto path = xe::utf8::join_guest_paths(parent->path(),
                                         xe::path_to_utf8(file_info.name));
  // (a file the game wrote before is taken from the write layer)
  std::filesystem::path layer = LayerPathOf(device, path);
  std::error_code layer_ec;
  if (!layer.empty() &&
      file_info.type == xe::filesystem::FileInfo::Type::kFile &&
      std::filesystem::is_regular_file(layer, layer_ec)) {
    if (auto info = xe::filesystem::GetInfo(layer)) {
      file_info.total_size = info->total_size;
      file_info.write_timestamp = info->write_timestamp;
      auto entry = new HostPathEntry(device, parent, path, layer);
      entry->create_timestamp_ = file_info.create_timestamp;
      entry->access_timestamp_ = file_info.access_timestamp;
      entry->write_timestamp_ = file_info.write_timestamp;
      entry->attributes_ = kFileAttributeNormal;
      entry->size_ = file_info.total_size;
      entry->allocation_size_ =
          xe::round_up(file_info.total_size, device->bytes_per_sector());
      return entry;
    }
  }
  auto entry = new HostPathEntry(device, parent, path, full_path);

  entry->create_timestamp_ = file_info.create_timestamp;
  entry->access_timestamp_ = file_info.access_timestamp;
  entry->write_timestamp_ = file_info.write_timestamp;
  if (file_info.type == xe::filesystem::FileInfo::Type::kDirectory) {
    entry->attributes_ = kFileAttributeDirectory;
  } else {
    entry->attributes_ = kFileAttributeNormal;
    if (device->is_read_only()) {
      entry->attributes_ |= kFileAttributeReadOnly;
    }
    entry->size_ = file_info.total_size;
    entry->allocation_size_ =
        xe::round_up(file_info.total_size, device->bytes_per_sector());
  }
  return entry;
}

X_STATUS HostPathEntry::Open(uint32_t desired_access, File** out_file) {
  if (is_read_only() && (desired_access & (FileAccess::kFileWriteData |
                                           FileAccess::kFileAppendData))) {
    XELOGE("Attempting to open file for write access on read-only device");
    return X_STATUS_ACCESS_DENIED;
  }
  // Game Vault: a write to a game file not in the write layer yet: copy it there first, write the copy
  if (desired_access & (FileAccess::kFileWriteData | FileAccess::kFileAppendData)) {
    std::filesystem::path layer = LayerPathOf(device_, path());
    if (!layer.empty() && host_path_ != layer) {
      std::error_code ec;
      ec = xe::filesystem::CreateFolder(layer.parent_path());
      std::filesystem::copy_file(host_path_, layer,
                                 std::filesystem::copy_options::overwrite_existing,
                                 ec);
      if (ec) {
        XELOGE("Game Vault: write layer: could not copy {}: {}", path(),
               ec.message());
        return X_STATUS_ACCESS_DENIED;
      }
      XELOGI("Game Vault: write layer: {} is written in the console's copy",
             path());
      host_path_ = layer;
    }
  }
  auto file_handle =
      xe::filesystem::FileHandle::OpenExisting(host_path_, desired_access);
  if (!file_handle) {
    // TODO(benvanik): pick correct response.
    return X_STATUS_NO_SUCH_FILE;
  }
  *out_file = new HostPathFile(desired_access, this, std::move(file_handle));
  return X_STATUS_SUCCESS;
}

std::unique_ptr<MappedMemory> HostPathEntry::OpenMapped(MappedMemory::Mode mode,
                                                        size_t offset,
                                                        size_t length) {
  return MappedMemory::Open(host_path_, mode, offset, length);
}

std::unique_ptr<Entry> HostPathEntry::CreateEntryInternal(
    const std::string_view name, uint32_t attributes) {
  auto full_path = host_path_ / xe::to_path(name);
  // Game Vault: a write-layer device makes new files and folders in the layer
  std::filesystem::path layer =
      LayerPathOf(device_, xe::utf8::join_guest_paths(path(), name));
  if (!layer.empty()) {
    std::error_code ec;
    ec = xe::filesystem::CreateFolder(
        (attributes & kFileAttributeDirectory) ? layer : layer.parent_path());
    full_path = layer;
  }
  if (attributes & kFileAttributeDirectory) {
    if (xe::filesystem::CreateFolder(full_path)) {
      return nullptr;
    }
  } else {
    auto file = xe::filesystem::OpenFile(full_path, "wb");
    if (!file) {
      return nullptr;
    }
    fclose(file);
  }
  auto file_info = xe::filesystem::GetInfo(full_path);
  if (!file_info) {
    return nullptr;
  }
  return std::unique_ptr<Entry>(
      HostPathEntry::Create(device_, this, full_path, file_info.value()));
}

bool HostPathEntry::DeleteEntryInternal(Entry* entry) {
  // Game Vault: a write-layer device never deletes the game's own files: only the layer's copy (an overwrite
  // is delete + create, and the new file goes to the layer)
  std::filesystem::path layer = LayerPathOf(
      device_, xe::utf8::join_guest_paths(path(), entry->name()));
  if (!layer.empty()) {
    std::error_code ec;
    if (entry->attributes() & kFileAttributeDirectory) {
      std::filesystem::remove_all(layer, ec);
    } else {
      std::filesystem::remove(layer, ec);
    }
    return true;
  }
  auto full_path = host_path_ / xe::to_path(entry->name());
  std::error_code ec;  // avoid exception on remove/remove_all failure
  if (entry->attributes() & kFileAttributeDirectory) {
    // Delete entire directory and contents.
    auto removed = std::filesystem::remove_all(full_path, ec);
    return removed >= 1 && removed != static_cast<std::uintmax_t>(-1);
  } else {
    // Skip directories, they we're handled above.
    if (std::filesystem::is_directory(full_path)) {
      return false;
    }

    if (std::filesystem::exists(full_path)) {
      const auto result = std::filesystem::remove(full_path, ec);
      if (ec) {
        XELOGE("{}: Cannot remove file entry. File: {} Error: {}", __func__,
               xe::strip_pii(xe::path_to_utf8(full_path)), ec.message());
        return false;
      }
    }
    return true;
  }
}

bool HostPathEntry::RenameEntryInternal(Entry* new_parent,
                                        const std::string_view name) {
  const std::filesystem::path new_host_path =
      static_cast<HostPathEntry*>(new_parent)->host_path_ / xe::to_path(name);

  std::error_code ec;
  std::filesystem::rename(host_path_, new_host_path, ec);
  if (ec) {
    XELOGE("RenameEntryInternal: Failed to rename '{}' to '{}': {}",
           xe::strip_pii(xe::path_to_utf8(host_path_)),
           xe::strip_pii(xe::path_to_utf8(new_host_path)), ec.message());
    return false;
  }
  host_path_ = new_host_path;
  return true;
}

void HostPathEntry::update() {
  auto file_info = xe::filesystem::GetInfo(host_path_);
  if (!file_info) {
    return;
  }
  // Creation time never changes, and posix reports st_ctime here, which
  // moves on every write.
  access_timestamp_ = file_info->access_timestamp;
  write_timestamp_ = file_info->write_timestamp;
  // A synthesized size, such as a collapsed package container, must stand.
  if (file_info->type == xe::filesystem::FileInfo::Type::kFile &&
      !reported_size_fixed_) {
    size_ = file_info->total_size;
    allocation_size_ =
        xe::round_up(file_info->total_size, device()->bytes_per_sector());
  }
}

bool HostPathEntry::SetCreateTimestamp(uint64_t timestamp) {
  if (device_->is_read_only()) {
    XELOGW(
        "{} - Tried to change read-only creation timestamp for file: {} to: {}",
        __FUNCTION__, name_, timestamp);
    return false;
  }
  XELOGI("{} - Tried to change creation timestamp for file: {} to: {}",
         __FUNCTION__, name_, timestamp);
  return true;
}

bool HostPathEntry::SetAccessTimestamp(uint64_t timestamp) {
  if (device_->is_read_only()) {
    XELOGW(
        "{} - Tried to change read-only access timestamp for file: {} to: {}",
        __FUNCTION__, name_, timestamp);
    return false;
  }
  XELOGI("{} - Tried to change access timestamp for file: {} to: {}",
         __FUNCTION__, name_, timestamp);
  return true;
}

bool HostPathEntry::SetWriteTimestamp(uint64_t timestamp) {
  if (device_->is_read_only()) {
    XELOGW("{} - Tried to change read-only write timestamp for file: {} to: {}",
           __FUNCTION__, name_, timestamp);
    return false;
  }
  XELOGI("{} - Tried to change write timestamp for file: {} to: {}",
         __FUNCTION__, name_, timestamp);
  return true;
}

}  // namespace vfs
}  // namespace xe
