// The desktop Discord client IPC transport is unavailable in an app container.
// Keep these symbols so the shared invite encoding/decoding remains available
// without linking desktop registry or named-pipe code into the UWP package.
#include "third_party/discord-rpc/include/discord_rpc.h"

extern "C" {
void Discord_Initialize(const char*, DiscordEventHandlers* handlers, int,
                        const char*) {
  if (handlers && handlers->errored) {
    handlers->errored(-1, "Discord desktop integration is unavailable on UWP");
  }
}
void Discord_Shutdown() {}
void Discord_RunCallbacks() {}
void Discord_UpdatePresence(const DiscordRichPresence*) {}
void Discord_ClearPresence() {}
void Discord_Respond(const char*, int) {}
void Discord_UpdateHandlers(DiscordEventHandlers*) {}
}
