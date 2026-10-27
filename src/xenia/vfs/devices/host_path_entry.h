/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_HOST_PATH_ENTRY_H_
#define XENIA_VFS_DEVICES_HOST_PATH_ENTRY_H_

#include <atomic>
#include <string>

#include "xenia/base/filesystem.h"
#include "xenia/vfs/entry.h"

namespace xe {
namespace vfs {

class HostPathDevice;

class HostPathEntry : public Entry {
 public:
  HostPathEntry(Device* device, Entry* parent, const std::string_view path,
                const std::filesystem::path& host_path);
  ~HostPathEntry() override;

  static HostPathEntry* Create(Device* device, Entry* parent,
                               const std::filesystem::path& full_path,
                               xe::filesystem::FileInfo file_info);

  const std::filesystem::path& host_path() const { return host_path_; }

  X_STATUS Open(uint32_t desired_access, File** out_file) override;

  // (Game Vault: a file of a network folder game is read, not mapped)
  bool can_map() const override { return !xe::filesystem::IsNetFolderPath(host_path_); }
  std::unique_ptr<MappedMemory> OpenMapped(MappedMemory::Mode mode,
                                           size_t offset,
                                           size_t length) override;
  void update() override;
  bool has_open_files() const override;
  void RetainFile() override { open_files_.fetch_add(1); }
  void ReleaseFile() override;

  bool SetCreateTimestamp(uint64_t timestamp) override;
  bool SetAccessTimestamp(uint64_t timestamp) override;
  bool SetWriteTimestamp(uint64_t timestamp) override;

 private:
  friend class HostPathDevice;

  std::unique_ptr<Entry> CreateEntryInternal(const std::string_view name,
                                             uint32_t attributes) override;
  bool DeleteEntryInternal(Entry* entry) override;
  bool RenameEntryInternal(Entry* new_parent,
                           const std::string_view name) override;

  std::filesystem::path host_path_;
  std::atomic<size_t> open_files_{0};
  // When set, size_ is a synthesized value (e.g. an STFS container size for a
  // collapsed content package) and update() must not overwrite it with the
  // backing file's real size.
  bool reported_size_fixed_ = false;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_HOST_PATH_ENTRY_H_
