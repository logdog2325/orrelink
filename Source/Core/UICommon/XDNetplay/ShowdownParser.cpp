// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/ShowdownParser.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string_view>

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
// given, when not null, gets a true for every stat the line actually names.
void ParseStatList(const std::string& text, std::array<int, 6>* out,
                   std::array<bool, 6>* given = nullptr)
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
    if (given)
      (*given)[it->second] = true;
  }
}

// ---- Hidden Power ----
//
// Gen 3 and Gen 4 store no Hidden Power type. The game derives it from the low
// bit of each IV:
//   type = (hp + 2*atk + 4*def + 8*spe + 16*spa + 32*spd) * 15 / 63
// A paste names the type ("Hidden Power [Ice]", "Hidden Power Ice") and very
// often carries no IVs line at all, or only "IVs: 0 Atk". Left at 31s such a
// set would come out as Hidden Power Dark, so the IVs are made to match the
// type the paste asks for, the way Showdown's own importer does.

// Low-bit weight of each stat, in Showdown order hp/atk/def/spa/spd/spe.
constexpr std::array<int, 6> HP_BIT_WEIGHT = {1, 2, 4, 16, 32, 8};

// The 16 types in the order the formula numbers them.
constexpr std::array<const char*, 16> HP_TYPE_NAMES = {
    "fighting", "flying", "poison", "ground",   "rock",    "bug", "ghost",  "steel",
    "fire",     "water",  "grass",  "electric", "psychic", "ice", "dragon", "dark",
};

// Showdown's standard spread per type (its HPivs table): bit i set means stat i
// (Showdown order) is 30, every other stat is 31. All of them give 70 power.
// Indexed like HP_TYPE_NAMES.
constexpr std::array<int, 16> HP_STANDARD_EVEN_MASK = {
    0b111100,  // fighting: def spa spd spe
    0b011111,  // flying:   hp atk def spa spd
    0b011100,  // poison:   def spa spd
    0b011000,  // ground:   spa spd
    0b110100,  // rock:     def spd spe
    0b010110,  // bug:      atk def spd
    0b010100,  // ghost:    def spd
    0b010000,  // steel:    spd
    0b101010,  // fire:     atk spa spe
    0b001110,  // water:    atk def spa
    0b001010,  // grass:    atk spa
    0b001000,  // electric: spa
    0b100010,  // psychic:  atk spe
    0b000110,  // ice:      atk def
    0b000010,  // dragon:   atk
    0b000000,  // dark:     all 31
};

// Type index the game derives from a parity pattern: bit i set = stat i odd.
constexpr int HiddenPowerTypeOfOddMask(int odd_mask)
{
  int sum = 0;
  for (int i = 0; i < 6; i++)
  {
    if (odd_mask & (1 << i))
      sum += HP_BIT_WEIGHT[i];
  }
  return sum * 15 / 63;
}

// The table above is proven against the formula at compile time.
constexpr bool StandardSpreadsAreCorrect()
{
  for (int type = 0; type < 16; type++)
  {
    if (HiddenPowerTypeOfOddMask(0b111111 & ~HP_STANDARD_EVEN_MASK[type]) != type)
      return false;
  }
  return true;
}
static_assert(StandardSpreadsAreCorrect(), "a Hidden Power standard spread gives the wrong type");

// Type index named by a move line, or -1: not Hidden Power, or no type named.
int NamedHiddenPowerType(const std::string& move)
{
  std::string letters;
  for (const char c : move)
  {
    if (std::isalpha(static_cast<unsigned char>(c)))
      letters += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  constexpr std::string_view PREFIX = "hiddenpower";
  if (letters.compare(0, PREFIX.size(), PREFIX) != 0)
    return -1;
  const std::string type = letters.substr(PREFIX.size());
  for (int i = 0; i < 16; i++)
  {
    if (type == HP_TYPE_NAMES[i])
      return i;
  }
  return -1;
}

int PopCount6(int mask)
{
  int count = 0;
  for (int i = 0; i < 6; i++)
    count += (mask >> i) & 1;
  return count;
}

// Makes set->ivs produce the Hidden Power type the set's moves name.
//  - IVs already right: nothing changes.
//  - No IVs line: Showdown's standard spread for the type.
//  - Some IVs given: the fewest low-bit flips that reach the type. A stat the
//    paste named is only touched when no other stat can do it, Speed is the
//    next most protected, and ties go to the pattern nearest the standard
//    spread. A flip moves a value by one (31 to 30, 0 to 1), so "0 Atk" stays
//    a minimum Attack set.
void ApplyHiddenPowerIvs(ShowdownSet* set, const std::array<bool, 6>& given)
{
  int wanted = -1;
  for (const std::string& move : set->moves)
  {
    wanted = NamedHiddenPowerType(move);
    if (wanted >= 0)
      break;
  }
  if (wanted < 0)
    return;

  int odd_mask = 0;
  for (int i = 0; i < 6; i++)
  {
    set->ivs[i] = std::clamp(set->ivs[i], 0, 31);
    if (set->ivs[i] & 1)
      odd_mask |= 1 << i;
  }
  if (HiddenPowerTypeOfOddMask(odd_mask) == wanted)
    return;

  const int standard_odd_mask = 0b111111 & ~HP_STANDARD_EVEN_MASK[wanted];
  const bool any_given = std::any_of(given.begin(), given.end(), [](bool g) { return g; });
  int target = standard_odd_mask;
  if (any_given)
  {
    constexpr int SPE = 5;
    int best_cost = -1;
    for (int candidate = 0; candidate < 64; candidate++)
    {
      if (HiddenPowerTypeOfOddMask(candidate) != wanted)
        continue;
      const int flips = candidate ^ odd_mask;
      int cost = 0;
      for (int i = 0; i < 6; i++)
      {
        if (flips & (1 << i))
          cost += given[i] ? 1000 : (i == SPE ? 30 : 10);
      }
      cost += PopCount6(candidate ^ standard_odd_mask);
      if (best_cost < 0 || cost < best_cost)
      {
        best_cost = cost;
        target = candidate;
      }
    }
  }

  for (int i = 0; i < 6; i++)
  {
    if (((target ^ odd_mask) >> i) & 1)
      set->ivs[i] ^= 1;
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
  std::array<bool, 6> ivs_given{};
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
      ParseStatList(line.substr(4), &set.ivs, &ivs_given);
    }
    else if (StartsWith(line, "Happiness:"))
    {
      if (const std::optional<int> h = ToInt(Trim(line.substr(10))))
        set.happiness = std::clamp(*h, 0, 255);
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
    // Anything else ("Tera Type:", ...) is ignored.
  }

  ApplyHiddenPowerIvs(&set, ivs_given);
  return set;
}
}  // namespace XDNetplay::ShowdownParser
