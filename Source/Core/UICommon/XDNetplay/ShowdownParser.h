// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace XDNetplay
{
// One parsed set from a Pokemon Showdown team export.
//
// EV/IV arrays use Showdown's writing order: hp, atk, def, spa, spd, spe.
// (Note this differs from the Gen 3 save order hp/atk/def/speed/spatk/spdef;
// MonFactory remaps.)
struct ShowdownSet
{
  std::string species;
  std::optional<std::string> nickname;
  char gender = 0;  // 'M' / 'F' from a "(M)"/"(F)" marker, 0 if none
  std::optional<std::string> item;
  std::optional<std::string> ability;
  int level = 100;
  bool shiny = false;
  std::optional<std::string> nature;
  std::array<int, 6> evs{};                        // hp, atk, def, spa, spd, spe
  std::array<int, 6> ivs{31, 31, 31, 31, 31, 31};  // hp, atk, def, spa, spd, spe
  std::vector<std::string> moves;                  // max 4
};

// Parser for Showdown team-export text. Sets are separated by blank lines;
// lines that are not recognized (e.g. "Happiness:", "Tera Type:") are ignored
// gracefully. Ported 1:1 from the Android Kotlin port (ShowdownParser.kt).
namespace ShowdownParser
{
// Parse a full export possibly containing multiple blank-line-separated sets.
std::vector<ShowdownSet> ParseTeam(const std::string& text);

// Parse one set from its (trimmed, non-blank) lines. std::nullopt if empty.
std::optional<ShowdownSet> ParseSet(const std::vector<std::string>& lines);
}  // namespace ShowdownParser
}  // namespace XDNetplay
