// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>
#include <vector>

#include "UICommon/XDNetplay/Gen3Data.h"
#include "UICommon/XDNetplay/Gen3Mon.h"
#include "UICommon/XDNetplay/ShowdownParser.h"

// ---------------------------------------------------------------------------
// Battle formats ("$OrreLink FORMAT")
// ---------------------------------------------------------------------------
//
// PURE party validation for the one-tap FORMAT feature: no UI, no file IO, no
// config reads. Callers hand in the format key's value plus (species, item)
// data and get back either "ok" or one specific, human-readable reason.
//
// The format is the HOST's choice, persisted in the MAIN_XD_FORMAT config key
// exactly like the Battle Style picks:
//
//     0 = Free           the default; changes NOTHING. No validation ever
//                        runs, sessions stay byte-identical to a build
//                        without this feature.
//     1 = Orre Colosseum the canon in-game ruleset of XD's Orre Colosseum.
//     2 = OU             the community "$XD OU Fixes" patch set (bring 6 pick
//                        4 and its mechanics fixes). No party-legality layer:
//                        like Free it validates nothing here; picking it makes
//                        the host session run with the Sys-bundled OU Fixes
//                        code enabled (BattleCustomizer derives the cheats
//                        flag from this key -- the format dropdown REPLACED
//                        the old standalone "OU Fixes" toggle).
//
// What "Orre Colosseum" enforces HERE (the party-legality layer):
//   * Species ban list: Gen 1-3 species EXCEPT the restricted legendaries and
//     mythicals -- Mewtwo, Mew, Lugia, Ho-Oh, Celebi, Kyogre, Groudon,
//     Rayquaza, Jirachi, Deoxys (BANNED_SPECIES in the .cpp, by National dex).
//   * Species Clause: no duplicate species among the party.
//   * Item Clause: no duplicate held items among the party. Itemless mons
//     NEVER count as duplicates of each other -- "no item" is not an item.
//   * Soul Dew is banned outright (item id 191; see the .cpp).
//
// What it deliberately does NOT enforce:
//   * The battle type, level preset, entries and entry mode are the IN-GAME
//     rules layer: BattleCustomizer::FormatRuleLines() pins them per format
//     (Double/Single, Lv100/Lv50, pick 4/3 with entry mode 0 so the game's
//     own pick-N flow runs).
//   * Sleep Clause, Freeze Clause and the Self-KO Clause are enforced by the
//     GAME, not here: BattleCustomizer pins the stock tournament ruleset
//     whose decoded clause bytes have them all ON.
//
// Enforcement sites (the callers):
//   * guest submission gate -- InjectGuestTeam / InjectGuestBundle validate
//     before any lifecycle side effect; the refusal reason rides the existing
//     status -> room-chat path prefixed "Orre Colosseum: ".
//   * host gate -- ValidateHostPartiesForFormat (TeamInjector.h) checks the
//     host's port-2 party and port-3 fallback party before hosting begins.
//   * paste-time feedback -- team editors and the Submit dialog show a
//     non-blocking note; only the two gates above ever block.
namespace XDNetplay::FormatRules
{
// Values of the MAIN_XD_FORMAT config key. An int (not an enum class) because
// it travels through config/JNI as a plain integer, like the style picks.
constexpr int FORMAT_FREE = 0;
constexpr int FORMAT_ORRE_COLOSSEUM = 1;
constexpr int FORMAT_OU = 2;
// The Orre community's settled competitive formats (2026-08-27 scope, see
// docs/orre-community-formats.md in the patch repo): three RULESETS times two
// ENTRY SHAPES. Standard = Orre Colosseum's rules; Unlimited = all Pokemon
// and items allowed (Soul Dew legal; Species/Item Clause still apply);
// Limited = level 50 with ALL legendaries banned (the Restricted/Mythical
// list plus every other legendary). Orre = bring 6 pick 4; Hoenn = bring 6
// pick 3. FORMAT_ORRE_COLOSSEUM above is the Standard/Orre cell.
constexpr int FORMAT_ORRE_UNLIMITED = 3;
constexpr int FORMAT_ORRE_LIMITED = 4;
constexpr int FORMAT_HOENN_STADIUM = 5;
constexpr int FORMAT_HOENN_UNLIMITED = 6;
constexpr int FORMAT_HOENN_LIMITED = 7;

// True only for the exact Orre Colosseum value: an unknown/garbage key value
// behaves as Free (no enforcement), never as a surprise lockout.
bool IsOrreColosseum(int format_key_value);

// True only for the exact OU value -- same unknown-behaves-as-Free rule.
bool IsOu(int format_key_value);

// The fixed battle level a format pins, or 0 for none. Standard/Unlimited
// (Orre Colosseum, Orre Unlimited, Hoenn Stadium, Hoenn Unlimited) pin 100;
// the two Limited formats pin 50; Free/OU and unknown values pin nothing (0).
// Used to decide whether a "raise team to the format level" convenience is
// offered -- it is ONLY offered for the level-100 formats, and only ever
// raises (never lowers, which would break legality).
int FormatFixedLevel(int format_key_value);

// True for every format that carries a party-legality layer (the six
// community formats -- Unlimited included, since Species and Item Clause
// still apply there). False for Free, OU and every unknown value: those
// validate nothing, so the callers' "one int compare then nothing" contract
// holds.
bool HasTeamRules(int format_key_value);

// The public-lobby session-name tag for a format, brackets and trailing
// space included ("[Orre] ", "[Hoenn-L] ", ...), or "" for Free and unknown
// values. Shared by both platforms so the lobby reads identically everywhere;
// purely a human-readable label -- matchmaking never keys off it.
const char* FormatSessionTag(int format_key_value);

// Short display name for a format key value ("Free" / "Orre Colosseum" /
// "OU"), shared by both platforms' UI so the dropdowns and messages agree.
const char* FormatDisplayName(int format_key_value);

// Validation outcome. When !ok, reason is one specific human sentence naming
// the offending mon or item, e.g.:
//     "banned species: Kyogre"
//     "banned item: Soul Dew"
//     "duplicate species: Snorlax"
//     "duplicate item: Leftovers (x2)"
struct Verdict
{
  bool ok = true;
  std::string reason;
};

// Entry point (a): parsed Showdown sets, PRE-build -- names are resolved
// through Gen3Data with its own normalization (lowercase, alphanumerics only),
// so "KYOGRE", "Mr. Mime" and "Nidoran-F" all resolve exactly as MonFactory
// will resolve them. A set whose species or item name does not resolve is
// SKIPPED here, because MonFactory::Build will refuse that whole set and the
// mon can never land in the save -- validating it would risk refusing a paste
// whose offending entry was never going to play. Validates ALL resolvable
// sets, whatever the party size (1..6; a paste is capped downstream).
Verdict ValidateSets(int format_key_value, const std::vector<ShowdownSet>& sets,
                     const Gen3Data& data);

// Entry point (b): built Gen3Mon spans -- the party-bundle path and save
// reading. mon.species is the INTERNAL (Hoenn) species id; it is mapped to the
// National dex number through Gen3Data before the ban list / Species Clause
// apply (the two id spaces diverge from Hoenn onward: Kyogre is internal 404,
// National 382). held_item == 0 means "no item" and never counts toward the
// Item Clause. Validates every non-empty mon in the span (party size 1..6).
Verdict ValidateParty(int format_key_value, std::span<const Gen3Mon> party,
                      const Gen3Data& data);
}  // namespace XDNetplay::FormatRules
