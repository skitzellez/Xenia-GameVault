#include "xenia/ui/file_picker.h"

#include <chrono>
#include <thread>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

#include "xenia/base/logging.h"

namespace xe::ui {
namespace {
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;
using namespace winrt::Windows::UI::Core;

template <typename Operation>
auto WaitForPicker(const Operation& operation) {
  // FilePicker's shared interface is synchronous. Keep the CoreWindow
  // dispatcher moving while the WinRT picker completes on its UI apartment.
  auto window = CoreWindow::GetForCurrentThread();
  if (!window) {
    return operation.get();
  }
  while (operation.Status() ==
         winrt::Windows::Foundation::AsyncStatus::Started) {
    window.Dispatcher().ProcessEvents(
        CoreProcessEventsOption::ProcessAllIfPresent);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return operation.GetResults();
}

class UWPFilePicker final : public FilePicker {
 public:
  bool Show(Window*) override {
    set_selected_files({});
    try {
      if (type() == Type::kDirectory) {
        FolderPicker picker;
        picker.FileTypeFilter().Append(L"*");
        auto folder = WaitForPicker(picker.PickSingleFolderAsync());
        if (folder) {
          set_selected_files({std::filesystem::path(folder.Path().c_str())});
        }
      } else if (mode() == Mode::kSave) {
        FileSavePicker picker;
        picker.SuggestedFileName(winrt::to_hstring(file_name()));
        auto suffix = default_extension();
        if (suffix.empty()) {
          suffix = std::filesystem::path(file_name()).extension().string();
        }
        if (suffix.empty()) {
          suffix = ".bin";
        } else if (suffix.front() != '.') {
          suffix.insert(suffix.begin(), '.');
        }
        picker.FileTypeChoices().Insert(
            L"File", winrt::single_threaded_vector<winrt::hstring>(
                         {winrt::to_hstring(suffix)}));
        auto file = WaitForPicker(picker.PickSaveFileAsync());
        if (file) {
          set_selected_files({std::filesystem::path(file.Path().c_str())});
        }
      } else {
        FileOpenPicker picker;
        for (const auto& [description, patterns] : extensions()) {
          size_t position = 0;
          while (position < patterns.size()) {
            size_t end = patterns.find(';', position);
            auto suffix = patterns.substr(position, end - position);
            if (suffix == "*.*") {
              suffix = "*";
            } else if (suffix.starts_with("*.")) {
              suffix.erase(suffix.begin());
            }
            if (!suffix.empty() && (suffix.front() == '.' || suffix == "*")) {
              picker.FileTypeFilter().Append(winrt::to_hstring(suffix));
            }
            if (end == std::string::npos) {
              break;
            }
            position = end + 1;
          }
        }
        if (!picker.FileTypeFilter().Size()) {
          picker.FileTypeFilter().Append(L"*");
        }
        std::vector<std::filesystem::path> selected;
        if (multi_selection()) {
          for (const auto& file :
               WaitForPicker(picker.PickMultipleFilesAsync())) {
            selected.emplace_back(file.Path().c_str());
          }
        } else if (auto file = WaitForPicker(picker.PickSingleFileAsync())) {
          selected.emplace_back(file.Path().c_str());
        }
        set_selected_files(std::move(selected));
      }
    } catch (const winrt::hresult_error& error) {
      XELOGW("File picker failed: {}", winrt::to_string(error.message()));
      return false;
    }
    return !selected_files().empty();
  }
};
}  // namespace

std::unique_ptr<FilePicker> FilePicker::Create() {
  return std::make_unique<UWPFilePicker>();
}
}  // namespace xe::ui
