// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

namespace XDNetplay
{
// ---------------------------------------------------------------------------
// MessageID::TeamData payload format
// ---------------------------------------------------------------------------
//
// The payload is still ONE string (no second message type). A submission may
// prefix the Showdown text with a HEADER BLOCK: a run of "Key: value" lines
// followed by one blank line:
//
//     Name: Logan\n
//     Model: 43\n
//     \n
//     Blaziken @ Life Orb\n
//     ...
//
// Rules, deliberately narrow so a Showdown export can never be mistaken for a
// header block:
//  - the block is recognized ONLY at byte 0 of the payload. Every line up to
//    the first blank line must be a header line -- a key of the exact shape
//    [A-Za-z][A-Za-z0-9_-]* immediately followed by ':' -- and that blank
//    line must exist. If ANY line breaks the run, or the payload ends before
//    a blank line, the WHOLE payload is team text (the pre-header behavior).
//  - why an export cannot trip this: some real set lines do look like headers
//    ("Type: Null @ Eviolite" -- Type: Null is a species -- and "Ability:"/
//    "EVs:"/"IVs:" lines), but every playable set breaks the run before its
//    blank line with a nature line ("Adamant Nature") or a move line
//    ("- Protect"), neither of which fits the key shape. Only a degenerate
//    set with no moves and no nature could be eaten, and that is not a
//    playable set to begin with.
//  - recognized keys, both case-sensitive:
//      "Name"  -- in-game trainer name. Everything after the colon up to the
//                 newline, trimmed and sanitized HOST-side (see
//                 EmeraldSave::SanitizeTrainerName).
//      "Model" -- the guest's cosmetic trainer-model pick for the battle
//                 style feature, an integer in decimal or 0xHH hex. The value
//                 is untrusted remote text: it is parsed here, but VALIDATED
//                 by the host against BattleCustomizer's model table. Absent
//                 or unparseable means "no preference" (the host's fallback
//                 dropdown wins) and MUST NOT reject the team; an unknown id
//                 is dropped, never clamped.
//    Unknown keys are skipped harmlessly. Netplay refuses mismatched builds,
//    so both ends of a session always speak the same grammar -- the skipping
//    is robustness, not a compatibility channel.
//  - anything after the blank line is the Showdown text, byte for byte.
//
// Backward compatibility, both directions:
//  - a payload with no header block parses as name = "", no model, and the
//    whole payload as Showdown text, which is exactly what pre-header clients
//    send (the original "Name:"-only single-header grammar is a strict subset
//    of this one, so those payloads parse identically);
//  - a pre-header HOST handed a header payload feeds the extra lines to
//    ShowdownParser, which skips them as unrecognized -- the team still
//    lands, only the rename/model are lost.
struct TeamSubmission
{
  std::string trainer_name;  // "" when the payload carried no Name header
  std::optional<int> model;  // "Model:" header; nullopt = no preference
  std::string showdown_text;
};

// Joiner side: wrap showdown_text with the header block. An empty (or
// whitespace-only) name and an absent model produce a bare payload identical
// to the old format. Newlines in the name are replaced with spaces so they
// cannot forge framing; a model without a value (or <= 0) emits no line.
std::string BuildTeamSubmissionPayload(const std::string& showdown_text,
                                       const std::string& trainer_name,
                                       std::optional<int> model = std::nullopt);

// Host side: split a received payload. Never fails -- an unparseable header
// block is simply treated as part of the team text.
TeamSubmission ParseTeamSubmissionPayload(const std::string& payload);

// Writes a Showdown-format team into the GBA save the netplay host syncs to
// the guest, so a joiner can bring their own team to someone else's room.
//
// The host owns both players' teams by construction: netplay ships the HOST's
// GBA saves to every client at start, so the guest's party has to exist in the
// host's socket-3 save before RequestStartGame reads it. This runs host-side
// when a TeamData message arrives, and MUST complete before that read -- the
// caller acknowledges only after this returns.
//
// The text is untrusted remote input. Everything downstream is name-whitelisted
// and bounded (unknown species/moves are skipped, stats clamped, party capped),
// and the write is verified with a .bak of the previous save, so a hostile paste
// can at worst produce a silly-but-legal party or an empty result.
//
// device is the SI channel of the guest's socket (2 in this fork's 2-player
// layout). status, when given, receives a human-readable one-line summary
// suitable for relaying into the room chat.
//
// trainer_name is the in-game name the guest asked for (TeamSubmission above);
// "" means "leave the save's existing name alone". It is untrusted remote text:
// it is sanitized down to at most 7 encodable Gen 3 characters, and a name that
// sanitizes to nothing is dropped (the existing name stays) rather than
// rejecting the whole team -- the status line always reports the name actually
// written, so both players can see it before the battle.
bool InjectGuestTeam(const std::string& showdown_text, const std::string& trainer_name, int device,
                     std::string* status);

// End-of-session cleanup: give the host their own team back AND leave no file
// on this machine holding the opponent's party. Call it whenever a room ends,
// from either side of the connection -- it works out for itself what is there.
//
// Competitive integrity, not just tidiness: a Gen 3 save carries every EV, IV
// and nature, so any surviving copy lets a player read their opponent's exact
// spread after (or during) a battle. What this takes:
//
//   <save>            the guest's injected party -- replaced from <save>.hostteam,
//                     or deleted outright when the file only ever existed to
//                     carry that party (no stash, template-seeded)
//   <save>.bak        VerifiedWriteSaveFile's undo copy. After the second and
//                     later injections of a room this is a PREVIOUS guest's
//                     party, and nothing else ever deleted it
//   <save>.tmp        a complete injected image, left behind by a failed rename
//   <save>.guestteam  the marker that says the above may be dirty
//   NetPlayTemp{1..4}.sav in <User>/GBA/ -- netplay's staging copies of the
//                     HOST's saves. On a joiner NetPlayTemp2.sav *is* the
//                     opponent's team; netplay only clears these on the way in
//                     to the next session, so they otherwise persist forever
//
// The host's own team survives all of this: that is what the .hostteam stash is
// for, and it is only released once the restore has actually succeeded.
//
// TIMING: this does not necessarily do the work now. The live mGBA core owns
// the GBA save while a battle runs and rewrites it at teardown, so when a room
// closes mid-battle the work is deferred until emulation reaches Uninitialized
// and runs then. Callers do not need to care which happened. Every run appends
// a "teamcleanup" line to the GBA detect log.
void RestoreHostTeam(int device);
}  // namespace XDNetplay
