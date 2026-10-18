// Copyright (c) 2026 Skitzelles - part of Xenia for Game Vault (github.com/skitzellez/xbox-game-vault).
// Licensed like the rest of Xenia (BSD, see LICENSE in the root).
#pragma once

#include <string>

namespace NetGame {

// Game Vault sends "smb://<server>/<share>/<path in the share>" as the game and netcred=<file>: a file in the
// folder only Game Vault and its emulators can open, one line per share "server<tab>share<tab>user<tab>password".
// The file is read once, written over and deleted. The game is then read over the network (libsmb2, smb2.dll)
// and registered with xe::filesystem under the stand-in path returned here; "" when it cannot be opened.
std::string Open(const std::string& url, const std::string& cred_file);

}  // namespace NetGame
