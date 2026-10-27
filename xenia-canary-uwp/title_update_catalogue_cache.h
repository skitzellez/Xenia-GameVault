// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UWP_TITLE_UPDATE_CATALOGUE_CACHE_H_
#define XENIA_UWP_TITLE_UPDATE_CATALOGUE_CACHE_H_

#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"
#include "xenia-canary-uwp/download_file.h"
#include "xenia-canary-uwp/title_update_catalogue.h"

namespace UWP {

inline constexpr size_t kTitleUpdateCatalogueCacheLimit = 2 * 1024 * 1024;

inline std::vector<TitleUpdateCandidate> LoadTitleUpdateCatalogue(
    const std::filesystem::path& path, std::string_view title_id,
    std::string_view media_id, std::string_view base_version = {}) {
  const auto title = NormalizeTitleUpdateId(title_id);
  const auto media = NormalizeTitleUpdateId(media_id);
  const auto base = NormalizeTitleUpdateBaseVersion(base_version);
  if (title.empty() || media.empty() ||
      (!base_version.empty() && base.empty())) {
    return {};
  }
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || !size || size > kTitleUpdateCatalogueCacheLimit) {
    return {};
  }
  std::string json(static_cast<size_t>(size), '\0');
  std::ifstream input(path, std::ios::binary);
  if (!input.read(json.data(), json.size()) ||
      input.peek() != std::char_traits<char>::eof()) {
    return {};
  }
  rapidjson::Document document;
  document.Parse(json.data(), json.size());
  if (document.HasParseError() || !document.IsObject()) {
    return {};
  }
  const auto version = document.FindMember("CacheVersion");
  const auto groups = document.FindMember("MediaIDS");
  if (version == document.MemberEnd() || !version->value.IsUint() ||
      version->value.GetUint() != 1 ||
      NormalizeTitleUpdateId(title_update_detail::String(document, "TitleID")) !=
          title ||
      groups == document.MemberEnd() || !groups->value.IsArray() ||
      groups->value.Size() != 1 || !groups->value[0].IsObject()) {
    return {};
  }
  const auto& group = groups->value[0];
  if (NormalizeTitleUpdateId(title_update_detail::String(group, "MediaID")) !=
      media) {
    return {};
  }
  const auto updates = group.FindMember("Updates");
  if (updates == group.MemberEnd() || !updates->value.IsArray()) {
    return {};
  }
  for (const auto& update : updates->value.GetArray()) {
    if (!update.IsObject()) {
      return {};
    }
    for (const auto* key :
         {"TitleUpdateID", "hash", "UploadDate", "Name", "BaseVersion"}) {
      const auto field = update.FindMember(key);
      if (field == update.MemberEnd() || !field->value.IsString()) {
        return {};
      }
    }
    const auto ordinal = update.FindMember("Version");
    const auto length = update.FindMember("Size");
    const auto hash = title_update_detail::String(update, "hash");
    const auto bytes = update.FindMember("filesize");
    const auto url = update.FindMember("url");
    if (ordinal == update.MemberEnd() || !ordinal->value.IsUint() ||
        length == update.MemberEnd() || !length->value.IsUint64() ||
        (bytes != update.MemberEnd() && !bytes->value.IsUint64()) ||
        (url != update.MemberEnd() && !url->value.IsString()) ||
        (!hash.empty() && NormalizeTitleUpdateContentId(hash).empty())) {
      return {};
    }
  }
  std::string error;
  auto result = ParseTitleUpdateCandidates(json, media, error);
  if (!error.empty() || result.size() != updates->value.Size()) {
    return {};
  }
  if (!base_version.empty()) {
    std::erase_if(result, [&](const auto& candidate) {
      return NormalizeTitleUpdateBaseVersion(candidate.base_version) != base;
    });
  }
  return result;
}

inline void SaveTitleUpdateCatalogue(
    const std::filesystem::path& path, std::string_view title_id,
    std::string_view media_id,
    const std::vector<TitleUpdateCandidate>& candidates) {
  const auto title = NormalizeTitleUpdateId(title_id);
  const auto media = NormalizeTitleUpdateId(media_id);
  if (title.empty() || media.empty() || candidates.empty()) {
    throw std::runtime_error("Cannot save an empty or unidentified update list.");
  }
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  const auto text = [&](const char* key, std::string_view value) {
    if (value.size() > kTitleUpdateCatalogueCacheLimit) {
      throw std::runtime_error("The title update list is too large to save.");
    }
    writer.Key(key);
    writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
  };
  writer.StartObject();
  writer.Key("CacheVersion");
  writer.Uint(1);
  text("TitleID", title);
  writer.Key("MediaIDS");
  writer.StartArray();
  writer.StartObject();
  text("MediaID", media);
  writer.Key("Updates");
  writer.StartArray();
  std::set<std::string> identifiers;
  for (const auto& candidate : candidates) {
    const auto content_id = NormalizeTitleUpdateContentId(candidate.content_id);
    if (!IsMatchingTitleUpdate(candidate, media) ||
        !identifiers.insert(candidate.id).second ||
        (!candidate.content_id.empty() && content_id.empty())) {
      throw std::runtime_error("Cannot save an invalid title update entry.");
    }
    writer.StartObject();
    text("TitleUpdateID", candidate.id);
    text("hash", content_id);
    text("url", candidate.download_url);
    writer.Key("Version");
    writer.Uint(candidate.version);
    writer.Key("Size");
    writer.Uint64(candidate.size_kb);
    writer.Key("filesize");
    writer.Uint64(candidate.size_bytes);
    text("UploadDate", candidate.upload_date);
    text("Name", candidate.name);
    text("BaseVersion", candidate.base_version);
    writer.EndObject();
    if (buffer.GetSize() > kTitleUpdateCatalogueCacheLimit) {
      throw std::runtime_error("The title update list is too large to save.");
    }
  }
  writer.EndArray();
  writer.EndObject();
  writer.EndArray();
  writer.EndObject();
  if (buffer.GetSize() > kTitleUpdateCatalogueCacheLimit) {
    throw std::runtime_error("The title update list is too large to save.");
  }
  DownloadFile output(path);
  output.Write({reinterpret_cast<const uint8_t*>(buffer.GetString()),
                buffer.GetSize()});
  output.Finish(buffer.GetSize());
}

}  // namespace UWP
#endif  // XENIA_UWP_TITLE_UPDATE_CATALOGUE_CACHE_H_
