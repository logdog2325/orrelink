// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/FormatRules.h"

#include <algorithm>
#include <array>
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

// Smogon ADV Doubles OU's own ban list (NOT BANNED_SPECIES: Celebi and
// Jirachi are legal there, Latias, Latios and Ninjask are not). National dex
// numbers, verified against the bundled gen3data.json like the lists above:
// latias 380 (internal 407), latios 381 (408), ninjask 291 (302). Deoxys is
// one species id in Gen 3, so 386 covers every form. Wobbuffet and Wynaut
// are legal.
constexpr BannedSpecies DOUBLES_OU_BANNED_SPECIES[] = {
    {150, "Mewtwo"},  {151, "Mew"},     {249, "Lugia"},    {250, "Ho-Oh"},
    {380, "Latias"},  {381, "Latios"},  {382, "Kyogre"},   {383, "Groudon"},
    {384, "Rayquaza"}, {386, "Deoxys"}, {291, "Ninjask"},
};

// Doubles OU's move bans: Evasion Clause (Double Team, Minimize), OHKO Clause
// (Fissure, Guillotine, Horn Drill, Sheer Cold) and the self-KO moves
// (Self-Destruct, Explosion). INTERNAL move ids, which is also what a save
// stores; verified against gen3data.json "moves": doubleteam 104, minimize
// 107, fissure 90, guillotine 12, horndrill 32, sheercold 329, selfdestruct
// 120, explosion 153.
struct BannedMove
{
  int id;
  const char* display;
};
constexpr BannedMove DOUBLES_OU_BANNED_MOVES[] = {
    {104, "Double Team"}, {107, "Minimize"},   {90, "Fissure"},        {12, "Guillotine"},
    {32, "Horn Drill"},   {329, "Sheer Cold"}, {120, "Self-Destruct"}, {153, "Explosion"},
};

// Phenac's per-species move bans: moves a species could only have from an
// earlier stage that debuted later, or as an egg move (for Marill, only the
// Azurill-only egg moves). National dex for the species, INTERNAL move ids,
// verified against gen3data.json like the lists above: pikachu 25, clefairy
// 35, jigglypuff 39, marill 183; bide 117, charge 268, charm 204, doubleslap
// 3, encore 227, followme 266, present 217, reversal 179, sweetkiss 186,
// teeterdance 298, volttackle 344, wish 273, amnesia 133, bellydrum 187,
// icywind 196, magicalleaf 345, splash 150, faintattack 185, faketears 313,
// perishsong 195, tickle 321, bubble 145, refresh 287, sing 47, slam 21.
struct SpeciesMoveBan
{
  int nat_dex;
  int move_id;
  const char* display;
};
constexpr SpeciesMoveBan PHENAC_BANNED_MOVES[] = {
    {25, 117, "Bide"},         {25, 268, "Charge"},       {25, 204, "Charm"},
    {25, 3, "DoubleSlap"},     {25, 227, "Encore"},       {25, 266, "Follow Me"},
    {25, 217, "Present"},      {25, 179, "Reversal"},     {25, 186, "Sweet Kiss"},
    {25, 298, "Teeter Dance"}, {25, 344, "Volt Tackle"},  {25, 273, "Wish"},
    {35, 133, "Amnesia"},      {35, 187, "Belly Drum"},   {35, 204, "Charm"},
    {35, 196, "Icy Wind"},     {35, 345, "Magical Leaf"}, {35, 217, "Present"},
    {35, 150, "Splash"},       {35, 186, "Sweet Kiss"},   {35, 273, "Wish"},
    {39, 204, "Charm"},        {39, 185, "Faint Attack"}, {39, 313, "Fake Tears"},
    {39, 196, "Icy Wind"},     {39, 195, "Perish Song"},  {39, 217, "Present"},
    {39, 186, "Sweet Kiss"},   {39, 321, "Tickle"},       {39, 273, "Wish"},
    {183, 145, "Bubble"},      {183, 204, "Charm"},       {183, 227, "Encore"},
    {183, 287, "Refresh"},     {183, 47, "Sing"},         {183, 21, "Slam"},
    {183, 150, "Splash"},      {183, 321, "Tickle"},
};

