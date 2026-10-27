/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/disc_image_file.h"

#include <limits>
#include "xenia/base/logging.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_image_entry.h"
namespace xe {
namespace vfs {

DiscImageFile::DiscImageFile(uint32_t file_access, DiscImageEntry* entry)
    : File(file_access, entry), entry_(entry) {}

DiscImageFile::~DiscImageFile() = default;

void DiscImageFile::Destroy() { delete this; }

X_STATUS DiscImageFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_read) {
  *out_bytes_read = 0;
  if (byte_offset >= entry_->data_size()) {
    return X_STATUS_END_OF_FILE;
  }

  if (entry_->data_offset() >
      std::numeric_limits<size_t>::max() - byte_offset) {
    return X_STATUS_END_OF_FILE;
  }
  const size_t real_offset = entry_->data_offset() + byte_offset;
  const size_t real_length =
      std::min(buffer.size(), entry_->data_size() - byte_offset);
  const auto* device = static_cast<const DiscImageDevice*>(entry_->device());
  if (!device->ReadImageData(real_offset, buffer.data(), real_length,
                             out_bytes_read)) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

}  // namespace vfs
}  // namespace xe
