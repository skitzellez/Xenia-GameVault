// Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
// PolyForm Noncommercial License 1.0.0: not for sale, and this notice stays in every copy.
//
// Game Vault: a Skylanders "Portal of Power" without the toy. The Xbox app can't reach a real USB portal (Dev Mode
// passes no raw USB devices to apps), so the game's portal packets (XamInputNonControllerGetRaw/SetRaw) are answered
// here, and the figures are 1 KB figure files (.sky/.bin/.dmp) in a folder. Written from the portal's published
// command set (A activate, C colour, J, L, M, Q read block, R reset, S status, V, W write block); the wired 360
// portal puts 0B 14 in front of each command.

#ifndef XENIA_HID_PORTAL_VIRTUAL_PORTAL_H_
#define XENIA_HID_PORTAL_VIRTUAL_PORTAL_H_

#include <array>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/hid/portal/portal.h"

namespace xe {
namespace hid {

class VirtualPortal final : public Portal {
 public:
  static constexpr int kSlots = 8;
  static constexpr size_t kFigureSize = 0x400;  // 64 blocks of 16 bytes

  struct SlotInfo {
    bool present = false;
    std::string name;  // the file's name without its extension
    uint16_t id = 0;
    uint16_t variant = 0;
  };

  VirtualPortal();
  ~VirtualPortal() override;

  // On only while a Skylanders game runs (set at title launch); off, the game sees no portal as before.
  void SetEnabled(bool enabled);
  bool enabled() const { return enabled_; }
  void SetFigureFolder(const std::filesystem::path& folder);
  // The portal's messages also go to this file (readable over Device Portal while a game runs).
  void SetLogFile(const std::filesystem::path& file);
  const std::filesystem::path& figure_folder() const { return folder_; }

  // Figure files in the folder, sorted by name.
  std::vector<std::filesystem::path> ListFigureFiles() const;
  // Puts a figure on the portal: the slot, or -1 with |error| set.
  int Place(const std::filesystem::path& file, std::string& error);
  bool Remove(int slot);
  void RemoveAll();
  SlotInfo GetSlot(int slot);
  bool IsPlaced(const std::filesystem::path& file);
  // A new blank figure file (character |id|, |variant|) named |name|.sky in the folder; empty path on failure.
  std::filesystem::path CreateFigure(uint16_t id, uint16_t variant, const std::string& name, std::string& error);

  bool IsConnected() override { return enabled_; }
  void OnDeviceArrival() override {}
  void OnDeviceRemoval() override {}

 private:
  struct Figure {
    std::filesystem::path file;
    std::array<uint8_t, kFigureSize> data{};
    uint8_t status = 0;               // bit 0: on the portal, bit 1: just changed
    std::deque<uint8_t> pending;      // status values still to report, in order
    uint32_t last_serial = 0;         // a figure taken off keeps its slot if it comes back
  };
  using Reply = std::array<uint8_t, kPortalBufferSize>;

  void OpenDevice() override {}
  void CloseDevice() override {}
  X_STATUS ReadInternal(std::span<uint8_t> data, int32_t& read_count) override;
  X_STATUS WriteInternal(std::span<uint8_t> data) override;

  void HandleCommand(const uint8_t* cmd, size_t size);
  void QueueReply(std::initializer_list<uint8_t> bytes);
  void QueueReply(const uint8_t* bytes, size_t size);
  void MakeStatus(uint8_t* out);
  void SaveFigure(const Figure& figure);
  bool ReplyHeader() const;
  void Note(const std::string& line);

  std::atomic<bool> enabled_{false};
  std::filesystem::path folder_;
  std::mutex log_mutex_;
  FILE* log_ = nullptr;
  std::mutex mutex_;
  std::array<Figure, kSlots> figures_;
  std::deque<Reply> replies_;
  bool active_ = true;
  bool header_seen_ = false;          // the game's commands came with 0B 14 in front
  uint8_t counter_ = 0;
  uint32_t commands_logged_ = 0;
  uint32_t reads_logged_ = 0;
  std::chrono::steady_clock::time_point last_status_{};
};

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_PORTAL_VIRTUAL_PORTAL_H_
