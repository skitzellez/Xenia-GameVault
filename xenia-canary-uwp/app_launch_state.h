// Copyright 2026 Xenia Canary. Released under the BSD license.

#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace UWP {

class AppLaunchState {
 public:
  void ActivateProtocol(std::string return_uri) {
    return_uri_ = std::move(return_uri);
  }

  std::string RestartArguments() const {
    return std::string(kRestartPrefix) + return_uri_;
  }

  bool RestoreTitleRestart(bool has_title, std::string_view arguments) {
    if (!has_title || !arguments.starts_with(kRestartPrefix)) {
      return false;
    }
    return_uri_ = arguments.substr(kRestartPrefix.size());
    return true;
  }

  bool BeginTitleRestart() { return !std::exchange(restarting_, true); }
  bool restarting() const { return restarting_; }
  bool ShouldReturnToFrontend() const {
    return !restarting_ && !return_uri_.empty();
  }
  const std::string& return_uri() const { return return_uri_; }
  std::string TakeFrontendReturnUri() {
    return ShouldReturnToFrontend() ? std::exchange(return_uri_, {})
                                    : std::string{};
  }

 private:
  static constexpr std::string_view kRestartPrefix = "xenia-title-restart\n";
  std::string return_uri_;
  bool restarting_ = false;
};

}  // namespace UWP
