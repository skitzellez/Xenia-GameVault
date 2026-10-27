/**
 * Copyright (c) 2026 Skitzelles - https://github.com/skitzellez
 * MIT License: free to use in any project; keep this notice.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions: the above
 * copyright notice and this permission notice shall be included in all copies
 * or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS",
 * WITHOUT WARRANTY OF ANY KIND.
 *
 * Why: in a UWP app, CreateFileW and friends only work where the app container
 * itself has rights, i.e. on an NTFS drive that was given "ALL APPLICATION
 * PACKAGES" access on a PC. On a FAT32 or exFAT stick, or an unprepared NTFS
 * one, they fail with "access denied" even though broadFileSystemAccess is
 * granted: that permission is honoured only by the *FromApp functions, which
 * go through the file broker when the direct call is refused and are
 * otherwise the same call.
 *
 * So the import tables of the app and of the C/C++ runtime (fopen, _wstat,
 * std::filesystem) are pointed at the FromApp versions. They take the same
 * arguments, and where the direct call is allowed they behave as before.
 */

#include "uwp_file_redirect.h"

#include <windows.h>

#include <fileapifromapp.h>
#include <memoryapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <winrt/Windows.Storage.h>

namespace {

// "\\?\F:\..." (the long-path form) is refused by the broker; the plain path
// works.
LPCWSTR Plain(LPCWSTR name) {
  if (name && name[0] == L'\\' && name[1] == L'\\' && name[2] == L'?' &&
      name[3] == L'\\' && name[4] && name[5] == L':') {
    return name + 4;
  }
  return name;
}

wchar_t* Widen(LPCSTR s) {
  if (!s) {
    return nullptr;
  }
  int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
  auto w = static_cast<wchar_t*>(malloc(n * sizeof(wchar_t)));
  if (w) {
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
  }
  return w;
}

// The first 50 failed opens, with path and error, go to
// LocalState\uwp_redirect.log so bug reports show what was refused.
void Note(LPCWSTR what, LPCWSTR name, DWORD err) {
  static LONG told;
  static wchar_t log_path[MAX_PATH + 64];
  if (!name || InterlockedIncrement(&told) > 50) {
    return;
  }
  if (!log_path[0]) {
    try {
      std::wstring dir(winrt::Windows::Storage::ApplicationData::Current()
                           .LocalFolder()
                           .Path());
      wcsncpy_s(log_path, (dir + L"\\uwp_redirect.log").c_str(), _TRUNCATE);
    } catch (...) {
      return;
    }
  }
  HANDLE f = CreateFile2FromAppW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ,
                                 OPEN_ALWAYS, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;
  }
  char line[1200];
  int len = snprintf(line, sizeof(line), "%ls failed (error %lu): %ls\r\n",
                     what, err, name);
  DWORD wrote;
  if (len > 0) {
    WriteFile(f, line, DWORD(len), &wrote, nullptr);
  }
  CloseHandle(f);
}

// The broker answers ERROR_INVALID_PARAMETER (or 4252) for an inheritable
// handle and for some flags on FAT32/exFAT; the C runtime passes both, and
// fopen then fails with errno 22. So retry the same open without them.
HANDLE CreateFileTry(LPCWSTR name, DWORD access, DWORD share,
                     LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                     HANDLE templ) {
  HANDLE h =
      CreateFileFromAppW(name, access, share, sa, disposition, flags, templ);
  if (h != INVALID_HANDLE_VALUE) {
    return h;
  }
  if (sa) {
    h = CreateFileFromAppW(name, access, share, nullptr, disposition, flags,
                           templ);
    if (h != INVALID_HANDLE_VALUE) {
      return h;
    }
  }
  if (GetLastError() != ERROR_INVALID_PARAMETER) {
    return h;
  }
  // Attributes and the access-pattern hints only.
  DWORD plain = flags & (0xFFFF | FILE_FLAG_SEQUENTIAL_SCAN |
                         FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_BACKUP_SEMANTICS);
  return CreateFileFromAppW(name, access, share, nullptr, disposition, plain,
                            nullptr);
}

HANDLE WINAPI CreateFileWFromApp(LPCWSTR name, DWORD access, DWORD share,
                                 LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                 DWORD flags, HANDLE templ) {
  name = Plain(name);
  HANDLE h = CreateFileTry(name, access, share, sa, disposition, flags, templ);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    wchar_t what[160];
    swprintf_s(what,
               L"CreateFileW access %08lx share %lx disposition %lu flags "
               L"%08lx sa %d",
               access, share, disposition, flags, sa ? 1 : 0);
    Note(what, name, err);
    SetLastError(err);
  }
  return h;
}