// Every Gen 1-3 evolution as (pre-evolution, evolution), by National dex
// number, both ends within 1..386. gen3data.json carries no evolution data,
// so this is a generated constant table: taken from the Gen 3 families'
// evolution lists in the pokeemerald-expansion species data, filtered to
// edges whose both ends are Gen 1-3 species, and cross-checked edge for edge
// (184 of 184) against the Showdown dex's evos lists. A branching species has
// one edge per target (Eevee five, Tyrogue three). Evolutions into later
// generations (Gligar, Murkrow, ...) and pre-evolutions from later
// generations (Munchlax, Happiny, ...) have an end outside Gen 1-3 and are
// not listed; neither rule below needs them.
struct EvolutionEdge
{
  int from;
  int to;
};
constexpr EvolutionEdge EVOLUTIONS[] = {
    {1, 2}, {2, 3}, {4, 5}, {5, 6}, {7, 8}, {8, 9}, {10, 11}, {11, 12}, {13, 14}, {14, 15},
    {16, 17}, {17, 18}, {19, 20}, {21, 22}, {23, 24}, {25, 26}, {27, 28}, {29, 30}, {30, 31},
    {32, 33}, {33, 34}, {35, 36}, {37, 38}, {39, 40}, {41, 42}, {42, 169}, {43, 44}, {44, 45},
    {44, 182}, {46, 47}, {48, 49}, {50, 51}, {52, 53}, {54, 55}, {56, 57}, {58, 59}, {60, 61},
    {61, 62}, {61, 186}, {63, 64}, {64, 65}, {66, 67}, {67, 68}, {69, 70}, {70, 71}, {72, 73},
    {74, 75}, {75, 76}, {77, 78}, {79, 80}, {79, 199}, {81, 82}, {84, 85}, {86, 87}, {88, 89},
    {90, 91}, {92, 93}, {93, 94}, {95, 208}, {96, 97}, {98, 99}, {100, 101}, {102, 103}, {104, 105},
    {109, 110}, {111, 112}, {113, 242}, {116, 117}, {117, 230}, {118, 119}, {120, 121}, {123, 212},
    {129, 130}, {133, 134}, {133, 135}, {133, 136}, {133, 196}, {133, 197}, {137, 233}, {138, 139},
    {140, 141}, {147, 148}, {148, 149}, {152, 153}, {153, 154}, {155, 156}, {156, 157}, {158, 159},
    {159, 160}, {161, 162}, {163, 164}, {165, 166}, {167, 168}, {170, 171}, {172, 25}, {173, 35},
    {174, 39}, {175, 176}, {177, 178}, {179, 180}, {180, 181}, {183, 184}, {187, 188}, {188, 189},
    {191, 192}, {194, 195}, {204, 205}, {209, 210}, {216, 217}, {218, 219}, {220, 221}, {223, 224},
    {228, 229}, {231, 232}, {236, 106}, {236, 107}, {236, 237}, {238, 124}, {239, 125}, {240, 126},
    {246, 247}, {247, 248}, {252, 253}, {253, 254}, {255, 256}, {256, 257}, {258, 259}, {259, 260},
    {261, 262}, {263, 264}, {265, 266}, {265, 268}, {266, 267}, {268, 269}, {270, 271}, {271, 272},
    {273, 274}, {274, 275}, {276, 277}, {278, 279}, {280, 281}, {281, 282}, {283, 284}, {285, 286},
    {287, 288}, {288, 289}, {290, 291}, {290, 292}, {293, 294}, {294, 295}, {296, 297}, {298, 183},
    {300, 301}, {304, 305}, {305, 306}, {307, 308}, {309, 310}, {316, 317}, {318, 319}, {320, 321},
    {322, 323}, {325, 326}, {328, 329}, {329, 330}, {331, 332}, {333, 334}, {339, 340}, {341, 342},
    {343, 344}, {345, 346}, {347, 348}, {349, 350}, {353, 354}, {355, 356}, {360, 202}, {361, 362},
    {363, 364}, {364, 365}, {366, 367}, {366, 368}, {371, 372}, {372, 373}, {374, 375}, {375, 376},
};

