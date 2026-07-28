// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/ShowdownParser.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

namespace XDNetplay::ShowdownParser
{
namespace
{
constexpr size_t MAX_MOVES = 4;

// Showdown stat tokens -> index in the hp/atk/def/spa/spd/spe arrays.
const std::map<std::string, int> STAT_INDEX = {
    {"hp", 0},     {"atk", 1},   {"attack", 1},    {"def", 2},      {"defense", 2},
    {"spa", 3},    {"spatk", 3}, {"spatt", 3},     {"spattack", 3}, {"spd", 4},
    {"spdef", 4},  {"spdefense", 4},               {"spe", 5},      {"speed", 5},
};

std::string Trim(const std::string& text)
{
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
    begin++;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    end--;
  return text.substr(begin, end - begin);
}

std::string ToLower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool StartsWith(const std::string& text, const std::string& prefix)
{
  return text.rfind(prefix, 0) == 0;
}

bool EndsWith(const std::string& text, const std::string& suffix)
{
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<int> ToInt(const std::string& text)
{
  if (text.empty())
    return std::nullopt;
  size_t i = text[0] == '-' ? 1 : 0;
  if (i == text.size())
    return std::nullopt;
  for (; i < text.size(); i++)
  {
    if (!std::isdigit(static_cast<unsigned char>(text[i])))
      return std::nullopt;
  }
  return std::atoi(text.c_str());
}

// Parse "252 Atk / 4 Def / 252 Spe" into out; unknown tokens ignored.
void ParseStatList(const std::string& text, std::array<int, 6>* out)
{
  std::stringstream parts(text);
  std::string part;
  while (std::getline(parts, part, '/'))
  {
    std::stringstream tokens(Trim(part));
    std::string value_token;
    std::string stat_token;
    if (!(tokens >> value_token >> stat_token))
      continue;
    const std::optional<int> value = ToInt(value_token);
    if (!value)
      continue;
    const auto it = STAT_INDEX.find(ToLower(stat_token));
    if (it == STAT_INDEX.end())
      continue;
    (*out)[it->second] = *value;
  }
}
}  // namespace

std::vector<ShowdownSet> ParseTeam(const std::string& text)
{
  std::vector<ShowdownSet> sets;
  std::vector<std::string> block;
  const auto flush = [&] {
    if (auto set = ParseSet(block))
      sets.push_back(std::move(*set));
    block.clear();
  };

  std::stringstream lines(text);
  std::string raw_line;
  while (std::getline(lines, raw_line))
  {
    const std::string line = Trim(raw_line);
    if (line.empty())
    {
      flush();
    }
    else if (!StartsWith(line, "==="))
    {
      // "=== [gen3ou] Team Name ===" headers from full exports are skipped.
      block.push_back(line);
    }
  }
  flush();
  return sets;
}

std::optional<ShowdownSet> ParseSet(const std::vector<std::string>& lines)
{
  if (lines.empty())
    return std::nullopt;

  // ---- header line: "Nickname (Species) (G) @ Item" with every part except
  // the species optional.
  std::string header = lines[0];
  std::optional<std::string> item;
  const size_t at_idx = header.rfind(" @ ");
  if (at_idx != std::string::npos)
  {
    const std::string item_text = Trim(header.substr(at_idx + 3));
    if (!item_text.empty())
      item = item_text;
    header = Trim(header.substr(0, at_idx));
  }

  char gender = 0;
  if (EndsWith(header, "(M)"))
  {
    gender = 'M';
    header = Trim(header.substr(0, header.size() - 3));
  }
  else if (EndsWith(header, "(F)"))
  {
    gender = 'F';
    header = Trim(header.substr(0, header.size() - 3));
  }

  std::optional<std::string> nickname;
  std::string species_name = header;
  if (EndsWith(header, ")"))
  {
    const size_t open = header.rfind('(');
    if (open != std::string::npos && open > 0)
    {
      species_name = Trim(header.substr(open + 1, header.size() - open - 2));
      const std::string nick = Trim(header.substr(0, open));
      if (!nick.empty())
        nickname = nick;
    }
  }
  if (species_name.empty())
    return std::nullopt;

  // ---- body lines
  ShowdownSet set;
  set.species = species_name;
  set.nickname = nickname;
  set.gender = gender;
  set.item = item;

  for (size_t i = 1; i < lines.size(); i++)
  {
    const std::string& line = lines[i];
    if (StartsWith(line, "Ability:"))
    {
      const std::string ability = Trim(line.substr(8));
      if (!ability.empty())
        set.ability = ability;
    }
    else if (StartsWith(line, "Level:"))
    {
      if (const std::optional<int> level = ToInt(Trim(line.substr(6))))
        set.level = *level;
    }
    else if (StartsWith(line, "Shiny:"))
    {
      set.shiny = ToLower(Trim(line.substr(6))) == "yes";
    }
    else if (StartsWith(line, "EVs:"))
    {
      ParseStatList(line.substr(4), &set.evs);
    }
    else if (StartsWith(line, "IVs:"))
    {
      ParseStatList(line.substr(4), &set.ivs);
    }
    else if (StartsWith(line, "-"))
    {
      if (set.moves.size() < MAX_MOVES)
      {
        const std::string move = Trim(line.substr(1));
        if (!move.empty())
          set.moves.push_back(move);
      }
    }
    else if (EndsWith(line, " Nature"))
    {
      const std::string nature = Trim(line.substr(0, line.size() - 7));
      if (!nature.empty())
        set.nature = nature;
    }
    // Anything else ("Happiness:", "Tera Type:", ...) is ignored.
  }

  return set;
}
}  // namespace XDNetplay::ShowdownParser
