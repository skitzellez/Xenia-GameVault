/* Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
 * BSD licence, like Xenia (see LICENSE in the root).
 *
 * Save states for Xbox 360 games (test feature). A state is the game's memory, each game thread's registers and
 * the guest address it goes on at, and the graphics registers. See gv_savestate.cc for what is and is not kept.
 */

#ifndef XENIA_GV_SAVESTATE_H_
#define XENIA_GV_SAVESTATE_H_

#include <filesystem>
#include <string>

namespace xe {

class Emulator;

// Both stop the game's threads for the time they take and start them again (unless they were already held, as
// under the in-game menu). Call from a host thread, never from a game thread. |message| gets one plain line for
// the player or the log.
bool GvSaveState(Emulator* emulator, const std::filesystem::path& path,
                 std::string* message);
bool GvLoadState(Emulator* emulator, const std::filesystem::path& path,
                 std::string* message);

// Guard for a load that went wrong: if the game crashes within half a minute of a load, Xenia logs it and
// closes at once (the crash screen holds the app in a state that once froze the whole console). Called from the
// emulator's crash handling with a word for the log.
void GvCloseIfCrashAfterLoad(const char* what);

// The build a state is made by: a state loads in another run of Xenia only on the same build.
void GvSetStateBuildTag(const std::string& tag);

}  // namespace xe

#endif  // XENIA_GV_SAVESTATE_H_
