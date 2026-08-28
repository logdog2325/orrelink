// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

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
//      "SaveBundle" -- base64 of a fixed-size party bundle extracted from the
//                 guest's OWN save (PartyBundle.h documents the byte layout
//                 and both endpoints). A payload is EITHER a bundle OR
//                 Showdown text: a bundle payload is the SaveBundle header
//                 (plus optionally Model), the blank line, and an EMPTY body.
//                 The Name header is IGNORED for bundles -- the bundle's real
//                 trainer name wins, because renaming would split the save's
//                 trainer from the mons' OT copies and make the whole party
//                 disobedient. Model remains meaningful: it rides as its own
//                 header exactly as for Showdown submissions. The value is
//                 untrusted remote text: it must base64-decode cleanly
//                 (bounded, strict) to populate save_bundle below, and the
//                 decoded bytes are then validated field-by-field host-side
//                 (PartyBundle::Validate) before anything touches a save.
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
  // Decoded "SaveBundle:" bytes; nullopt when the payload carried no such
  // header or its base64 was malformed. When set, this payload is a bundle
  // submission: showdown_text is empty by construction and trainer_name is
  // ignored (the bundle's own trainer identity wins). The bytes are decoded
  // but NOT yet validated -- hand them to InjectGuestBundle, which runs
  // PartyBundle::Validate before anything else.
  std::optional<std::vector<u8>> save_bundle;
};

// Joiner side: wrap showdown_text with the header block. An empty (or
// whitespace-only) name and an absent model produce a bare payload identical
// to the old format. Newlines in the name are replaced with spaces so they
// cannot forge framing; a model without a value (or <= 0) emits no line.
std::string BuildTeamSubmissionPayload(const std::string& showdown_text,
                                       const std::string& trainer_name,
                                       std::optional<int> model = std::nullopt);

// Joiner side, "Use my save": wrap a party bundle (PartyBundle::Extract's
// output) as a bundle payload -- the SaveBundle header, an optional Model
// header, the blank line, an empty body. No Name header is ever emitted: the
// bundle's own trainer identity is authoritative. Base64 contains no newline,
// so a bundle cannot forge framing.
std::string BuildBundleSubmissionPayload(const std::vector<u8>& bundle,
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

// Host side, bundle counterpart of InjectGuestTeam: validate an untrusted
// remote party bundle strictly (PartyBundle::Validate -- exact length, party
// count 1..6, every mon through the existing checksum-verifying reader, name
// field normalized), build the disposable save from the bundled EMERALD
// template with the bundle's party AND trainer identity (PartyBundle::
// BuildSave; no OT re-stamping -- see PartyBundle.h), and write it through
// the same verified-write path. The .hostteam stash / .guestteam marker /
// cleanup lifecycle is IDENTICAL to InjectGuestTeam's: from the lifecycle's
// point of view the disposable save is just another injected guest team, and
// RestoreHostTeam takes it back out the same way. Refuses when the guest
// slot currently holds an FRLG save, for the same reason InjectGuestTeam
// does: that socket's ROM cannot run the Emerald-template disposable save.
bool InjectGuestBundle(const std::vector<u8>& bundle, int device, std::string* status);

// HOST gate for the one-tap FORMAT feature (FormatRules.h): when the host's
// MAIN_XD_FORMAT key is Orre Colosseum, validate the parties the HOST brings
// to the room -- the port-2 save (the host's own team) AND the port-3 save
// (the guest-slot fallback, played whenever the guest never submits) -- since
// both will be played under the room's rules. Returns true when hosting may
// proceed; false with *reason set to a user-displayable sentence naming the
// offending mon or item AND which slot it sits in. Callers block hosting on
// false (desktop: ModalMessageBox in MainWindow::NetPlayHost; Android: the
// connection-error surface in nativeHost -- the same place the FRLG/
// disposable-save hosting refusals ship), strictly BEFORE the disposable-save
// swap and the server, so a refusal leaves no swapped state behind.
//
// With format=Free this returns true after one int compare -- no file is read,
// no validation runs, hosting is byte-identical to a build without formats.
// Deliberately lenient about anything that is not a rules violation: a port
// with no save, an unreadable/FRLG save (whose party the Emerald-offset
// readers cannot decode) or an empty party is SKIPPED, not refused -- those
// conditions exist in Free too and have their own handling; this gate only
// answers "is a readable party Orre-legal".
bool ValidateHostPartiesForFormat(std::string* reason);

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

// Boot-boundary self-heal for a session that DIED without its cleanup: the
// app was killed or crashed while a room was open, so RestoreHostTeam never
// ran, and the socket save still holds the last guest's party with .guestteam
// / .hostteam lying beside it. Field case: a killed 1.4.1 session left the
// opponent's full team as the port-3 save, and the next SOLO boot played it.
// SelfHealStaleGuestState only runs when the NEXT injection arrives, which a
// solo boot never triggers -- this is the missing boundary heal.
//
// For each GBA device with lifecycle files pending, runs the full purge
// (restore the host's save, scrub .bak/.tmp/NetPlayTemp copies, drop the
// marker). When no lifecycle is pending it still deletes stray NetPlayTemp
// saves -- a JOINER's machine keeps the host's synced saves there when a
// session is killed, with no marker to say so.
//
// Safe to call from any session boundary, as often as wanted: it is a no-op
// unless leftovers exist, and it refuses to touch anything while netplay is
// running (markers then belong to the LIVE session) or while emulation owns
// the saves. DisposableSave::HealLeftoverSession calls this first, so every
// existing heal boundary (launcher open, solo boot, host start, save import)
// gets it automatically.
void HealLeftoverGuestState();
}  // namespace XDNetplay
