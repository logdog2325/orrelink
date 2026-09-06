// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>

#include "Common/CommonTypes.h"

#include "UICommon/XDNetplay/Gen3Data.h"
#include "UICommon/XDNetplay/Gen3Mon.h"
#include "UICommon/XDNetplay/ShowdownParser.h"

namespace XDNetplay::MonFactory
{
// Build a party-ready Gen3Mon from a parsed Showdown set, mirroring the
// defaults of the validated Python injector (and its Android Kotlin port,
// MonFactory.kt):
//
//  - language English (0x0202), friendship 255 unless the paste says Happiness: N
//  - origins: met level = current level, game of origin 3 (Emerald),
//    ball 4 (Poke Ball), OT gender male, met location 0
//  - PP = 35 for every non-empty move slot, 0 otherwise, PP bonuses 0
//  - contest stats, ribbons, status, pokerus all 0
//
// The PID is chosen deterministically to satisfy, in priority order: nature
// (PID % 25), ability slot (PID & 1), gender (PID low byte vs the species
// gender ratio, when the set requests a gender the species can actually
// have), and shininess ((TID^SID^PIDhigh^PIDlow) < 8). Shiny is relaxed first
// if the combination is unsatisfiable; the search is bounded, never infinite.
//
// trainer_id is the raw u32 as read from the save: low u16 public ID, high
// u16 secret ID. Returns std::nullopt (with *error set, if given) for
// unknown species/move/item/ability/nature names.
std::optional<Gen3Mon> Build(const ShowdownSet& set, const Gen3Data& data,
                             const std::string& trainer_name, u32 trainer_id,
                             std::string* error = nullptr);

// Raise ONE built mon to level 100, in place, keeping its IVs, EVs, nature,
// moves, item and identity -- only the level, experience and the six party
// stats are recomputed (exactly as Build does at construction). RAISE ONLY:
// a mon already at level 100 (or above) is left untouched, and a mon is
// NEVER lowered -- lowering past where a move was learned would make it
// illegal. Empty slots and species the data cannot resolve are skipped.
// Returns true if the mon was changed. This is the level-100-format
// convenience ("Level 100 or lower" is legal, so raising is opt-in, never
// automatic).
bool RaiseMonToLevel100(Gen3Mon& mon, const Gen3Data& data);

// RaiseMonToLevel100 across a party span; returns the number of mons raised.
int RaisePartyToLevel100(std::span<Gen3Mon> party, const Gen3Data& data);
}  // namespace XDNetplay::MonFactory
