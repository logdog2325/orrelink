// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/FormatRules.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <utility>

#include <fmt/format.h>

namespace XDNetplay::FormatRules
{
namespace
{
// The Orre Colosseum restricted list, by NATIONAL dex number. National ids are
// the stable identity here because the internal (Hoenn) species id space
// diverges from National from Hoenn onward.
//
// Every id verified against the bundled Data/Sys/XDNetplay/gen3data.json
// (identical copy shipped as the Android asset), field "species".<name>.natDex:
//   mewtwo 150, mew 151, lugia 249, hooh 250, celebi 251, kyogre 382
//   (internal id 404), groudon 383 (405), rayquaza 384 (406), jirachi 385
//   (409), deoxys 386 (410). The file carries one entry per species (no
//   separate form entries), so matching on natDex covers all forms.
struct BannedSpecies
{
  int nat_dex;
  const char* display;
};
constexpr BannedSpecies BANNED_SPECIES[] = {
    {150, "Mewtwo"}, {151, "Mew"},     {249, "Lugia"},    {250, "Ho-Oh"},   {251, "Celebi"},
    {382, "Kyogre"}, {383, "Groudon"}, {384, "Rayquaza"}, {385, "Jirachi"}, {386, "Deoxys"},
};

// The remaining Gen 1-3 legendaries, banned ONLY by the Limited ruleset (on
// top of BANNED_SPECIES). National dex numbers, same identity rules as above.
constexpr BannedSpecies LEGENDARY_SPECIES[] = {
    {144, "Articuno"}, {145, "Zapdos"},   {146, "Moltres"},   {243, "Raikou"},
    {244, "Entei"},    {245, "Suicune"},  {377, "Regirock"},  {378, "Regice"},
    {379, "Registeel"}, {380, "Latias"},  {381, "Latios"},
};

const BannedSpecies* FindLegendarySpecies(int nat_dex)
{
  for (const BannedSpecies& banned : LEGENDARY_SPECIES)
  {
    if (banned.nat_dex == nat_dex)
      return &banned;
  }
  return nullptr;
}

// What one format enforces. The clauses (Species/Item) are not in here
// because every format with team rules applies them unconditionally.
struct RulesProfile
{
  bool ban_restricted = false;   // BANNED_SPECIES (Restricted + Mythicals)
  bool ban_legendaries = false;  // LEGENDARY_SPECIES on top (Limited only)
  bool ban_soul_dew = false;
  int max_level = 0;  // 0 = no level rule; Limited = 50 (the in-game Lv50
                      // ruleset refuses over-level mons at team entry, so the
                      // gates say it in words first)
};

RulesProfile ProfileFor(int format_key_value)
{
  switch (format_key_value)
  {
  case FORMAT_ORRE_COLOSSEUM:
  case FORMAT_HOENN_STADIUM:
    return {true, false, true, 0};
  case FORMAT_ORRE_UNLIMITED:
  case FORMAT_HOENN_UNLIMITED:
    return {false, false, false, 0};
  case FORMAT_ORRE_LIMITED:
  case FORMAT_HOENN_LIMITED:
    return {true, true, true, 50};
  default:
    return {};
  }
}

// Soul Dew's Gen 3 item id, verified against the bundled
// Data/Sys/XDNetplay/gen3data.json, field "items"."souldew" = 191.
constexpr int SOUL_DEW_ITEM_ID = 191;
constexpr const char* SOUL_DEW_DISPLAY = "Soul Dew";

const BannedSpecies* FindBannedSpecies(int nat_dex)
{
  for (const BannedSpecies& banned : BANNED_SPECIES)
  {
    if (banned.nat_dex == nat_dex)
      return &banned;
  }
  return nullptr;
}

// One party member, normalized from either entry point.
struct Entry
{
  // National dex number; 0 when the species could not be mapped (never
  // matches the ban list -- species_key still dedups it).
  int nat_dex = 0;
  // Species Clause identity: the National dex number when known, otherwise a
  // key derived from the raw internal id so two identical unknowns still
  // collide with each other and nothing else.
  int species_key = 0;
  // Held item id; 0 = no item. ITEMLESS MONS NEVER COUNT AS DUPLICATES of
  // each other -- "no item" is excluded from the Item Clause entirely.
  int item_id = 0;
  // Level when the entry point knows it, else 0 (never checked). Showdown
  // sets default to 100 in the parser; built mons carry the save's byte.
  int level = 0;
  // What the refusal message calls this mon / item: for Showdown sets the
  // text the user actually typed, for built mons the Gen3Data name.
  std::string species_display;
  std::string item_display;
};

// "leftovers" -> "Leftovers": the bundle path only has Gen3Data's normalized
// lookup names to show, so at least lead with a capital.
std::string Capitalize(std::string name)
{
  if (!name.empty())
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
  return name;
}

// The shared rule core. Check order is fixed so refusals are deterministic:
// banned species, banned item, level, Species Clause, Item Clause -- first
// violation wins, and within a check the earliest party slot wins.
Verdict ValidateEntries(const RulesProfile& profile, const std::vector<Entry>& entries)
{
  for (const Entry& entry : entries)
  {
    if (profile.ban_restricted && FindBannedSpecies(entry.nat_dex) != nullptr)
      return {false, fmt::format("banned species: {}", entry.species_display)};
    if (profile.ban_legendaries && FindLegendarySpecies(entry.nat_dex) != nullptr)
      return {false, fmt::format("banned species: {}", entry.species_display)};
  }

  if (profile.ban_soul_dew)
  {
    for (const Entry& entry : entries)
    {
      if (entry.item_id == SOUL_DEW_ITEM_ID)
        return {false, fmt::format("banned item: {}", entry.item_display)};
    }
  }

  if (profile.max_level > 0)
  {
    for (const Entry& entry : entries)
    {
      // level == 0 means the entry point could not know it; the in-game rules
      // screen is the final arbiter there.
      if (entry.level > profile.max_level)
      {
        return {false, fmt::format("over the level limit: {} (Lv {}, max {})",
                                   entry.species_display, entry.level, profile.max_level)};
      }
    }
  }

  // Species Clause: no duplicate species among the party.
  {
    std::map<int, const Entry*> seen;
    for (const Entry& entry : entries)
    {
      const auto [it, inserted] = seen.emplace(entry.species_key, &entry);
      if (!inserted)
        return {false, fmt::format("duplicate species: {}", it->second->species_display)};
    }
  }

  // Item Clause: no duplicate held items among the party. Only mons that HOLD
  // an item participate (item_id != 0 was filtered by the builders), so any
  // number of itemless mons coexist.
  {
    std::map<int, int> counts;
    for (const Entry& entry : entries)
    {
      if (entry.item_id != 0)
        counts[entry.item_id]++;
    }
    for (const Entry& entry : entries)
    {
      if (entry.item_id != 0 && counts[entry.item_id] > 1)
      {
        return {false, fmt::format("duplicate item: {} (x{})", entry.item_display,
                                   counts[entry.item_id])};
      }
    }
  }

  return {};
}
}  // namespace

bool IsOrreColosseum(int format_key_value)
{
  // Exact match only: an unknown value behaves as Free (no enforcement),
  // never as a surprise lockout.
  return format_key_value == FORMAT_ORRE_COLOSSEUM;
}

bool IsOu(int format_key_value)
{
  return format_key_value == FORMAT_OU;
}

int FormatFixedLevel(int format_key_value)
{
  return ProfileFor(format_key_value).max_level == 50 ? 50 :
         (HasTeamRules(format_key_value) ? 100 : 0);
}

bool HasTeamRules(int format_key_value)
{
  switch (format_key_value)
  {
  case FORMAT_ORRE_COLOSSEUM:
  case FORMAT_ORRE_UNLIMITED:
  case FORMAT_ORRE_LIMITED:
  case FORMAT_HOENN_STADIUM:
  case FORMAT_HOENN_UNLIMITED:
  case FORMAT_HOENN_LIMITED:
    return true;
  default:
    return false;
  }
}

const char* FormatDisplayName(int format_key_value)
{
  switch (format_key_value)
  {
  case FORMAT_ORRE_COLOSSEUM: return "Orre Colosseum";
  case FORMAT_OU: return "OU";
  case FORMAT_ORRE_UNLIMITED: return "Orre Unlimited";
  case FORMAT_ORRE_LIMITED: return "Orre Limited";
  case FORMAT_HOENN_STADIUM: return "Hoenn Stadium";
  case FORMAT_HOENN_UNLIMITED: return "Hoenn Unlimited";
  case FORMAT_HOENN_LIMITED: return "Hoenn Limited";
  default: return "Free";
  }
}

const char* FormatSessionTag(int format_key_value)
{
  switch (format_key_value)
  {
  case FORMAT_ORRE_COLOSSEUM: return "[Orre] ";
  case FORMAT_OU: return "[OU] ";
  case FORMAT_ORRE_UNLIMITED: return "[Orre-U] ";
  case FORMAT_ORRE_LIMITED: return "[Orre-L] ";
  case FORMAT_HOENN_STADIUM: return "[Hoenn] ";
  case FORMAT_HOENN_UNLIMITED: return "[Hoenn-U] ";
  case FORMAT_HOENN_LIMITED: return "[Hoenn-L] ";
  default: return "";
  }
}

Verdict ValidateSets(int format_key_value, const std::vector<ShowdownSet>& sets,
                     const Gen3Data& data)
{
  if (!HasTeamRules(format_key_value))
    return {};
  std::vector<Entry> entries;
  entries.reserve(sets.size());
  for (const ShowdownSet& set : sets)
  {
    // Resolution mirrors MonFactory::Build exactly: FindSpecies/ItemId apply
    // Gen3Data's normalization (lowercase, alphanumerics only), so case and
    // punctuation ("KYOGRE", "Mr. Mime", "soul dew") can never cause a false
    // refusal. A set whose species or item does NOT resolve is skipped whole,
    // because MonFactory refuses such a set and it never reaches the save.
    const Gen3Data::Species* species = data.FindSpecies(set.species);
    if (species == nullptr)
      continue;

    Entry entry;
    entry.nat_dex = species->nat_dex;
    entry.species_key = species->nat_dex;
    entry.species_display = set.species;  // name the mon what the user typed
    entry.level = set.level;
    if (set.item)
    {
      const std::optional<int> item_id = data.ItemId(*set.item);
      if (!item_id)
        continue;  // MonFactory refuses the whole set on an unknown item
      entry.item_id = *item_id;
      entry.item_display = *set.item;  // name the item what the user typed
    }
    // No "@ item" in the paste: item_id stays 0 and this mon is invisible to
    // the Item Clause -- itemless mons must never collide with each other.
    entries.push_back(std::move(entry));
  }
  return ValidateEntries(ProfileFor(format_key_value), entries);
}

Verdict ValidateParty(int format_key_value, std::span<const Gen3Mon> party, const Gen3Data& data)
{
  if (!HasTeamRules(format_key_value))
    return {};
  // Reverse maps, built per call: internal species id -> species entry, and
  // item id -> normalized display name. Party validation is a rare,
  // interactive-scale event; a linear pass over ~400 species / ~350 items is
  // nothing.
  std::map<int, const Gen3Data::Species*> species_by_internal_id;
  for (const auto& [key, species] : data.GetSpecies())
    species_by_internal_id.emplace(species.id, &species);
  std::map<int, std::string> item_name_by_id;
  for (const auto& [name, id] : data.GetItems())
    item_name_by_id.emplace(id, name);

  std::vector<Entry> entries;
  entries.reserve(party.size());
  for (const Gen3Mon& mon : party)
  {
    if (mon.IsEmpty())
      continue;

    Entry entry;
    entry.level = static_cast<int>(mon.level);
    const auto species_it = species_by_internal_id.find(static_cast<int>(mon.species));
    if (species_it != species_by_internal_id.end())
    {
      entry.nat_dex = species_it->second->nat_dex;
      entry.species_key = species_it->second->nat_dex;
      const BannedSpecies* banned = FindBannedSpecies(entry.nat_dex);
      // Prefer the canonical spelling for the fixed ban list ("Ho-Oh" rather
      // than a de-punctuated "hooh"); everything else gets the Gen3Data name.
      entry.species_display =
          banned != nullptr ? banned->display : Capitalize(species_it->second->name);
    }
    else
    {
      // Unknown internal id (nothing in gen3data.json): it cannot be on the
      // ban list, but two copies of the same unknown still violate the
      // Species Clause. Offset the key far past every National dex number so
      // it can never collide with a known species.
      entry.species_key = 1000000 + static_cast<int>(mon.species);
      entry.species_display = fmt::format("species #{}", mon.species);
    }

    // held_item == 0 is "no item": excluded from the Item Clause entirely, so
    // any number of itemless mons never read as duplicates of each other.
    if (mon.held_item != 0)
    {
      entry.item_id = static_cast<int>(mon.held_item);
      if (entry.item_id == SOUL_DEW_ITEM_ID)
      {
        entry.item_display = SOUL_DEW_DISPLAY;
      }
      else
      {
        const auto item_it = item_name_by_id.find(entry.item_id);
        entry.item_display = item_it != item_name_by_id.end() ?
                                 Capitalize(item_it->second) :
                                 fmt::format("item #{}", entry.item_id);
      }
    }
    entries.push_back(std::move(entry));
  }
  return ValidateEntries(ProfileFor(format_key_value), entries);
}
}  // namespace XDNetplay::FormatRules
