// Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
// PolyForm Noncommercial License 1.0.0: not for sale, and this notice stays in every copy.
// Windows calls that libraries built for the desktop (Mesa's spirv_to_dxil: util/os_misc.c) import but the Xbox's
// app runtime may not have: an import the console lacks stops the whole app from loading. Defining the import's
// pointer here keeps the linker from importing it at all. A console window: the Xbox app never has one.
#include <windows.h>

extern "C" {
static HWND WINAPI GameVaultNoConsoleWindow() { return nullptr; }
HWND(WINAPI* __imp_GetConsoleWindow)() = GameVaultNoConsoleWindow;
}

// Temp files by ANSI name (api-ms-win-core-file-l1-2-2, newer than the rest the app imports): nothing the Xbox app
// runs makes them; they report "no temp folder".
extern "C" {
static DWORD WINAPI GameVaultNoTempPath(DWORD, LPSTR) { return 0; }
static UINT WINAPI GameVaultNoTempFileName(LPCSTR, LPCSTR, UINT, LPSTR) { return 0; }
DWORD(WINAPI* __imp_GetTempPathA)(DWORD, LPSTR) = GameVaultNoTempPath;
UINT(WINAPI* __imp_GetTempFileNameA)(LPCSTR, LPCSTR, UINT, LPSTR) = GameVaultNoTempFileName;
}
