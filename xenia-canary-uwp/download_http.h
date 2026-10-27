// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UWP_DOWNLOAD_HTTP_H_
#define XENIA_UWP_DOWNLOAD_HTTP_H_

#include <array>
#include <functional>
#include <optional>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "xenia-canary-uwp/download_file.h"

namespace UWP {

// The response must have been requested with ResponseHeadersRead.
inline winrt::Windows::Foundation::IAsyncOperation<uint64_t> SaveHttpResponse(
    winrt::Windows::Web::Http::HttpResponseMessage response,
    std::filesystem::path destination,
    std::function<void(uint64_t, std::optional<uint64_t>)> progress = {}) {
  response.EnsureSuccessStatusCode();
  co_await winrt::resume_background();
  std::optional<uint64_t> expected_size;
  if (const auto length = response.Content().Headers().ContentLength()) {
    expected_size = length.Value();
  }
  DownloadFile file(std::move(destination));
  auto input = co_await response.Content().ReadAsInputStreamAsync();
  constexpr uint32_t kChunkSize = 64 * 1024;
  winrt::Windows::Storage::Streams::Buffer buffer(kChunkSize);
  std::array<uint8_t, kChunkSize> bytes;
  for (;;) {
    auto chunk = co_await input.ReadAsync(
        buffer, kChunkSize,
        winrt::Windows::Storage::Streams::InputStreamOptions::Partial);
    if (!chunk.Length()) {
      break;
    }
    auto reader =
        winrt::Windows::Storage::Streams::DataReader::FromBuffer(chunk);
    reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(),
                                                bytes.data() + chunk.Length()));
    file.Write(std::span<const uint8_t>(bytes.data(), chunk.Length()));
    if (progress) {
      progress(file.size(), expected_size);
    }
  }
  input.Close();
  file.Finish(expected_size);
  co_return file.size();
}

}  // namespace UWP
#endif  // XENIA_UWP_DOWNLOAD_HTTP_H_
