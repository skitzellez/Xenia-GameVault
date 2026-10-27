/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <map>
#include "xenia/base/platform_win.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"
#if XE_PLATFORM_WINRT
#include "xenia-canary-uwp/UWPUtil.h"
#endif

namespace xe {

void LaunchWebBrowser(const std::string_view url) {
#if XE_PLATFORM_WINRT
  UWP::LaunchUri(std::string(url));
#else
  auto wide_url = xe::to_utf16(url);
  ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(wide_url.c_str()),
                nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

void LaunchFileExplorer(const std::filesystem::path& url) {
#if !XE_PLATFORM_WINRT
  ShellExecuteW(nullptr, L"explore", url.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
#endif
}

void ShowSimpleMessageBox(SimpleMessageBoxType type,
                          const std::string_view message) {
#if !XE_PLATFORM_WINRT
  const wchar_t* title;
  std::u16string wide_message = xe::to_utf16(message);
  DWORD type_flags = MB_OK | MB_APPLMODAL | MB_SETFOREGROUND;
  switch (type) {
    default:
    case SimpleMessageBoxType::Help:
      title = L"Xenia Help";
      type_flags |= MB_ICONINFORMATION;
      break;
    case SimpleMessageBoxType::Warning:
      title = L"Xenia Warning";
      type_flags |= MB_ICONWARNING;
      break;
    case SimpleMessageBoxType::Error:
      title = L"Xenia Error";
      type_flags |= MB_ICONERROR;
      break;
  }
  MessageBoxW(nullptr, reinterpret_cast<LPCWSTR>(wide_message.c_str()), title,
              type_flags);
#endif
}

static std::map<const uint32_t, DWORD> xeniaToWindowsPriorityClassMapping = {
    {0, NORMAL_PRIORITY_CLASS},
    {1, ABOVE_NORMAL_PRIORITY_CLASS},
    {2, HIGH_PRIORITY_CLASS},
    {3, REALTIME_PRIORITY_CLASS}};

bool SetProcessPriorityClass(const uint32_t priority_class) {
  if (!xeniaToWindowsPriorityClassMapping.count(priority_class)) {
    return false;
  }

  return SetPriorityClass(GetCurrentProcess(),
                          xeniaToWindowsPriorityClassMapping[priority_class]);
}

}  // namespace xe
