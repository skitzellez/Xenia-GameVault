// Copyright 2026 Xenia Canary. Released under the BSD license.
#ifndef XENIA_UWP_TITLE_UPDATE_CATALOGUE_H_
#define XENIA_UWP_TITLE_UPDATE_CATALOGUE_H_

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "third_party/rapidjson/include/rapidjson/document.h"
#include "xenia-canary-uwp/UWPUtil.h"

namespace UWP {

inline std::string NormalizeTitleUpdateId(std::string_view id) {
  if (id.size() != 8) {
    return {};
  }
  std::string result(id);
  for (auto& ch : result) {
    if (ch >= 'a' && ch <= 'f') {
      ch -= 'a' - 'A';
    } else if (!((ch >= 'A' && ch <= 'F') || (ch >= '0' && ch <= '9'))) {
      return {};
    }
  }
  return result == "00000000" ? std::string{} : result;
}

inline bool IsTitleUpdateDownloadId(std::string_view id) {
  return !id.empty() && id.size() <= 20 &&
         std::all_of(id.begin(), id.end(),
                     [](char ch) { return ch >= '0' && ch <= '9'; });
}

inline std::string NormalizeTitleUpdateBaseVersion(std::string_view version) {
  return version == "00000000" ? std::string(version)
                                : NormalizeTitleUpdateId(version);
}

inline bool IsTitleUpdateDownloadUrl(std::string_view url, std::string_view id) {
  if (!IsTitleUpdateDownloadId(id)) {
    return false;
  }
  for (const auto prefix : {"https://assets.xboxunity.net/api/gettu/",
                            "http://assets.xboxunity.net/api/gettu/"}) {
    if (url.starts_with(prefix) &&
        url.substr(std::string_view(prefix).size()) == id) {
      return true;
    }
  }
  return false;
}

inline std::string NormalizeTitleUpdateContentId(std::string_view id) {
  if (id.size() != 40) {
    return {};
  }
  std::string result(id);
  for (auto& ch : result) {
    if (ch >= 'a' && ch <= 'f') {
      ch -= 'a' - 'A';
    } else if (!((ch >= 'A' && ch <= 'F') || (ch >= '0' && ch <= '9'))) {
      return {};
    }
  }
  return result;
}

inline bool IsMatchingTitleUpdate(const TitleUpdateCandidate& update,
                                  std::string_view media_id) {
  const auto normalized = NormalizeTitleUpdateId(media_id);
  return !normalized.empty() &&
         NormalizeTitleUpdateId(update.media_id) == normalized &&
         IsTitleUpdateDownloadId(update.id) &&
         (update.download_url.empty() ||
          IsTitleUpdateDownloadUrl(update.download_url, update.id));
}

namespace title_update_detail {
inline std::string String(const rapidjson::Value& object, const char* key) {
  const auto it = object.FindMember(key);
  if (it == object.MemberEnd()) {
    return {};
  }
  const auto& value = it->value;
  if (value.IsString()) {
    return {value.GetString(), value.GetStringLength()};
  }
  return value.IsUint64() ? std::to_string(value.GetUint64()) : std::string{};
}

inline uint64_t Number(const rapidjson::Value& object, const char* key) {
  const auto text = String(object, key);
  uint64_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size()
             ? value
             : 0;
}
}  // namespace title_update_detail

inline std::vector<TitleUpdateCandidate> ParseTitleUpdateCandidates(
    std::string_view json, std::string_view media_id, std::string& error,
    std::string_view title_id = {}, std::string_view base_version = {}) {
  error.clear();
  const auto normalized = NormalizeTitleUpdateId(media_id);
  if (normalized.empty()) {
    error =
        "The game's media ID is unavailable. A compatible update cannot be "
        "selected.";
    return {};
  }
  const auto requested_title = NormalizeTitleUpdateId(title_id);
  const auto requested_base = NormalizeTitleUpdateBaseVersion(base_version);
  if ((!title_id.empty() && requested_title.empty()) ||
      (!base_version.empty() && requested_base.empty())) {
    error = "The game's title ID or base version is invalid.";
    return {};
  }
  rapidjson::Document document;
  document.Parse(json.data(), json.size());
  if (document.HasParseError() ||
      (!document.IsObject() && !document.IsArray())) {
    error = "Failed to parse title update metadata.";
    return {};
  }
  std::vector<TitleUpdateCandidate> candidates;
  std::set<std::string> seen;
  const auto add = [&](const rapidjson::Value& entry, bool current_api) {
    if (!entry.IsObject()) {
      return;
    }
    if (current_api) {
      const auto returned_title = NormalizeTitleUpdateId(
          title_update_detail::String(entry, "titleid"));
      if (NormalizeTitleUpdateId(title_update_detail::String(entry, "mediaid")) !=
              normalized ||
          returned_title.empty() ||
          (!title_id.empty() && returned_title != requested_title)) {
        return;
      }
    }
    TitleUpdateCandidate candidate;
    candidate.id = title_update_detail::String(entry, "TitleUpdateID");
    candidate.media_id = normalized;
    candidate.download_url = title_update_detail::String(entry, "url");
    if (!IsMatchingTitleUpdate(candidate, normalized) ||
        (current_api && candidate.download_url.empty())) {
      return;
    }
    candidate.content_id = NormalizeTitleUpdateContentId(
        title_update_detail::String(entry, current_api ? "tuhash" : "hash"));
    const auto version = title_update_detail::Number(
        entry, current_api ? "version" : "Version");
    if (version > std::numeric_limits<uint32_t>::max()) {
      return;
    }
    candidate.version = static_cast<uint32_t>(version);
    candidate.size_bytes = title_update_detail::Number(entry, "filesize");
    if (current_api) {
      if (!candidate.size_bytes) {
        return;
      }
      candidate.size_kb = candidate.size_bytes / 1024 +
                          (candidate.size_bytes % 1024 != 0);
    } else {
      candidate.size_kb = title_update_detail::Number(entry, "Size");
    }
    candidate.upload_date = title_update_detail::String(entry, "UploadDate");
    candidate.name = title_update_detail::String(
        entry, current_api ? "gamename" : "Name");
    candidate.base_version = title_update_detail::String(
        entry, current_api ? "baseversion" : "BaseVersion");
    const auto returned_base =
        NormalizeTitleUpdateBaseVersion(candidate.base_version);
    if ((current_api && returned_base.empty()) ||
        (!base_version.empty() && returned_base != requested_base)) {
      return;
    }
    if (current_api) {
      candidate.base_version = returned_base;
    }
    if (seen.insert(candidate.id).second) {
      candidates.push_back(std::move(candidate));
    }
  };
  if (document.IsArray()) {
    for (const auto& entry : document.GetArray()) {
      add(entry, true);
    }
  } else {
    const auto media = document.FindMember("MediaIDS");
    if (media == document.MemberEnd() || !media->value.IsArray()) {
      error = "No matching title updates were found online.";
      return {};
    }
    for (const auto& group : media->value.GetArray()) {
      if (!group.IsObject() ||
          NormalizeTitleUpdateId(title_update_detail::String(group, "MediaID")) !=
              normalized) {
        continue;
      }
      const auto updates = group.FindMember("Updates");
      if (updates == group.MemberEnd() || !updates->value.IsArray()) {
        continue;
      }
      for (const auto& entry : updates->value.GetArray()) {
        add(entry, false);
      }
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b) {
              if (a.version != b.version) {
                return a.version > b.version;
              }
              if (a.upload_date != b.upload_date) {
                return a.upload_date > b.upload_date;
              }
              return a.id.size() != b.id.size() ? a.id.size() > b.id.size()
                                                : a.id > b.id;
            });
  if (candidates.empty()) {
    error = "No title updates match this game's media ID (" + normalized + ").";
  }
  return candidates;
}

}  // namespace UWP
#endif  // XENIA_UWP_TITLE_UPDATE_CATALOGUE_H_
