/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <cstring>
#include <vector>

#include "xenia/vfs/devices/disc_image_device.h"

#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/vfs/devices/disc_image_entry.h"

namespace xe {
namespace vfs {

using namespace xe::literals;

constexpr size_t kXESectorSize = 2_KiB;

DiscImageDevice::DiscImageDevice(const std::string_view mount_path,
                                 const std::filesystem::path& host_path)
    : Device(mount_path), name_("GDFX"), host_path_(host_path) {}

DiscImageDevice::~DiscImageDevice() = default;

bool DiscImageDevice::Initialize() {
#if XE_PLATFORM_WINRT
  file_handle_ = filesystem::FileHandle::OpenExisting(
      host_path_, filesystem::FileAccess::kGenericRead);
  if (!file_handle_) {
    XELOGE("Disc image could not be opened");
    return false;
  }
  std::error_code size_error;
  image_size_ = filesystem::FileSizeShared(host_path_, size_error);
  if (size_error) {
    XELOGE("Disc image size could not be read: {}", size_error.message());
    return false;
  }
  XELOGFS("DiscImageDevice::Initialize");
#else
  mmap_ = MappedMemory::Open(host_path_, MappedMemory::Mode::kRead);
  if (!mmap_) {
    XELOGE("Disc image could not be mapped");
    return false;
  } else {
    XELOGFS("DiscImageDevice::Initialize");
  }
  image_size_ = mmap_->size();
#endif

  ParseState state = {0};
#if !XE_PLATFORM_WINRT
  state.ptr = mmap_->data();
#endif
  state.size = image_size_;
  auto result = Verify(&state);
  if (result != Error::kSuccess) {
    XELOGE("Failed to verify disc image header: {}",
           static_cast<int32_t>(result));
    return false;
  }

#if XE_PLATFORM_WINRT
  std::vector<uint8_t> root_buffer(state.root_size);
  size_t root_bytes_read = 0;
  if (!ReadImageData(state.root_offset, root_buffer.data(), root_buffer.size(),
                     &root_bytes_read) ||
      root_bytes_read != root_buffer.size()) {
    XELOGE("Failed to read root directory from disc image");
    return false;
  }
  result = ReadAllEntries(&state, root_buffer.data());
#else
  result = ReadAllEntries(&state, state.ptr + state.root_offset);
#endif
  if (result != Error::kSuccess) {
    XELOGE("Failed to read all GDFX entries: {}", static_cast<int32_t>(result));
    return false;
  }

  return true;
}

bool DiscImageDevice::ReadImageData(size_t file_offset, void* buffer,
                                    size_t buffer_length,
                                    size_t* out_bytes_read) const {
  if (out_bytes_read) {
    *out_bytes_read = 0;
  }
  if (file_offset > image_size_ || buffer_length > image_size_ - file_offset) {
    return false;
  }
#if XE_PLATFORM_WINRT
  if (!file_handle_) {
    return false;
  }
  size_t bytes_read = 0;
  if (!file_handle_->Read(file_offset, buffer, buffer_length, &bytes_read) ||
      bytes_read != buffer_length) {
    return false;
  }
  if (out_bytes_read) {
    *out_bytes_read = bytes_read;
  }
  return true;
#else
  if (!mmap_ || file_offset > mmap_->size() ||
      buffer_length > mmap_->size() - file_offset) {
    return false;
  }
  std::memcpy(buffer, mmap_->data() + file_offset, buffer_length);
  if (out_bytes_read) {
    *out_bytes_read = buffer_length;
  }
  return true;
#endif
}

void DiscImageDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* DiscImageDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  XELOGFS("DiscImageDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

DiscImageDevice::Error DiscImageDevice::Verify(ParseState* state) {
  // Find sector 32 of the game partition - try at a few points.
  static constexpr size_t likely_offsets[] = {
      0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000,
  };
  bool magic_found = false;
  for (size_t n = 0; n < xe::countof(likely_offsets); n++) {
    state->game_offset = likely_offsets[n];
    if (VerifyMagic(state, state->game_offset + (32 * kXESectorSize))) {
      magic_found = true;
      break;
    }
  }
  if (!magic_found) {
    // File doesn't have the magic values - likely not a real GDFX source.
    return Error::kErrorFileMismatch;
  }

  // Read sector 32 to get FS state.
  if (state->size < state->game_offset + (32 * kXESectorSize)) {
    return Error::kErrorReadError;
  }
  std::array<uint8_t, kXESectorSize> fs_sector;
  size_t fs_bytes_read = 0;
  if (!ReadImageData(state->game_offset + (32 * kXESectorSize),
                     fs_sector.data(), fs_sector.size(), &fs_bytes_read) ||
      fs_bytes_read != fs_sector.size()) {
    return Error::kErrorReadError;
  }
  uint8_t* fs_ptr = fs_sector.data();
  state->root_sector = xe::load<uint32_t>(fs_ptr + 20);
  state->root_size = xe::load<uint32_t>(fs_ptr + 24);
  state->root_offset =
      state->game_offset + (state->root_sector * kXESectorSize);
  if (state->root_size < 14 || state->root_size > 32_MiB ||
      state->root_offset > state->size ||
      state->root_size > state->size - state->root_offset) {
    return Error::kErrorDamagedFile;
  }

  return Error::kSuccess;
}

bool DiscImageDevice::VerifyMagic(ParseState* state, size_t offset) {
  if (offset >= state->size) {
    return false;
  }

  // Simple check to see if the given offset contains the magic value.
  uint8_t magic[20];
  size_t magic_bytes_read = 0;
  return ReadImageData(offset, magic, sizeof(magic), &magic_bytes_read) &&
         magic_bytes_read == sizeof(magic) &&
         std::memcmp(magic, "MICROSOFT*XBOX*MEDIA", sizeof(magic)) == 0;
}

DiscImageDevice::Error DiscImageDevice::ReadAllEntries(
    ParseState* state, const uint8_t* root_buffer) {
  auto root_entry = new DiscImageEntry(this, nullptr, "", mmap_.get());
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  if (!ReadEntry(state, root_buffer, 0, root_entry)) {
    return Error::kErrorOutOfMemory;
  }

  return Error::kSuccess;
}

bool DiscImageDevice::ReadEntry(ParseState* state, const uint8_t* buffer,
                                uint16_t entry_ordinal,
                                DiscImageEntry* parent) {
  const uint8_t* p = buffer + (entry_ordinal * 4);

  uint16_t node_l = xe::load<uint16_t>(p + 0);
  uint16_t node_r = xe::load<uint16_t>(p + 2);
  size_t sector = xe::load<uint32_t>(p + 4);
  size_t length = xe::load<uint32_t>(p + 8);
  uint8_t attributes = xe::load<uint8_t>(p + 12);
  uint8_t name_length = xe::load<uint8_t>(p + 13);
  auto name_buffer = reinterpret_cast<const char*>(p + 14);

  if (node_l && !ReadEntry(state, buffer, node_l, parent)) {
    return false;
  }

  // Filename is stored as Windows-1252, convert it to UTF-8.
  auto ansi_name = std::string(name_buffer, name_length);
  auto name = xe::win1252_to_utf8(ansi_name);
  // Fallback to normal name if for whatever reason conversion from 1252 code
  // page failed.
  if (name.empty()) {
    name = ansi_name;
  }

  auto entry = DiscImageEntry::Create(this, parent, name, mmap_.get());
  entry->attributes_ = attributes | kFileAttributeReadOnly;
  entry->size_ = length;
  entry->allocation_size_ = xe::round_up(length, bytes_per_sector());

  // Set to January 1, 1970 (UTC) in 100-nanosecond intervals
  entry->create_timestamp_ = 10000 * 11644473600000LL;
  entry->access_timestamp_ = 10000 * 11644473600000LL;
  entry->write_timestamp_ = 10000 * 11644473600000LL;

  if (attributes & kFileAttributeDirectory) {
    // Folder.
    entry->data_offset_ = 0;
    entry->data_size_ = 0;
    if (length) {
      // Not a leaf - read in children.
      if (state->size < state->game_offset + (sector * kXESectorSize)) {
        // Out of bounds read.
        return false;
      }
#if XE_PLATFORM_WINRT
      std::vector<uint8_t> folder_buffer(length);
      size_t folder_bytes_read = 0;
      if (!ReadImageData(state->game_offset + (sector * kXESectorSize),
                         folder_buffer.data(), folder_buffer.size(),
                         &folder_bytes_read) ||
          folder_bytes_read != folder_buffer.size()) {
        return false;
      }
      if (!ReadEntry(state, folder_buffer.data(), 0, entry.get())) {
        return false;
      }
#else
      // Read child list.
      uint8_t* folder_ptr =
          state->ptr + state->game_offset + (sector * kXESectorSize);
      if (!ReadEntry(state, folder_ptr, 0, entry.get())) {
        return false;
      }
#endif
    }
  } else {
    // File.
    entry->data_offset_ = state->game_offset + (sector * kXESectorSize);
    entry->data_size_ = length;
  }

  // Add to parent.
  parent->children_.emplace_back(std::move(entry));

  // Read next file in the list.
  if (node_r && !ReadEntry(state, buffer, node_r, parent)) {
    return false;
  }

  return true;
}

}  // namespace vfs
}  // namespace xe