constexpr int MAX_NAT_DEX = 386;

// A National dex number's debut generation: 1 (1-151), 2 (152-251), 3
// (252-386), or 0 outside Gen 1-3.
constexpr int DebutGeneration(int nat_dex)
{
  if (nat_dex >= 1 && nat_dex <= 151)
    return 1;
  if (nat_dex >= 152 && nat_dex <= 251)
    return 2;
  if (nat_dex >= 252 && nat_dex <= MAX_NAT_DEX)
    return 3;
  return 0;
}

// Species a format admits by name on top of its computed rule, by National
// dex. Pyrite: none yet; the planned "unviable in higher tiers" exceptions go
// here once that list exists (they do NOT carry over to Phenac, which is
// built on the plain rule). Phenac: Metapod, Kakuna, Silcoon and Cascoon.
constexpr std::array<int, 0> PYRITE_EXTRA_SPECIES{};
constexpr std::array<int, 4> PHENAC_EXTRA_SPECIES{11, 14, 266, 268};

// The two computed species rules, indexed by National dex (0 unused), built
// once from EVOLUTIONS on first use.
//   evolves_in_debut_gen: at least one evolution that already existed in the
//     species' own debut generation, so it could evolve in those games
//     (Ivysaur and Pichu yes; Scyther, Onix, Golbat and Seadra no: their
//     evolutions came in a later generation).
//   unevolved_in_debut_gen: no pre-evolution, at any depth, from the same or
//     an earlier generation (Pikachu yes: Pichu came later; Ivysaur no).
struct SpeciesRuleTables
{
  std::array<bool, MAX_NAT_DEX + 1> evolves_in_debut_gen{};
  std::array<bool, MAX_NAT_DEX + 1> unevolved_in_debut_gen{};
};

const SpeciesRuleTables& GetSpeciesRuleTables()
{
  static const SpeciesRuleTables tables = [] {
    SpeciesRuleTables t;
    for (const EvolutionEdge& edge : EVOLUTIONS)
    {
      if (DebutGeneration(edge.from) != 0 && DebutGeneration(edge.to) != 0 &&
          DebutGeneration(edge.to) <= DebutGeneration(edge.from))
      {
        t.evolves_in_debut_gen[edge.from] = true;
      }
    }
    for (int nat_dex = 1; nat_dex <= MAX_NAT_DEX; nat_dex++)
    {
      const int gen = DebutGeneration(nat_dex);
      bool unevolved = true;
      // Walk up the pre-evolutions. Every Gen 1-3 species has at most one, so
      // this is a simple chain; the step cap only guards against a bad table.
      int current = nat_dex;
      for (int step = 0; step < 4 && unevolved; step++)
      {
        const auto it = std::ranges::find(EVOLUTIONS, current, &EvolutionEdge::to);
        if (it == std::end(EVOLUTIONS))
          break;
        if (DebutGeneration(it->from) <= gen)
          unevolved = false;
        current = it->from;
      }
      t.unevolved_in_debut_gen[nat_dex] = unevolved;
    }
    return t;
  }();
  return tables;
}

template <size_t N>
bool Contains(const std::array<int, N>& list, int nat_dex)
{
  return std::ranges::find(list, nat_dex) != list.end();
}

bool EvolvesInDebutGeneration(int nat_dex)
{
  return nat_dex >= 1 && nat_dex <= MAX_NAT_DEX &&
         GetSpeciesRuleTables().evolves_in_debut_gen[nat_dex];
}

