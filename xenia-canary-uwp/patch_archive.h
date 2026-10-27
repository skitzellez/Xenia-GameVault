// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UWP_PATCH_ARCHIVE_H_
#define XENIA_UWP_PATCH_ARCHIVE_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "third_party/zlib-ng/zlib-ng.h"

namespace UWP {

class PatchArchiveWorkspace {
 public:
  explicit PatchArchiveWorkspace(const std::filesystem::path& parent) {
    static std::atomic<uint64_t> sequence{0};
    const auto root = std::filesystem::absolute(parent).lexically_normal();
    std::filesystem::create_directories(root);
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
      path_ =
          root /
          (".xenia-patch-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(sequence++));
      if (std::filesystem::create_directory(path_)) {
        return;
      }
    }
    throw std::runtime_error("Could not create the patch staging folder.");
  }
  ~PatchArchiveWorkspace() {
    if (!keep_) {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }
  }
  PatchArchiveWorkspace(const PatchArchiveWorkspace&) = delete;
  PatchArchiveWorkspace& operator=(const PatchArchiveWorkspace&) = delete;
  const std::filesystem::path& path() const { return path_; }
  void Keep() { keep_ = true; }

 private:
  std::filesystem::path path_;
  bool keep_ = false;
};

namespace patch_archive {
inline uint16_t U16(const uint8_t* data) {
  return uint16_t(data[0]) | (uint16_t(data[1]) << 8);
}
inline uint32_t U32(const uint8_t* data) {
  return uint32_t(U16(data)) | (uint32_t(U16(data + 2)) << 16);
}
inline void Require(bool value, const char* message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

class Reader {
 public:
  explicit Reader(const std::filesystem::path& path)
      : file_(path, std::ios::binary), size_(std::filesystem::file_size(path)) {
    Require(bool(file_), "Could not open the downloaded patch archive.");
  }
  void Read(uint64_t offset, std::span<uint8_t> output) {
    Require(offset <= size_ && output.size() <= size_ - offset,
            "The patch archive is truncated.");
    file_.seekg(static_cast<std::streamoff>(offset));
    Require(
        bool(file_.read(reinterpret_cast<char*>(output.data()), output.size())),
        "Could not read the patch archive.");
  }
  uint64_t size() const { return size_; }

 private:
  std::ifstream file_;
  uint64_t size_;
};

struct Entry {
  std::string name;
  std::filesystem::path filename;
  uint16_t flags;
  uint16_t method;
  uint32_t crc;
  uint32_t compressed;
  uint32_t uncompressed;
  uint32_t local_offset;
};

inline std::filesystem::path ValidateName(std::string name) {
  Require(!name.empty() && name.size() <= 4096 &&
              name.find('\0') == std::string::npos &&
              name.find_first_of(":*?\"<>|") == std::string::npos,
          "The patch archive contains an invalid path.");
  std::replace(name.begin(), name.end(), '\\', '/');
  Require(name.front() != '/', "The patch archive contains an absolute path.");
  size_t begin = 0;
  std::string filename;
  while (begin < name.size()) {
    const auto end = name.find('/', begin);
    const auto component = name.substr(begin, end - begin);
    Require(!component.empty() && component != "." && component != ".." &&
                component.back() != '.' && component.back() != ' ' &&
                std::none_of(component.begin(), component.end(),
                             [](unsigned char c) { return c < 32; }),
            "The patch archive contains an unsafe path.");
    auto stem = component.substr(0, component.find('.'));
    for (char& c : stem) {
      if (c >= 'a' && c <= 'z') {
        c -= 'a' - 'A';
      }
    }
    Require(stem != "CON" && stem != "PRN" && stem != "AUX" && stem != "NUL" &&
                !(stem.size() == 4 &&
                  (stem.starts_with("COM") || stem.starts_with("LPT")) &&
                  stem[3] >= '1' && stem[3] <= '9'),
            "The patch archive contains a reserved filename.");
    filename = component;
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return std::filesystem::u8path(filename);
}

inline void ExtractEntry(Reader& archive, const Entry& entry,
                         uint64_t directory_offset,
                         const std::filesystem::path& destination) {
  std::array<uint8_t, 30> local;
  archive.Read(entry.local_offset, local);
  Require(U32(local.data()) == 0x04034b50 &&
              U16(local.data() + 6) == entry.flags &&
              U16(local.data() + 8) == entry.method,
          "The patch archive has inconsistent entry headers.");
  const auto name_length = U16(local.data() + 26);
  const auto extra_length = U16(local.data() + 28);
  std::vector<uint8_t> name(name_length);
  archive.Read(uint64_t(entry.local_offset) + 30, name);
  Require(std::string(name.begin(), name.end()) == entry.name,
          "The patch archive has inconsistent filenames.");
  const uint64_t data_offset =
      uint64_t(entry.local_offset) + 30 + name_length + extra_length;
  Require(data_offset <= directory_offset &&
              entry.compressed <= directory_offset - data_offset,
          "The patch archive contains an invalid entry size.");
  if (!(entry.flags & 8)) {
    Require(U32(local.data() + 14) == entry.crc &&
                U32(local.data() + 18) == entry.compressed &&
                U32(local.data() + 22) == entry.uncompressed,
            "The patch archive has inconsistent entry sizes or checksums.");
  } else {
    const uint64_t descriptor_offset = data_offset + entry.compressed;
    Require(descriptor_offset + 12 <= directory_offset,
            "The patch archive is missing a data descriptor.");
    std::array<uint8_t, 16> descriptor{};
    archive.Read(descriptor_offset, std::span(descriptor.data(), 12));
    bool valid = U32(descriptor.data()) == entry.crc &&
                 U32(descriptor.data() + 4) == entry.compressed &&
                 U32(descriptor.data() + 8) == entry.uncompressed;
    if (!valid && U32(descriptor.data()) == 0x08074b50 &&
        descriptor_offset + 16 <= directory_offset) {
      archive.Read(descriptor_offset, descriptor);
      valid = U32(descriptor.data() + 4) == entry.crc &&
              U32(descriptor.data() + 8) == entry.compressed &&
              U32(descriptor.data() + 12) == entry.uncompressed;
    }
    Require(valid, "The patch archive has an invalid data descriptor.");
  }
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  Require(bool(output), "Could not create a staged patch file.");
  std::array<uint8_t, 64 * 1024> input_buffer, output_buffer;
  uint64_t written = 0;
  uint32_t crc = 0;
  auto write = [&](std::span<const uint8_t> data) {
    Require(data.size() <= entry.uncompressed - written,
            "A patch entry expands beyond its declared size.");
    Require(bool(output.write(reinterpret_cast<const char*>(data.data()),
                              data.size())),
            "Could not write the patch file. Check available storage space.");
    crc = zng_crc32(crc, data.data(), static_cast<uint32_t>(data.size()));
    written += data.size();
  };
  uint64_t consumed = 0;
  if (entry.method == 0) {
    Require(entry.compressed == entry.uncompressed,
            "A stored patch entry has inconsistent sizes.");
    while (consumed < entry.compressed) {
      const size_t size =
          std::min<uint64_t>(input_buffer.size(), entry.compressed - consumed);
      archive.Read(data_offset + consumed,
                   std::span(input_buffer.data(), size));
      write(std::span(input_buffer.data(), size));
      consumed += size;
    }
  } else {
    struct Inflater {
      zng_stream stream{};
      Inflater() {
        Require(zng_inflateInit2(&stream, -MAX_WBITS) == Z_OK,
                "Could not initialize patch decompression.");
      }
      ~Inflater() { zng_inflateEnd(&stream); }
    } inflater;
    auto& stream = inflater.stream;
    for (;;) {
      if (!stream.avail_in && consumed < entry.compressed) {
        const size_t size = std::min<uint64_t>(input_buffer.size(),
                                               entry.compressed - consumed);
        archive.Read(data_offset + consumed,
                     std::span(input_buffer.data(), size));
        stream.next_in = input_buffer.data();
        stream.avail_in = static_cast<uint32_t>(size);
        consumed += size;
      }
      stream.next_out = output_buffer.data();
      stream.avail_out = static_cast<uint32_t>(output_buffer.size());
      const auto input_before = stream.avail_in;
      const int result = zng_inflate(&stream, Z_NO_FLUSH);
      const auto produced = output_buffer.size() - stream.avail_out;
      write(std::span(output_buffer.data(), produced));
      if (result == Z_STREAM_END) {
        Require(consumed == entry.compressed && !stream.avail_in,
                "A compressed patch entry contains trailing data.");
        break;
      }
      Require(result == Z_OK && (produced || stream.avail_in < input_before),
              "A compressed patch entry is corrupt or truncated.");
    }
  }
  Require(written == entry.uncompressed && crc == entry.crc,
          "A patch entry failed its size or checksum check.");
  output.flush();
  Require(bool(output), "Could not flush the staged patch file.");
  output.close();
  Require(bool(output), "Could not finish writing the staged patch file.");
}
}  // namespace patch_archive

inline size_t ExtractPatchArchive(const std::filesystem::path& archive_path,
                                  const std::filesystem::path& destination) {
  using namespace patch_archive;
  Reader archive(archive_path);
  Require(archive.size() >= 22, "The patch archive is truncated.");
  std::vector<uint8_t> tail(std::min<uint64_t>(archive.size(), 65557));
  archive.Read(archive.size() - tail.size(), tail);
  size_t end = tail.size() - 22;
  for (;;) {
    if (U32(tail.data() + end) == 0x06054b50 &&
        end + 22 + U16(tail.data() + end + 20) == tail.size()) {
      break;
    }
    Require(end != 0, "The patch archive has no valid central directory.");
    --end;
  }
  const auto* footer = tail.data() + end;
  const uint32_t directory_size = U32(footer + 12);
  const uint32_t directory_offset = U32(footer + 16);
  const uint16_t entries = U16(footer + 10);
  Require(!U16(footer + 4) && !U16(footer + 6) && U16(footer + 8) == entries,
          "Multi-volume patch archives are not supported.");
  Require(entries != 0xFFFF && directory_size != UINT32_MAX &&
              directory_offset != UINT32_MAX,
          "ZIP64 patch archives are not supported.");
  Require(uint64_t(directory_offset) + directory_size <=
              archive.size() - tail.size() + end,
          "The patch archive has an invalid central directory.");
  PatchArchiveWorkspace workspace(destination);
  const auto staged = workspace.path() / "files";
  const auto backup = workspace.path() / "backup";
  std::filesystem::create_directory(staged);
  std::filesystem::create_directory(backup);
  std::vector<std::filesystem::path> files;
  std::set<std::filesystem::path> names;
  uint64_t cursor = directory_offset;
  const uint64_t directory_end = cursor + directory_size;
  for (uint32_t i = 0; i < entries; ++i) {
    Require(cursor + 46 <= directory_end,
            "The patch central directory is truncated.");
    std::array<uint8_t, 46> central;
    archive.Read(cursor, central);
    Require(U32(central.data()) == 0x02014b50,
            "The patch central directory is corrupt.");
    const auto name_length = U16(central.data() + 28);
    const uint64_t next = cursor + 46 + name_length + U16(central.data() + 30) +
                          U16(central.data() + 32);
    Require(next <= directory_end, "The patch central directory is truncated.");
    std::vector<uint8_t> name(name_length);
    archive.Read(cursor + 46, name);
    Entry entry{std::string(name.begin(), name.end()),
                {},
                U16(central.data() + 8),
                U16(central.data() + 10),
                U32(central.data() + 16),
                U32(central.data() + 20),
                U32(central.data() + 24),
                U32(central.data() + 42)};
    entry.filename = ValidateName(entry.name);
    Require(!U16(central.data() + 34) && entry.compressed != UINT32_MAX &&
                entry.uncompressed != UINT32_MAX &&
                entry.local_offset != UINT32_MAX,
            "ZIP64 or multi-volume patch entries are not supported.");
    Require((U32(central.data() + 38) >> 16 & 0170000) != 0120000,
            "Symbolic links are not allowed in patch archives.");
    cursor = next;
    if (entry.name.back() == '/' || entry.name.back() == '\\') {
      continue;
    }
    auto extension = entry.filename.extension().string();
    for (char& c : extension) {
      if (c >= 'A' && c <= 'Z') {
        c += 'a' - 'A';
      }
    }
    if (extension != ".toml") {
      continue;
    }
    Require(
        !(entry.flags & ~0x080Eu) && (entry.method == 0 || entry.method == 8),
        "A patch entry uses encryption or unsupported compression.");
    auto key = entry.filename.native();
    for (auto& c : key) {
      if (c >= 'A' && c <= 'Z') {
        c += 'a' - 'A';
      }
    }
    Require(names.emplace(key).second &&
                !std::filesystem::exists(staged / entry.filename),
            "The patch archive contains duplicate patch filenames.");
    ExtractEntry(archive, entry, directory_offset, staged / entry.filename);
    files.push_back(entry.filename);
  }
  Require(cursor == directory_end && !files.empty(),
          "The archive contains no patch files or an invalid directory.");
  const auto target = std::filesystem::absolute(destination).lexically_normal();
  for (const auto& file : files) {
    const auto status = std::filesystem::symlink_status(target / file);
    Require(!std::filesystem::exists(status) ||
                std::filesystem::is_regular_file(status),
            "An installed patch path is not a regular file.");
  }
  struct Installed {
    std::filesystem::path file;
    bool backed_up = false;
    bool replaced = false;
  };
  std::vector<Installed> installed;
  installed.reserve(files.size());
  try {
    for (const auto& file : files) {
      auto& item = installed.emplace_back(file);
      if (std::filesystem::exists(target / file)) {
        std::filesystem::rename(target / file, backup / file);
        item.backed_up = true;
      }
      std::filesystem::rename(staged / file, target / file);
      item.replaced = true;
    }
  } catch (...) {
    bool restored = true;
    for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
      std::error_code error;
      if (it->replaced) {
        std::filesystem::remove(target / it->file, error);
        restored &= !error;
      }
      if (it->backed_up) {
        error.clear();
        std::filesystem::rename(backup / it->file, target / it->file, error);
        restored &= !error;
      }
    }
    if (!restored) {
      workspace.Keep();
      throw std::runtime_error(
          "Patch installation failed; recovery files were retained in the "
          "patch staging folder.");
    }
    throw;
  }
  return files.size();
}
}  // namespace UWP
#endif  // XENIA_UWP_PATCH_ARCHIVE_H_
