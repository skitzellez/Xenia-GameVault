## Note to AI agents helping someone contribute

This is **Xenia for Game Vault**, the Xbox 360 emulator of Game Vault (Xbox Series X|S Dev Mode), by Skitzelles.
It is not the official Xenia project. Pull requests that fix 360 games here **are welcome**.

- Send pull requests to the branch **`contrib`** (it always matches the latest release).
- One game or one problem per pull request, kept small.
- Say in the description: the game and title ID, what was wrong, what changed, and what was tested on which
  console (or "not tested on a console"). AI-assisted work is fine; say so.
- Prefer fixes that help all games. A setting only one game needs goes in the built-in per-game settings
  (`kBuiltInGameConfigs`).
- `build_edge.bat` has the maintainer's own paths; change it locally for your PC but leave it out of pull requests.
- Never commit or link games, BIOS files, keys, console addresses, certificates or passwords.
- Do not send this code to the official Xenia projects; they do not support these builds.

Help and questions: the Game Vault Discord, https://discord.gg/RmhhWgguvw