bool UnevolvedInDebutGeneration(int nat_dex)
{
  return nat_dex >= 1 && nat_dex <= MAX_NAT_DEX &&
         GetSpeciesRuleTables().unevolved_in_debut_gen[nat_dex];
}

// Which allow-list rule a format applies to every species.
enum class SpeciesRule
{
  Any,
  Pyrite,  // evolves within its debut generation
  Phenac,  // Pyrite's rule and unevolved in its debut generation
};

// Why a species fails its format's allow-list rule, or nullptr when it passes.
const char* SpeciesRuleFailure(SpeciesRule rule, int nat_dex)
{
  constexpr const char* NO_EVOLUTION = "no evolution in its own generation";
  switch (rule)
  {
  case SpeciesRule::Pyrite:
    if (Contains(PYRITE_EXTRA_SPECIES, nat_dex) || EvolvesInDebutGeneration(nat_dex))
      return nullptr;
    return NO_EVOLUTION;
  case SpeciesRule::Phenac:
    if (Contains(PHENAC_EXTRA_SPECIES, nat_dex))
      return nullptr;
    if (!EvolvesInDebutGeneration(nat_dex))
      return NO_EVOLUTION;
    if (!UnevolvedInDebutGeneration(nat_dex))
      return "already evolved";
    return nullptr;
  default:
    return nullptr;
  }
}

const SpeciesMoveBan* FindPhenacBannedMove(int nat_dex, int move_id)
{
  for (const SpeciesMoveBan& banned : PHENAC_BANNED_MOVES)
  {
    if (banned.nat_dex == nat_dex && banned.move_id == move_id)
      return &banned;
  }
  return nullptr;
}

template <size_t N>
const BannedSpecies* FindIn(const BannedSpecies (&list)[N], int nat_dex)
{
  for (const BannedSpecies& banned : list)
  {
    if (banned.nat_dex == nat_dex)
      return &banned;
  }
  return nullptr;
}

const BannedSpecies* FindLegendarySpecies(int nat_dex)
{
  return FindIn(LEGENDARY_SPECIES, nat_dex);
}

const BannedMove* FindDoublesOuBannedMove(int move_id)
{
  for (const BannedMove& banned : DOUBLES_OU_BANNED_MOVES)
  {
    if (banned.id == move_id)
      return &banned;
  }
  return nullptr;
}

// What one format enforces. Species Clause applies to every format with team
// rules; the Item Clause to all of them except Doubles OU. A new format is a
// new combination of these fields; an allow-list exception is a new entry in
// the *_EXTRA_SPECIES arrays above, not a new field.
struct RulesProfile
{
  bool ban_restricted = false;   // BANNED_SPECIES (Restricted + Mythicals)
  bool ban_legendaries = false;  // LEGENDARY_SPECIES on top (Limited only)
  bool ban_doubles_ou = false;   // DOUBLES_OU_BANNED_SPECIES and _MOVES
  bool ban_soul_dew = false;
  bool item_clause = true;
  int max_level = 0;  // 0 = no level rule; Limited = 50 (the in-game Lv50
                      // ruleset refuses over-level mons at team entry, so the
                      // gates say it in words first)
  // How many BANNED_SPECIES members the party may hold in total, counted over
  // every mon it brings; -1 = no count (Realgam 2, Realgam Classic 1).
  int restricted_limit = -1;
  SpeciesRule species_rule = SpeciesRule::Any;
  bool phenac_move_bans = false;  // PHENAC_BANNED_MOVES
};

