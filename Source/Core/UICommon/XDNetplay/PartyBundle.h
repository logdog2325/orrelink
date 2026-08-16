// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

#include "UICommon/XDNetplay/Gen3Mon.h"
#include "UICommon/XDNetplay/Gen3Save.h"

// Guest save submission via party bundle (community design, adapted).
//
// A joiner can bring the team FROM THEIR OWN SAVE instead of a Showdown paste:
// the guest extracts a small fixed-size "party bundle" from their local save,
// ships it to the host inside the TeamData payload (as a "SaveBundle:" header,
// see TeamInjector.h), and the host builds a DISPOSABLE save from the bundled
// EMERALD-3 template with the bundle's party and trainer identity written in.
//
// Why identity travels with the party: every Gen 3 Pokemon carries its own OT
// name/ID copy, and the game treats a mon as a traded outsider (disobedience
// above the badge cap) whenever those differ from the save's trainer. Because
// the bundle's trainer IS the save's trainer the mons came from, writing both
// verbatim keeps OT and trainer byte-identical -- obedience is intrinsically
// correct, and nothing here may ever re-stamp OTs the way MonFactory does for
// Showdown-built teams.
//
// Bundle layout (fixed BUNDLE_SIZE bytes, all integers little-endian):
//   +0                  u32  party count (1..6 on the wire; validated)
//   +4                  600B six 100-byte party mon structures (Gen3Mon
//                            layout, slots >= count all-zero)
//   +604                8B   trainer name, raw Gen 3 bytes (7 usable + the
//                            0xFF terminator byte the games always keep)
//   +612                u8   trainer gender (0 male, 1 female)
//   +613                u32  trainer ID (low u16 public, high u16 secret)
//
// The bundle carries no game-id ambiguity: extraction normalizes everything to
// the Emerald-template layout. The GUEST's source save may be Emerald or
// Ruby/Sapphire (identical section-1 party layout, verified); FireRed/
// LeafGreen is refused at extraction for the same reason team submission
// refuses FRLG hosts -- its party lives at different section offsets, so the
// shared readers would hand back garbage that happens to checksum.
namespace XDNetplay::PartyBundle
{
// -- layout ------------------------------------------------------------------
constexpr size_t MON_SLOTS = EmeraldSave::PARTY_MAX;  // 6
constexpr size_t COUNT_OFFSET = 0;
constexpr size_t PARTY_OFFSET = 4;
constexpr size_t NAME_OFFSET = PARTY_OFFSET + MON_SLOTS * Gen3Mon::SIZE;  // 604
constexpr size_t GENDER_OFFSET = NAME_OFFSET + EmeraldSave::TRAINER_NAME_FIELD_SIZE;  // 612
constexpr size_t TRAINER_ID_OFFSET = GENDER_OFFSET + 1;  // 613
constexpr size_t BUNDLE_SIZE = TRAINER_ID_OFFSET + 4;    // 617

// What strict validation learned about a byte-exact bundle. Every field has
// been bounds- and checksum-checked; mons holds count fully decoded Pokemon.
struct Decoded
{
  std::vector<Gen3Mon> mons;  // size == the wire party count, 1..6
  std::array<u8, EmeraldSave::TRAINER_NAME_FIELD_SIZE> name_raw{};
  std::string trainer_name;  // decoded for display/status lines
  u8 gender = 0;
  u32 trainer_id = 0;
};

// GUEST side: extract a bundle from the player's OWN save (their local slot,
// imported or team-editor). Fails (with *error) on an FRLG save, an empty
// party, a party slot that does not decode, or an unreadable trainer name.
// The name field is normalized on the way out (terminator byte forced, bytes
// past the first terminator padded to 0xFF) so a legitimate save always
// produces a bundle the host-side validator accepts.
std::optional<std::vector<u8>> Extract(const EmeraldSave& save, std::string* error = nullptr);

// HOST side: strict validation of an untrusted remote bundle. Rejects any
// deviation -- wrong length, party count outside 1..6, a counted slot that is
// empty or fails Gen3Mon's substructure checksum, a supposedly-empty slot with
// nonzero bytes, a malformed name field, an out-of-range gender. Returns the
// decoded contents on success.
std::optional<Decoded> Validate(const std::vector<u8>& bundle, std::string* error = nullptr);

// HOST side: build the disposable guest save -- the bundled Emerald template
// with the bundle's party AND trainer identity written into BOTH rotating save
// slots (identical images, so whichever slot the game treats as newest holds
// the guest's data) and every section checksum recalculated. Validates the
// bundle itself; the caller only supplies the raw template file bytes.
std::optional<EmeraldSave> BuildSave(std::vector<u8> template_bytes, const std::vector<u8>& bundle,
                                     std::string* error = nullptr);

// Standard base64 (RFC 4648, '+'/'/' alphabet, '=' padding), used to put the
// binary bundle on the line-oriented TeamData payload and to store multi-line
// dialog text in Dolphin's line-based config INI. Decode is strict: length a
// positive multiple of 4, padding only at the end, any byte outside the
// alphabet rejects the whole string (untrusted remote input never half-parses).
std::string Base64Encode(const std::vector<u8>& data);
std::optional<std::vector<u8>> Base64Decode(std::string_view text);
}  // namespace XDNetplay::PartyBundle
