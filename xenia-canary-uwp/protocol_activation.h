// Copyright 2026 Xenia Canary. Released under the BSD license.

#ifndef XENIA_UWP_PROTOCOL_ACTIVATION_H_
#define XENIA_UWP_PROTOCOL_ACTIVATION_H_

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace UWP {

// Parse the decoded cmd query parameter. Backslashes are literal path
// separators, not escape characters. Paths containing spaces must be quoted.
// Accept a path alone or an optional xeniacanary.exe token followed by a path;
// additional command-line options are deliberately not interpreted.
inline std::optional<std::string> ParseProtocolCommand(
    std::string_view command) {
  const auto skip_space = [&]() {
    const auto next = command.find_first_not_of(" \t\r\n\f\v");
    command.remove_prefix(next == std::string_view::npos ? command.size()
                                                         : next);
  };
  const auto read_token = [&]() -> std::optional<std::string_view> {
    skip_space();
    if (command.empty()) {
      return std::nullopt;
    }
    if (command.front() == '"') {
      command.remove_prefix(1);
      const auto end = command.find('"');
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      const auto token = command.substr(0, end);
      command.remove_prefix(end + 1);
      if (!command.empty() &&
          std::string_view(" \t\r\n\f\v").find(command.front()) ==
              std::string_view::npos) {
        return std::nullopt;
      }
      return token;
    }
    const auto end = command.find_first_of(" \t\r\n\f\v");
    const auto token = command.substr(0, end);
    command.remove_prefix(token.size());
    return token;
  };

  auto path = read_token();
  if (!path) {
    return std::nullopt;
  }
  auto executable = path->substr(path->find_last_of("/\\") + 1);
  constexpr std::string_view kExecutable = "xeniacanary.exe";
  bool has_executable = executable.size() == kExecutable.size();
  for (size_t i = 0; has_executable && i < executable.size(); ++i) {
    const char c = executable[i];
    const char lower = c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c;
    has_executable = lower == kExecutable[i];
  }
  if (has_executable) {
    path = read_token();
  }
  skip_space();
  if (!path || path->empty() || !command.empty() || path->front() == '-' ||
      path->find('"') != std::string_view::npos ||
      path->find('\0') != std::string_view::npos) {
    return std::nullopt;
  }
  return std::string(*path);
}

struct ProtocolActivation {
  std::optional<std::string> game_path;
  std::string launch_on_exit;
};

// Windows QueryParsed performs URI decoding before these arguments arrive.
// Each activation creates fresh state so launchOnExit cannot leak into the
// next.
inline ProtocolActivation ParseProtocolActivation(
    std::span<const std::pair<std::string, std::string>> arguments) {
  ProtocolActivation result;
  for (const auto& [name, value] : arguments) {
    if (name == "cmd") {
      result.game_path = ParseProtocolCommand(value);
    } else if (name == "launchOnExit") {
      result.launch_on_exit = value;
    }
  }
  return result;
}

}  // namespace UWP

#endif  // XENIA_UWP_PROTOCOL_ACTIVATION_H_
