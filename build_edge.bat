@echo off
rem Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
rem PolyForm Noncommercial License 1.0.0: not for sale, and this notice stays in every copy.
rem Builds Xenia Edge with the Game Vault UWP layer (CMake preset "uwp") for Xbox: Release x64, unsigned.
rem Edge's D3D12 shaders go through Mesa (spirv_to_dxil), built by meson: needs meson, ninja and python mako on PATH.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
set NoDefaultCurrentDirectoryInExePath=
rem (.local\bin holds a broken python3.13 launcher that meson would pick)
set PATH=%PATH:C:\Users\GamerDad\.local\bin;=%
set PATH=C:\Users\GamerDad\AppData\Local\Programs\Python\Python312;C:\Users\GamerDad\AppData\Local\Programs\Python\Python312\Scripts;C:\Users\GamerDad\AppData\Local\Microsoft\WinGet\Links;%PATH%
cd /d "%~dp0"
if not exist build\uwp\xenia.sln cmake --preset uwp -DCMAKE_SYSTEM_VERSION=10.0.26100.0 || exit /b 1
cmake --build build\uwp --config Release --target xenia-uwp -- /m /v:m /nologo %*
