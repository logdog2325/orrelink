// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace XDNetplay
{
enum class Status
{
  // The build's XDNetplay::VERSION is at least as new as the newest published tag.
  UpToDate,
  // A newer tag exists; html_url points at its release page.
  UpdateAvailable,
  // GitHub answered but the answer could not be turned into a verdict (unparseable body,
  // missing field, or a tag that is not purely numeric). latest_tag may still be set.
  Unknown,
  // GitHub could not be reached, or replied with an error status.
  NetworkError,
};

struct UpdateCheckResult
{
  Status status = Status::Unknown;
  std::string latest_tag;
  std::string release_name;
  std::string html_url;
  std::string published_at;  // YYYY-MM-DD, empty if GitHub did not supply one
  // One line, already phrased for a message box. Never claims "up to date" unless status is
  // UpToDate.
  std::string message;
};

// Queries the newest release of the fork's GitHub repository and compares it against
// XDNetplay::VERSION.
//
// BLOCKING: this performs an HTTP request and must never be called from a UI thread. Callers own
// the threading (Qt runs it on a worker and posts the result back; Android calls it from
// Dispatchers.IO).
UpdateCheckResult CheckForUpdate();

// Orders two release tags the way a human reads them: an optional leading 'v' is ignored, the rest
// is split on '.' and compared component by component as integers, and a missing component counts
// as 0 ("1.2" == "1.2.0"). Returns <0 if a is older, 0 if equal, >0 if a is newer.
//
// std::nullopt means "cannot compare" -- some component was not a plain number (e.g. "0.4.4-rc1").
// Callers must surface that to the user rather than assume either side is newer. Comparing as
// integers is the entire point: a lexical compare puts "0.4.9" after "0.4.44".
std::optional<int> CompareVersions(std::string_view a, std::string_view b);
}  // namespace XDNetplay