HANDLE WINAPI CreateFile2FromApp(LPCWSTR name, DWORD access, DWORD share,
                                 DWORD disposition,
                                 LPCREATEFILE2_EXTENDED_PARAMETERS ext) {
  name = Plain(name);
  HANDLE h = CreateFile2FromAppW(name, access, share, disposition, ext);
  if (h == INVALID_HANDLE_VALUE && ext && ext->lpSecurityAttributes) {
    CREATEFILE2_EXTENDED_PARAMETERS no_sa = *ext;
    no_sa.lpSecurityAttributes = nullptr;
    h = CreateFile2FromAppW(name, access, share, disposition, &no_sa);
  }
  if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_INVALID_PARAMETER ||
      !ext) {
    if (h == INVALID_HANDLE_VALUE) {
      DWORD err = GetLastError();
      Note(L"CreateFile2", name, err);
      SetLastError(err);
    }
    return h;
  }
  CREATEFILE2_EXTENDED_PARAMETERS plain = *ext;
  plain.lpSecurityAttributes = nullptr;
  plain.hTemplateFile = nullptr;
  plain.dwSecurityQosFlags = 0;
  plain.dwFileFlags &= FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_RANDOM_ACCESS |
                       FILE_FLAG_BACKUP_SEMANTICS;
  return CreateFile2FromAppW(name, access, share, disposition, &plain);
}

HANDLE WINAPI CreateFileAFromApp(LPCSTR name, DWORD access, DWORD share,
                                 LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                 DWORD flags, HANDLE templ) {
  wchar_t* w = Widen(name);
  HANDLE h = CreateFileWFromApp(w, access, share, sa, disposition, flags, templ);
  DWORD err = GetLastError();
  free(w);
  SetLastError(err);
  return h;
}

DWORD WINAPI GetFileAttributesWFromApp(LPCWSTR name) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExFromAppW(Plain(name), GetFileExInfoStandard,
                                   &data)) {
    return INVALID_FILE_ATTRIBUTES;
  }
  return data.dwFileAttributes;
}

DWORD WINAPI GetFileAttributesAFromApp(LPCSTR name) {
  wchar_t* w = Widen(name);
  DWORD a = GetFileAttributesWFromApp(w);
  DWORD err = GetLastError();
  free(w);
  SetLastError(err);
  return a;
}

BOOL WINAPI GetFileAttributesExWFromApp(LPCWSTR name,
                                        GET_FILEEX_INFO_LEVELS level,
                                        LPVOID info) {
  return GetFileAttributesExFromAppW(Plain(name), level, info);
}

HANDLE WINAPI FindFirstFileExWFromApp(LPCWSTR name, FINDEX_INFO_LEVELS level,
                                      LPVOID data, FINDEX_SEARCH_OPS op,
                                      LPVOID filter, DWORD flags) {
  return FindFirstFileExFromAppW(Plain(name), level, data, op, filter, flags);
}

HANDLE WINAPI FindFirstFileWFromApp(LPCWSTR name, LPWIN32_FIND_DATAW data) {
  return FindFirstFileExFromAppW(Plain(name), FindExInfoStandard, data,
                                 FindExSearchNameMatch, nullptr, 0);
}

BOOL WINAPI CreateDirectoryWFromApp(LPCWSTR name, LPSECURITY_ATTRIBUTES sa) {
  return CreateDirectoryFromAppW(Plain(name), sa);
}

BOOL WINAPI DeleteFileWFromApp(LPCWSTR name) {
  return DeleteFileFromAppW(Plain(name));
}

BOOL WINAPI RemoveDirectoryWFromApp(LPCWSTR name) {
  return RemoveDirectoryFromAppW(Plain(name));
}

BOOL WINAPI SetFileAttributesWFromApp(LPCWSTR name, DWORD attributes) {
  return SetFileAttributesFromAppW(Plain(name), attributes);
}

// Saving through a temporary file renamed over the old one (saves, settings).
// MoveFileFromAppW never replaces, so an existing target is replaced with
// ReplaceFileFromAppW, or deleted first when that is refused; another drive
// means copy and delete, as MOVEFILE_COPY_ALLOWED asks.
BOOL WINAPI MoveFileExWFromApp(LPCWSTR from, LPCWSTR to, DWORD flags) {
  from = Plain(from);
  to = Plain(to);
  if (!to) {
    return DeleteFileFromAppW(from);
  }
  if (MoveFileFromAppW(from, to)) {
    return TRUE;
  }
  DWORD err = GetLastError();
  if ((flags & MOVEFILE_REPLACE_EXISTING) &&
      (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS)) {
    if (ReplaceFileFromAppW(to, from, nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS,
                            nullptr, nullptr)) {
      return TRUE;
    }
    if (DeleteFileFromAppW(to) && MoveFileFromAppW(from, to)) {
      return TRUE;
    }
    err = GetLastError();
  } else if ((flags & MOVEFILE_COPY_ALLOWED) && err == ERROR_NOT_SAME_DEVICE) {
    if (CopyFileFromAppW(from, to, !(flags & MOVEFILE_REPLACE_EXISTING)) &&
        DeleteFileFromAppW(from)) {
      return TRUE;
    }
    err = GetLastError();
  }
  SetLastError(err);
  return FALSE;
}

