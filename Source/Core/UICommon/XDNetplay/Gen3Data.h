// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace XDNetplay
{
// Static Gen 3 (Emerald) game data loaded from the bundled
// Sys/XDNetplay/gen3data.json (the same file the Android build ships as an
// asset). Ported 1:1 from the Android Kotlin port (Gen3Data.kt).
//
// Schema (verified against the generated file):
//   species:   { name: { id, natDex, baseStats [hp,atk,def,speed,spatk,spdef],
//                        abilities [a1,a2], genderRatio, expGroup, types } }
//   moves:     { name: id }
//   items:     { name: id }
//   abilities: { name: id }
//   natures:   { name: { id, plus, minus } }   plus/minus: -1 = neutral,
//              else stat index 0=atk 1=def 2=speed 3=spatk 4=spdef
//   expTables: { group: { max, cumulative [101] } }  cumulative indexed by
//              level (a plain 101-element array is also accepted)
//   charset:   { char: byte }  (pokeemerald charmap; '$' maps to 0xFF because
//              '$' is the EOS marker in pokeemerald source strings — use
//              Gen3Text for actual name encoding)
//   typeNames: { typeId: name }  (optional)
//
// All name keys are normalized: lowercase with non-alphanumerics stripped.
// The lookup helpers apply the same normalization to their inputs.
class Gen3Data
{
public:
  struct Species
  {
    std::string name;
    int id = 0;       // internal (Hoenn) species id used in save data
    int nat_dex = 0;  // National Dex number
    std::array<int, 6> base_stats{};  // hp, atk, def, speed, spatk, spdef
    std::array<int, 2> abilities{};   // slot 1, slot 2 (0 = none)
    int gender_ratio = 0;  // 0 = always male, 254 = always female, 255 =
                           // genderless, else female iff (PID & 0xFF) < ratio
    std::string exp_group;
    std::vector<int> types;
  };

  struct Nature
  {
    std::string name;
    int id = 0;
    int plus = -1;  // -1 = neutral, else 0=atk 1=def 2=speed 3=spatk 4=spdef
    int minus = -1;
  };

  // Normalize a display name to a lookup key: lowercase, ASCII letters and
  // digits only ("Mr. Mime" -> "mrmime", "Nidoran-F" -> "nidoranf").
  static std::string NormalizeName(const std::string& name);

  // Parse the JSON text. Returns std::nullopt (with *error) on malformed
  // input or a missing top-level object.
  static std::optional<Gen3Data> FromJson(const std::string& text, std::string* error = nullptr);

#ifndef XDNETPLAY_STANDALONE_NO_FILE_IO
  // Load the bundled Sys/XDNetplay/gen3data.json.
  static std::optional<Gen3Data> LoadBundled(std::string* error = nullptr);
#endif

  // -- lookup helpers (input names are normalized) ---------------------------

  const Species* FindSpecies(const std::string& name) const;
  std::optional<int> MoveId(const std::string& name) const;
  std::optional<int> ItemId(const std::string& name) const;
  std::optional<int> AbilityId(const std::string& name) const;
  const Nature* FindNature(const std::string& name) const;

  // Total experience at level (1..100) for growth group exp_group (e.g.
  // "MEDIUM_SLOW"), from the cumulative table. std::nullopt for an unknown
  // group or out-of-range level.
  std::optional<int> ExpForLevel(const std::string& exp_group, int level) const;

  const std::map<std::string, Species>& GetSpecies() const { return m_species; }
  // Normalized item name -> id, exposed for reverse lookups (FormatRules names
  // a held item in its refusal reasons from this map).
  const std::map<std::string, int>& GetItems() const { return m_items; }
  const std::map<int, std::string>& GetTypeNames() const { return m_type_names; }

private:
  std::map<std::string, Species> m_species;
  std::map<std::string, int> m_moves;
  std::map<std::string, int> m_items;
  std::map<std::string, int> m_abilities;
  std::map<std::string, Nature> m_natures;
  std::map<std::string, std::vector<int>> m_exp_tables;
  std::map<std::string, int> m_charset;  // key: single-codepoint UTF-8 string
  std::map<int, std::string> m_type_names;
};
}  // namespace XDNetplay
