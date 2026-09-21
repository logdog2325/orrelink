// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/Gen3Data.h"

#include <algorithm>
#include <cctype>

#include <picojson.h>

#ifndef XDNETPLAY_STANDALONE_NO_FILE_IO
#include "Common/FileUtil.h"
#endif

namespace XDNetplay
{
namespace
{
int JsonInt(const picojson::value& v)
{
  return v.is<double>() ? static_cast<int>(v.get<double>()) : 0;
}

std::vector<int> JsonIntVector(const picojson::value& v)
{
  std::vector<int> out;
  if (!v.is<picojson::array>())
    return out;
  for (const picojson::value& element : v.get<picojson::array>())
    out.push_back(JsonInt(element));
  return out;
}

template <size_t N>
std::array<int, N> JsonIntArray(const picojson::value& v)
{
  std::array<int, N> out{};
  const std::vector<int> values = JsonIntVector(v);
  for (size_t i = 0; i < N && i < values.size(); i++)
    out[i] = values[i];
  return out;
}

std::map<std::string, int> JsonIdMap(const picojson::value& v)
{
  std::map<std::string, int> out;
  if (!v.is<picojson::object>())
    return out;
  for (const auto& [key, value] : v.get<picojson::object>())
    out[Gen3Data::NormalizeName(key)] = JsonInt(value);
  return out;
}
}  // namespace

std::string Gen3Data::NormalizeName(const std::string& name)
{
  std::string out;
  out.reserve(name.size());
  for (const char c : name)
  {
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9'))
      out.push_back(lower);
  }
  return out;
}

std::optional<Gen3Data> Gen3Data::FromJson(const std::string& text, std::string* error)
{
  picojson::value root;
  const std::string parse_error = picojson::parse(root, text);
  if (!parse_error.empty() || !root.is<picojson::object>())
  {
    if (error)
      *error = "gen3data.json malformed: " + parse_error;
    return std::nullopt;
  }

  const auto member = [&root](const char* name) -> const picojson::value& {
    static const picojson::value null_value;
    const auto& obj = root.get<picojson::object>();
    const auto it = obj.find(name);
    return it != obj.end() ? it->second : null_value;
  };

  Gen3Data data;

  const picojson::value& species_value = member("species");
  if (!species_value.is<picojson::object>())
  {
    if (error)
      *error = "gen3data.json has no species object";
    return std::nullopt;
  }
  for (const auto& [key, value] : species_value.get<picojson::object>())
  {
    if (!value.is<picojson::object>())
      continue;
    const picojson::object& s = value.get<picojson::object>();
    const auto field = [&s](const char* name) -> const picojson::value& {
      static const picojson::value null_value;
      const auto it = s.find(name);
      return it != s.end() ? it->second : null_value;
    };
    Species species;
    species.name = NormalizeName(key);
    species.id = JsonInt(field("id"));
    species.nat_dex = JsonInt(field("natDex"));
    species.base_stats = JsonIntArray<6>(field("baseStats"));
    species.abilities = JsonIntArray<2>(field("abilities"));
    species.gender_ratio = JsonInt(field("genderRatio"));
    if (field("expGroup").is<std::string>())
      species.exp_group = field("expGroup").get<std::string>();
    species.types = JsonIntVector(field("types"));
    data.m_species[species.name] = std::move(species);
  }

  const picojson::value& exp_value = member("expTables");
  if (exp_value.is<picojson::object>())
  {
    for (const auto& [key, value] : exp_value.get<picojson::object>())
    {
      // Actual generated shape: { "max": n, "cumulative": [101] }. Also
      // accept a bare [101] array (the originally stated schema).
      if (value.is<picojson::object>())
      {
        const auto& obj = value.get<picojson::object>();
        const auto it = obj.find("cumulative");
        if (it != obj.end())
          data.m_exp_tables[key] = JsonIntVector(it->second);
      }
      else if (value.is<picojson::array>())
      {
        data.m_exp_tables[key] = JsonIntVector(value);
      }
    }
  }

  const picojson::value& natures_value = member("natures");
  if (natures_value.is<picojson::object>())
  {
    for (const auto& [key, value] : natures_value.get<picojson::object>())
    {
      if (!value.is<picojson::object>())
        continue;
      const picojson::object& n = value.get<picojson::object>();
      Nature nature;
      nature.name = NormalizeName(key);
      const auto get = [&n](const char* name, int fallback) {
        const auto it = n.find(name);
        return it != n.end() ? JsonInt(it->second) : fallback;
      };
      nature.id = get("id", 0);
      nature.plus = get("plus", -1);
      nature.minus = get("minus", -1);
      data.m_natures[nature.name] = std::move(nature);
    }
  }

  const picojson::value& charset_value = member("charset");
  if (charset_value.is<picojson::object>())
  {
    for (const auto& [key, value] : charset_value.get<picojson::object>())
      data.m_charset[key] = JsonInt(value);
  }

  const picojson::value& type_names_value = member("typeNames");
  if (type_names_value.is<picojson::object>())
  {
    for (const auto& [key, value] : type_names_value.get<picojson::object>())
    {
      if (value.is<std::string>())
        data.m_type_names[std::atoi(key.c_str())] = value.get<std::string>();
    }
  }

  data.m_moves = JsonIdMap(member("moves"));
  data.m_items = JsonIdMap(member("items"));
  data.m_abilities = JsonIdMap(member("abilities"));
  return data;
}

#ifndef XDNETPLAY_STANDALONE_NO_FILE_IO
std::optional<Gen3Data> Gen3Data::LoadBundled(std::string* error)
{
  const std::string path = File::GetSysDirectory() + "XDNetplay/gen3data.json";
  std::string text;
  if (!File::ReadFileToString(path, text))
  {
    if (error)
      *error = "could not read " + path;
    return std::nullopt;
  }
  return FromJson(text, error);
}
#endif

namespace
{
// Base PP per internal move id, 0..354 (index 0 is "no move"). Generation 3
// values, which differ from later games for some moves (Giga Drain is 5 here,
// Recover 20). Extracted by script from the Emerald move table and checked
// entry by entry against a second, independent source; the two agree on all
// 355 ids. The Android copy is Gen3MovePp.kt.
constexpr std::array<u8, 355> MOVE_BASE_PP = {
     0, 35, 25, 10, 15, 20, 20, 15, 15, 15, 35, 30,  5, 10, 30, 30, 35, 35, 20, 15,
    20, 20, 10, 20, 30,  5, 25, 15, 15, 15, 25, 20,  5, 35, 15, 20, 20, 20, 15, 30,
    35, 20, 20, 30, 25, 40, 20, 15, 20, 20, 20, 30, 25, 15, 30, 25,  5, 15, 10,  5,
    20, 20, 20,  5, 35, 20, 25, 20, 20, 20, 15, 20, 10, 10, 40, 25, 10, 35, 30, 15,
    20, 40, 10, 15, 30, 15, 20, 10, 15, 10,  5, 10, 10, 25, 10, 20, 40, 30, 30, 20,
    20, 15, 10, 40, 15, 20, 30, 20, 20, 10, 40, 40, 30, 30, 30, 20, 30, 10, 10, 20,
     5, 10, 30, 20, 20, 20,  5, 15, 10, 20, 15, 15, 35, 20, 15, 10, 20, 30, 15, 40,
    20, 15, 10,  5, 10, 30, 10, 15, 20, 15, 40, 40, 10,  5, 15, 10, 10, 10, 15, 30,
    30, 10, 10, 20, 10,  1,  1, 10, 10, 10,  5, 15, 25, 15, 10, 15, 30,  5, 40, 15,
    10, 25, 10, 30, 10, 20, 10, 10, 10, 10, 10, 20,  5, 40,  5,  5, 15,  5, 10,  5,
    15, 10,  5, 10, 20, 20, 40, 15, 10, 20, 20, 25,  5, 15, 10,  5, 20, 15, 20, 25,
    20,  5, 30,  5, 10, 20, 40,  5, 20, 40, 20, 15, 35, 10,  5,  5,  5, 15,  5, 20,
     5,  5, 15, 20, 10,  5,  5, 15, 15, 15, 15, 10, 10, 10, 10, 10, 10, 10, 10, 15,
    15, 15, 10, 20, 20, 10, 20, 20, 20, 20, 20, 10, 10, 10, 20, 20,  5, 15, 10, 10,
    15, 10, 20,  5,  5, 10, 10, 20,  5, 10, 20, 10, 20, 20, 20,  5,  5, 15, 20, 10,
    15, 20, 15, 10, 10, 15, 10,  5,  5, 10, 15, 10,  5, 20, 25,  5, 40, 10,  5, 40,
    15, 20, 20,  5, 15, 20, 30, 15, 15,  5, 10, 30, 20, 30, 15,  5, 40, 15,  5, 20,
     5, 15, 25, 40, 15, 20, 15, 20, 15, 20, 10, 20, 20,  5,  5,
};
}  // namespace

int Gen3Data::MoveBasePp(int move_id)
{
  if (move_id <= 0 || move_id >= static_cast<int>(MOVE_BASE_PP.size()))
    return 0;
  return MOVE_BASE_PP[static_cast<size_t>(move_id)];
}

int Gen3Data::MoveMaxPp(int move_id, int pp_ups)
{
  const int base = MoveBasePp(move_id);
  return base + base * std::clamp(pp_ups, 0, 3) / 5;
}

const Gen3Data::Species* Gen3Data::FindSpecies(const std::string& name) const
{
  const auto it = m_species.find(NormalizeName(name));
  return it != m_species.end() ? &it->second : nullptr;
}

std::optional<int> Gen3Data::MoveId(const std::string& name) const
{
  const auto it = m_moves.find(NormalizeName(name));
  if (it == m_moves.end())
    return std::nullopt;
  return it->second;
}

std::optional<int> Gen3Data::ItemId(const std::string& name) const
{
  const auto it = m_items.find(NormalizeName(name));
  if (it == m_items.end())
    return std::nullopt;
  return it->second;
}

std::optional<int> Gen3Data::AbilityId(const std::string& name) const
{
  const auto it = m_abilities.find(NormalizeName(name));
  if (it == m_abilities.end())
    return std::nullopt;
  return it->second;
}

const Gen3Data::Nature* Gen3Data::FindNature(const std::string& name) const
{
  const auto it = m_natures.find(NormalizeName(name));
  return it != m_natures.end() ? &it->second : nullptr;
}

std::optional<int> Gen3Data::ExpForLevel(const std::string& exp_group, int level) const
{
  const auto it = m_exp_tables.find(exp_group);
  if (it == m_exp_tables.end() || level < 1 || level > 100 ||
      static_cast<size_t>(level) >= it->second.size())
  {
    return std::nullopt;
  }
  return it->second[level];
}
}  // namespace XDNetplay
