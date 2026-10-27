// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UWP_CONTENT_EXECUTION_INFO_H_
#define XENIA_UWP_CONTENT_EXECUTION_INFO_H_

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>

#include "xenia/kernel/xam/xcontent/xcontent.h"

namespace UWP {
inline std::optional<xe::xex2_opt_execution_info> ReadContentExecutionInfo(
    const std::filesystem::path& path) {
  using namespace xe::kernel::xam;
  std::ifstream file(path, std::ios::binary);
  XContentHeader header{};
  if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
      !header.is_magic_valid()) {
    return std::nullopt;
  }
  file.seekg(offsetof(XContentContainerHeader, content_metadata) +
             offsetof(XContentMetadata, execution_info));
  xe::xex2_opt_execution_info info{};
  if (!file.read(reinterpret_cast<char*>(&info), sizeof(info))) {
    return std::nullopt;
  }
  return info;
}
}  // namespace UWP
#endif  // XENIA_UWP_CONTENT_EXECUTION_INFO_H_