RulesProfile ProfileFor(int format_key_value)
{
  switch (format_key_value)
  {
  case FORMAT_ORRE_COLOSSEUM:
  case FORMAT_HOENN_STADIUM:
    return {.ban_restricted = true, .ban_soul_dew = true};
  case FORMAT_ORRE_UNLIMITED:
  case FORMAT_HOENN_UNLIMITED:
    return {};
  case FORMAT_ORRE_LIMITED:
  case FORMAT_HOENN_LIMITED:
    return {.ban_restricted = true, .ban_legendaries = true, .ban_soul_dew = true, .max_level = 50};
  case FORMAT_DOUBLES_OU:
    return {.ban_doubles_ou = true, .item_clause = false};
  case FORMAT_REALGAM:
    return {.ban_soul_dew = true, .restricted_limit = 2};
  case FORMAT_REALGAM_CLASSIC:
    return {.ban_soul_dew = true, .restricted_limit = 1};
  case FORMAT_PYRITE:
    return {.ban_soul_dew = true, .max_level = 50, .species_rule = SpeciesRule::Pyrite};
  case FORMAT_PHENAC:
    return {.ban_soul_dew = true,
            .max_level = 50,
            .species_rule = SpeciesRule::Phenac,
            .phenac_move_bans = true};
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
  return FindIn(BANNED_SPECIES, nat_dex);
}

// The display name of a move on one of the fixed move-ban tables, or nullptr.
// Built mons only carry move ids, and only banned moves are ever named.
const char* FixedBannedMoveName(int move_id)
{
  if (const BannedMove* banned = FindDoublesOuBannedMove(move_id))
    return banned->display;
  for (const SpeciesMoveBan& banned : PHENAC_BANNED_MOVES)
  {
    if (banned.move_id == move_id)
      return banned.display;
  }
  return nullptr;
}

// The canonical spelling of any species on one of the ban lists ("Ho-Oh"
// rather than a de-punctuated "hooh"), or nullptr.
const char* CanonicalBannedName(int nat_dex)
{
  for (const BannedSpecies* banned :
       {FindBannedSpecies(nat_dex), FindLegendarySpecies(nat_dex),
        FindIn(DOUBLES_OU_BANNED_SPECIES, nat_dex)})
  {
    if (banned != nullptr)
      return banned->display;
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
  // Resolved internal move ids with what the refusal calls each move. Only
  // the move bans read these.
  std::vector<std::pair<int, std::string>> moves;
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
// banned species (the ban lists, then the allow-list rule), the Restricted
// count, banned item, banned moves, level, Species Clause, Item Clause --
// first violation wins, and within a check the earliest party slot wins.
Verdict ValidateEntries(const RulesProfile& profile, const std::vector<Entry>& entries)
{
  for (const Entry& entry : entries)
  {
    if (profile.ban_restricted && FindBannedSpecies(entry.nat_dex) != nullptr)
      return {false, fmt::format("banned species: {}", entry.species_display)};
    if (profile.ban_legendaries && FindLegendarySpecies(entry.nat_dex) != nullptr)
      return {false, fmt::format("banned species: {}", entry.species_display)};
    if (profile.ban_doubles_ou && FindIn(DOUBLES_OU_BANNED_SPECIES, entry.nat_dex) != nullptr)
      return {false, fmt::format("banned species: {}", entry.species_display)};
    // An allow-list admits only species it knows, so an unmapped species
    // (nat_dex 0) fails here too.
    if (const char* why = SpeciesRuleFailure(profile.species_rule, entry.nat_dex))
      return {false, fmt::format("banned species: {} ({})", entry.species_display, why)};
  }

  if (profile.restricted_limit >= 0)
  {
    const auto count = std::ranges::count_if(entries, [](const Entry& entry) {
      return FindBannedSpecies(entry.nat_dex) != nullptr;
    });
    if (count > profile.restricted_limit)
    {
      return {false, fmt::format("{} Restricted or Mythical Pokemon, the limit is {}", count,
                                 profile.restricted_limit)};
    }
  }

  if (profile.ban_soul_dew)
  {
    for (const Entry& entry : entries)
    {
      if (entry.item_id == SOUL_DEW_ITEM_ID)
        return {false, fmt::format("banned item: {}", entry.item_display)};
    }
  }

  if (profile.ban_doubles_ou || profile.phenac_move_bans)
  {
    for (const Entry& entry : entries)
    {
      for (const auto& [move_id, move_display] : entry.moves)
      {
        const bool banned =
            (profile.ban_doubles_ou && FindDoublesOuBannedMove(move_id) != nullptr) ||
            (profile.phenac_move_bans && FindPhenacBannedMove(entry.nat_dex, move_id) != nullptr);
        if (banned)
        {
          return {false,
                  fmt::format("banned move: {} ({})", move_display, entry.species_display)};
        }
      }
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
  // number of itemless mons coexist. Doubles OU has no Item Clause.
  if (profile.item_clause)
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

std::vector<int> SelectableFormats(bool include_multi)
{
  std::vector<int> formats = {FORMAT_ORRE_COLOSSEUM,  FORMAT_OU,
                              FORMAT_DOUBLES_OU,      FORMAT_ORRE_UNLIMITED,
                              FORMAT_ORRE_LIMITED,    FORMAT_HOENN_STADIUM,
                              FORMAT_HOENN_UNLIMITED, FORMAT_HOENN_LIMITED,
                              FORMAT_REALGAM,         FORMAT_REALGAM_CLASSIC,
                              FORMAT_PYRITE,          FORMAT_PHENAC};
  if (include_multi)
    formats.push_back(FORMAT_MULTI);
  formats.push_back(FORMAT_FREE);
  return formats;
}

std::vector<int> KnownFormats()
{
  return SelectableFormats(/*include_multi=*/true);
}

bool IsKnownFormat(int format_key_value)
{
  return format_key_value >= FORMAT_FREE && format_key_value <= FORMAT_PHENAC;
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
  case FORMAT_DOUBLES_OU:
  case FORMAT_REALGAM:
  case FORMAT_REALGAM_CLASSIC:
  case FORMAT_PYRITE:
  case FORMAT_PHENAC:
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
  case FORMAT_DOUBLES_OU: return "Doubles OU";
  case FORMAT_REALGAM: return "Realgam";
  case FORMAT_REALGAM_CLASSIC: return "Realgam Classic";
  case FORMAT_PYRITE: return "Pyrite";
  case FORMAT_PHENAC: return "Phenac";
  case FORMAT_MULTI: return "Multi";
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
  case FORMAT_DOUBLES_OU: return "[DOU] ";
  case FORMAT_REALGAM: return "[Realgam] ";
  case FORMAT_REALGAM_CLASSIC: return "[Realgam-C] ";
  case FORMAT_PYRITE: return "[Pyrite] ";
  case FORMAT_PHENAC: return "[Phenac] ";
  case FORMAT_MULTI: return "[Multi] ";
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
    for (const std::string& move : set.moves)
    {
      // An unknown move name can never be on a ban list.
      if (const std::optional<int> move_id = data.MoveId(move))
        entry.moves.emplace_back(*move_id, move);  // name it what the user typed
    }
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
      const char* canonical = CanonicalBannedName(entry.nat_dex);
      // Prefer the canonical spelling for the fixed ban lists ("Ho-Oh" rather
      // than a de-punctuated "hooh"); everything else gets the Gen3Data name.
      entry.species_display =
          canonical != nullptr ? std::string(canonical) : Capitalize(species_it->second->name);
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
    // Only the banned moves are ever named, so the fixed tables' spelling is
    // all a built mon needs; 0 is an empty move slot. Which moves are banned
    // for this format and species is ValidateEntries' call.
    for (const u32 move : mon.moves)
    {
      if (const char* name = FixedBannedMoveName(static_cast<int>(move)))
        entry.moves.emplace_back(static_cast<int>(move), name);
    }
    entries.push_back(std::move(entry));
  }
  return ValidateEntries(ProfileFor(format_key_value), entries);
}
}  // namespace XDNetplay::FormatRules