BOOL WINAPI MoveFileWFromApp(LPCWSTR from, LPCWSTR to) {
  return MoveFileExWFromApp(from, to, MOVEFILE_COPY_ALLOWED);
}

BOOL WINAPI ReplaceFileWFromApp(LPCWSTR replaced, LPCWSTR replacement,
                                LPCWSTR backup, DWORD flags, LPVOID exclude,
                                LPVOID reserved) {
  return ReplaceFileFromAppW(Plain(replaced), Plain(replacement), Plain(backup),
                             flags, exclude, reserved);
}

struct Redirect {
  const char* name;
  void* to;
};

const Redirect kRedirects[] = {
    {"CreateFileW", reinterpret_cast<void*>(CreateFileWFromApp)},
    {"CreateFile2", reinterpret_cast<void*>(CreateFile2FromApp)},
    {"CreateFileA", reinterpret_cast<void*>(CreateFileAFromApp)},
    {"GetFileAttributesExW", reinterpret_cast<void*>(GetFileAttributesExWFromApp)},
    {"GetFileAttributesW", reinterpret_cast<void*>(GetFileAttributesWFromApp)},
    {"GetFileAttributesA", reinterpret_cast<void*>(GetFileAttributesAFromApp)},
    {"FindFirstFileExW", reinterpret_cast<void*>(FindFirstFileExWFromApp)},
    {"FindFirstFileW", reinterpret_cast<void*>(FindFirstFileWFromApp)},
    {"CreateDirectoryW", reinterpret_cast<void*>(CreateDirectoryWFromApp)},
    {"DeleteFileW", reinterpret_cast<void*>(DeleteFileWFromApp)},
    {"RemoveDirectoryW", reinterpret_cast<void*>(RemoveDirectoryWFromApp)},
    {"SetFileAttributesW", reinterpret_cast<void*>(SetFileAttributesWFromApp)},
    {"MoveFileExW", reinterpret_cast<void*>(MoveFileExWFromApp)},
    {"MoveFileW", reinterpret_cast<void*>(MoveFileWFromApp)},
    {"ReplaceFileW", reinterpret_cast<void*>(ReplaceFileWFromApp)},
};

// Points one module's imports of the functions above at the FromApp versions.
unsigned RedirectModule(HMODULE module) {
  auto base = reinterpret_cast<BYTE*>(module);
  if (!base) {
    return 0;
  }
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return 0;
  }
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  IMAGE_DATA_DIRECTORY dir =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) {
    return 0;
  }
  unsigned count = 0;
  for (auto imp =
           reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
       imp->Name; imp++) {
    auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(
        base +
        (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    for (; names->u1.AddressOfData; names++, thunk++) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
        continue;
      }
      auto fn = reinterpret_cast<const char*>(
          reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base +
                                                  names->u1.AddressOfData)
              ->Name);
      for (const Redirect& r : kRedirects) {
        if (strcmp(fn, r.name) ||
            thunk->u1.Function == reinterpret_cast<ULONG_PTR>(r.to)) {
          continue;
        }
        ULONG old;
        if (VirtualProtectFromApp(&thunk->u1.Function,
                                  sizeof(thunk->u1.Function), PAGE_READWRITE,
                                  &old)) {
          thunk->u1.Function = reinterpret_cast<ULONG_PTR>(r.to);
          VirtualProtectFromApp(&thunk->u1.Function, sizeof(thunk->u1.Function),
                                old, &old);
          count++;
        }
        break;
      }
    }
  }
  return count;
}

}  // namespace

unsigned UwpRedirectFileApis() {
  // The app itself, the C runtime (fopen, _wstat) and the C++ runtime
  // (std::filesystem). Modules that are not loaded are skipped.
  static const wchar_t* const kModules[] = {
      L"ucrtbase.dll",     L"ucrtbased.dll",   L"msvcp140_app.dll",
      L"msvcp140d_app.dll", L"msvcp140.dll",   L"msvcp140d.dll",
      L"msvcrt.dll",       nullptr,
  };
  unsigned n = RedirectModule(GetModuleHandleW(nullptr));
  for (int i = 0; kModules[i]; i++) {
    if (HMODULE m = GetModuleHandleW(kModules[i])) {
      n += RedirectModule(m);
    }
  }
  return n;
}
