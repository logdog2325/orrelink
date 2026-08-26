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
// banned species, banned item, Species Clause, Item Clause -- first violation
// wins, and within a check the earliest party slot wins.
Verdict ValidateEntries(const std::vector<Entry>& entries)
{
  for (const Entry& entry : entries)
  {
    if (FindBannedSpecies(entry.nat_dex) != nullptr)
      return {false, fmt::format("banned species: {}", entry.species_display)};
  }

  for (const Entry& entry : entries)
  {
    if (entry.item_id == SOUL_DEW_ITEM_ID)
      return {false, fmt::format("banned item: {}", entry.item_display)};
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

const char* FormatDisplayName(int format_key_value)
{
  return IsOrreColosseum(format_key_value) ? "Orre Colosseum" : "Free";
}

Verdict ValidateSets(const std::vector<ShowdownSet>& sets, const Gen3Data& data)
{
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
  return ValidateEntries(entries);
}

Verdict ValidateParty(std::span<const Gen3Mon> party, const Gen3Data& data)
{
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
  return ValidateEntries(entries);
}
}  // namespace XDNetplay::FormatRules
