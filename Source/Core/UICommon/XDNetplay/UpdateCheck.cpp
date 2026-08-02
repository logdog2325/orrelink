// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/UpdateCheck.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Common/HttpRequest.h"

#include "UICommon/XDNetplay/Version.h"

namespace XDNetplay
{
namespace
{
// The release LIST, deliberately not .../releases/latest: that endpoint ignores pre-releases, and
// every build this project has ever shipped is one, so it would report a version from months back
// and tell everybody they were current forever. Newest-first, so one page is plenty.
constexpr char RELEASE_API_URL[] =
    "https://api.github.com/repos/logdog2325/dolphin-xd-netplay/releases?per_page=20";
constexpr char RELEASES_PAGE_URL[] = "https://github.com/logdog2325/dolphin-xd-netplay/releases";

// GitHub rejects API requests that arrive without a User-Agent.
constexpr char USER_AGENT[] = "dolphin-xd-netplay";

constexpr std::chrono::milliseconds REQUEST_TIMEOUT{10000};

// Long enough for any version anyone will ever tag, short enough that the accumulator below cannot
// overflow.
constexpr std::size_t MAX_COMPONENT_DIGITS = 9;

std::string_view Trim(std::string_view text)
{
  const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!text.empty() && is_space(text.front()))
    text.remove_prefix(1);
  while (!text.empty() && is_space(text.back()))
    text.remove_suffix(1);
  return text;
}

// std::nullopt for anything that is not a plain run of digits, which is what makes a suffixed tag
// ("0.5.0-rc1") report as uncomparable instead of silently ordering wrong.
std::optional<u64> ParseComponent(std::string_view component)
{
  if (component.empty() || component.size() > MAX_COMPONENT_DIGITS)
    return std::nullopt;

  u64 value = 0;
  for (const char c : component)
  {
    if (c < '0' || c > '9')
      return std::nullopt;
    value = value * 10 + static_cast<u64>(c - '0');
  }
  return value;
}

std::optional<std::vector<u64>> ParseVersion(std::string_view tag)
{
  tag = Trim(tag);
  if (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V'))
    tag.remove_prefix(1);
  if (tag.empty())
    return std::nullopt;

  std::vector<u64> components;
  while (true)
  {
    const std::size_t dot = tag.find('.');
    const std::optional<u64> component =
        ParseComponent(dot == std::string_view::npos ? tag : tag.substr(0, dot));
    if (!component)
      return std::nullopt;

    components.push_back(*component);
    if (dot == std::string_view::npos)
      break;
    tag.remove_prefix(dot + 1);
  }
  return components;
}

// The date half of an ISO 8601 timestamp ("2026-07-30T18:12:03Z" -> "2026-07-30").
std::string DateOf(const std::string& timestamp)
{
  return timestamp.substr(0, timestamp.find('T'));
}

std::optional<std::string> GetStringField(const picojson::object& object, const char* key)
{
  const auto it = object.find(key);
  if (it == object.end() || !it->second.is<std::string>())
    return std::nullopt;

  const std::string& value = it->second.get<std::string>();
  if (value.empty())
    return std::nullopt;

  return value;
}

UpdateCheckResult MakeNetworkError(std::string message)
{
  UpdateCheckResult result;
  result.status = Status::NetworkError;
  result.html_url = RELEASES_PAGE_URL;
  result.message = std::move(message);
  return result;
}
}  // namespace

std::optional<int> CompareVersions(std::string_view a, std::string_view b)
{
  const std::optional<std::vector<u64>> left = ParseVersion(a);
  const std::optional<std::vector<u64>> right = ParseVersion(b);
  if (!left || !right)
    return std::nullopt;

  const std::size_t count = std::max(left->size(), right->size());
  for (std::size_t i = 0; i < count; i++)
  {
    const u64 lhs = i < left->size() ? (*left)[i] : 0;
    const u64 rhs = i < right->size() ? (*right)[i] : 0;
    if (lhs != rhs)
      return lhs < rhs ? -1 : 1;
  }
  return 0;
}

UpdateCheckResult CheckForUpdate()
{
  Common::HttpRequest request{REQUEST_TIMEOUT};
  if (!request.IsValid())
    return MakeNetworkError("Could not start the update check on this system.");

  // AllowedReturnCodes::All so the status code below can be read and explained; Ok_Only would
  // collapse a rate limit and an unreachable server into the same empty response.
  const Common::HttpRequest::Response response =
      request.Get(RELEASE_API_URL,
                  {{"User-Agent", USER_AGENT}, {"Accept", "application/vnd.github+json"}},
                  Common::HttpRequest::AllowedReturnCodes::All);

  if (!response)
  {
    return MakeNetworkError(
        "Could not reach GitHub to check for updates. Check your connection and try again.");
  }

  const s32 code = request.GetLastResponseCode();
  if (code == 403 || code == 429)
    return MakeNetworkError("GitHub is rate-limiting update checks, try again later.");
  if (code != 200)
    return MakeNetworkError(fmt::format("GitHub replied with HTTP {} to the update check.", code));

  UpdateCheckResult result;
  result.html_url = RELEASES_PAGE_URL;

  const char* data = reinterpret_cast<const char*>(response->data());
  picojson::value json;
  const std::string parse_error = picojson::parse(json, data, data + response->size());
  if (!parse_error.empty() || !json.is<picojson::array>())
  {
    result.status = Status::Unknown;
    result.message = "GitHub's reply could not be read, so the latest release is unknown.";
    return result;
  }

  // Newest by VERSION, not by position. GitHub returns the list newest-created-first, which is not
  // the same ordering as the tags whenever a build is re-cut, and a draft has no download page yet.
  const picojson::array& releases = json.get<picojson::array>();
  const picojson::object* newest = nullptr;
  std::string newest_tag;
  for (const picojson::value& entry : releases)
  {
    if (!entry.is<picojson::object>())
      continue;
    const picojson::object& candidate = entry.get<picojson::object>();
    const auto draft = candidate.find("draft");
    if (draft != candidate.end() && draft->second.evaluate_as_boolean())
      continue;
    const std::optional<std::string> tag = GetStringField(candidate, "tag_name");
    if (!tag)
      continue;

    // A tag comparable with itself is one this code can order; an odd one (say "nightly") must not
    // shadow the real releases just for being listed first, so it is only a last resort.
    const bool candidate_ranks = CompareVersions(*tag, *tag).has_value();
    if (newest)
    {
      const bool newest_ranks = CompareVersions(newest_tag, newest_tag).has_value();
      if (newest_ranks && !candidate_ranks)
        continue;
      if (newest_ranks == candidate_ranks &&
          (!candidate_ranks || CompareVersions(newest_tag, *tag).value_or(0) >= 0))
      {
        continue;
      }
    }
    newest = &candidate;
    newest_tag = *tag;
  }

  if (!newest)
  {
    result.status = Status::Unknown;
    result.message = "GitHub listed no releases, so the latest release is unknown.";
    return result;
  }

  const picojson::object& release = *newest;
  const std::optional<std::string> tag_name = GetStringField(release, "tag_name");
  const std::optional<std::string> name = GetStringField(release, "name");
  const std::optional<std::string> html_url = GetStringField(release, "html_url");
  const std::optional<std::string> published_at = GetStringField(release, "published_at");

  // Only the two fields the verdict actually rests on are required. A field that is absent or the
  // wrong type means the check failed, not that this build is current: reporting "up to date" off a
  // reply we could not read would hide every future release.
  //
  // The other two are cosmetic and genuinely optional at the source: GitHub returns name as null
  // for a release published without a title, and a draft has no published_at. Requiring them would
  // turn a perfectly readable "you are behind" into "unknown" over a missing headline.
  if (!tag_name || !html_url)
  {
    result.status = Status::Unknown;
    result.message =
        "GitHub's reply was missing release details, so the latest release is unknown.";
    return result;
  }

  result.latest_tag = *tag_name;
  result.release_name = name.value_or(*tag_name);
  result.published_at = published_at ? DateOf(*published_at) : std::string{};

  // Both front ends hand this to the OS URL opener (QDesktopServices::openUrl,
  // Intent.ACTION_VIEW), which will happily act on schemes that are not the web. Only a URL that
  // is actually inside this repository is taken at its word; anything else falls back to the
  // hardcoded releases page, which is never wrong, only less specific.
  constexpr std::string_view REPO_PREFIX = "https://github.com/logdog2325/dolphin-xd-netplay/";
  result.html_url = html_url->starts_with(REPO_PREFIX) ? *html_url : RELEASES_PAGE_URL;

  const std::optional<int> order = CompareVersions(VERSION, result.latest_tag);
  if (!order)
  {
    result.status = Status::Unknown;
    result.message =
        fmt::format("This build is {}, and the newest release is {}. They cannot be compared "
                    "automatically -- open the release page to see which is newer.",
                    VERSION, result.latest_tag);
    return result;
  }

  if (*order < 0)
  {
    result.status = Status::UpdateAvailable;
    result.message = fmt::format("XD Netplay {} is available. This build is {}.", result.latest_tag,
                                 VERSION);
    return result;
  }

  result.status = Status::UpToDate;
  result.message = fmt::format("XD Netplay {} is the newest release.", VERSION);
  return result;
}
}  // namespace XDNetplay
