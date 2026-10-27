// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UWP_DOWNLOAD_FILE_H_
#define XENIA_UWP_DOWNLOAD_FILE_H_

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

namespace UWP {

class DownloadFile {
 public:
  explicit DownloadFile(std::filesystem::path destination)
      : destination_(std::move(destination)) {
    static std::atomic<uint64_t> sequence{0};
    partial_ = destination_;
    partial_ +=
        ".part-" +
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(sequence++);
    if (!destination_.parent_path().empty()) {
      std::filesystem::create_directories(destination_.parent_path());
    }
    file_.open(partial_, std::ios::binary | std::ios::trunc);
    if (!file_) {
      throw std::runtime_error("Could not create the download file.");
    }
  }
  ~DownloadFile() {
    file_.close();
    if (!partial_.empty()) {
      std::error_code ec;
      std::filesystem::remove(partial_, ec);
    }
  }
  DownloadFile(const DownloadFile&) = delete;
  DownloadFile& operator=(const DownloadFile&) = delete;

  void Write(std::span<const uint8_t> data) {
    if (!file_.write(reinterpret_cast<const char*>(data.data()), data.size())) {
      throw std::runtime_error(
          "Could not write the download. Check available storage space.");
    }
    size_ += data.size();
  }

  uint64_t size() const { return size_; }

  void Finish(std::optional<uint64_t> expected_size) {
    if (!size_) {
      throw std::runtime_error("The server returned an empty download.");
    }
    if (expected_size && size_ != *expected_size) {
      throw std::runtime_error(
          "The download was incomplete. Please try again.");
    }
    file_.flush();
    if (!file_) {
      throw std::runtime_error("Could not flush the download to storage.");
    }
    file_.close();
    if (!file_) {
      throw std::runtime_error("Could not finish writing the download.");
    }
    std::filesystem::rename(partial_, destination_);
    partial_.clear();
  }

 private:
  std::filesystem::path destination_;
  std::filesystem::path partial_;
  std::ofstream file_;
  uint64_t size_ = 0;
};

}  // namespace UWP
#endif  // XENIA_UWP_DOWNLOAD_FILE_H_
