// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UWP_TITLE_UPDATE_IDENTITY_H_
#define XENIA_UWP_TITLE_UPDATE_IDENTITY_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace UWP {

struct TitleUpdateIdentity {
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint32_t version = 0;
  uint32_t base_version = 0;
  uint64_t profile_id = 0;
  bool has_fragments = false;
  std::string content_id;
  std::string key;
};

// Identity only: the installer still validates the package and its payload.
inline std::optional<TitleUpdateIdentity> ReadTitleUpdateIdentity(
    const std::filesystem::path& path) {
  auto header_path = path;
  std::error_code error;
  if (std::filesystem::is_directory(path, error)) {
    auto package = path.lexically_normal();
    if (package.filename().empty()) package = package.parent_path();
    auto name = package.filename();
    if (name.empty()) return std::nullopt;
    name += ".header";
    header_path = package.parent_path().parent_path() / "Headers" /
                  "000B0000" / name;
  }
  if (error || !std::filesystem::is_regular_file(header_path, error) || error) {
    return std::nullopt;
  }

  // Shared CON/LIVE/PIRS layout: XContentHeader followed by XContentMetadata.
  constexpr size_t kContentIdOffset = 0x32C;
  constexpr size_t kExecutionOffset = 0x354;
  constexpr size_t kExecutionSize = 0x18;
  std::array<uint8_t, 0x3AD> header{};
  std::ifstream input(header_path, std::ios::binary);
  if (!input.read(reinterpret_cast<char*>(header.data()), header.size())) {
    return std::nullopt;
  }
  const auto read32 = [&](size_t offset) {
    return uint32_t(header[offset]) << 24 |
           uint32_t(header[offset + 1]) << 16 |
           uint32_t(header[offset + 2]) << 8 | header[offset + 3];
  };
  const auto magic = read32(0);
  const auto volume_type = read32(0x3A9);
  if ((magic != 0x434F4E20 && magic != 0x4C495645 && magic != 0x50495253) ||
      read32(0x344) != 0x000B0000 || volume_type > 1) {
    return std::nullopt;
  }

  TitleUpdateIdentity identity;
  identity.media_id = read32(kExecutionOffset);
  identity.version = read32(kExecutionOffset + 4);
  identity.base_version = read32(kExecutionOffset + 8);
  identity.title_id = read32(kExecutionOffset + 12);
  identity.profile_id = uint64_t(read32(0x371)) << 32 | read32(0x375);
  identity.has_fragments = volume_type == 1;

  bool has_content_id = false;
  for (size_t i = 0; i < 20; ++i) {
    has_content_id |= header[kContentIdOffset + i] != 0;
  }
  // An absent content ID cannot establish that two packages contain the same TU.
  if (has_content_id) {
    constexpr char hex[] = "0123456789abcdef";
    constexpr char upper_hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < 20; ++i) {
      const auto value = header[kContentIdOffset + i];
      identity.content_id += upper_hex[value >> 4];
      identity.content_id += upper_hex[value & 15];
    }
    identity.key = "tu:";
    const auto append_hex = [&](size_t offset, size_t count) {
      for (size_t i = 0; i < count; ++i) {
        const auto value = header[offset + i];
        identity.key += hex[value >> 4];
        identity.key += hex[value & 15];
      }
    };
    append_hex(kContentIdOffset, 20);
    identity.key += ':';
    append_hex(kExecutionOffset, kExecutionSize);
  }
  return identity;
}

}  // namespace UWP
#endif  // XENIA_UWP_TITLE_UPDATE_IDENTITY_H_
