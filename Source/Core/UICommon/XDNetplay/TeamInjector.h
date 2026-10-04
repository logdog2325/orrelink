// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "UICommon/XDNetplay/FormatRules.h"

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
  // "RaiseLevel100: 1" header: the guest asked the host to raise every
  // under-level mon to Lv. 100 before injecting. Honored ONLY when the host's
  // format is a level-100 format, and only ever RAISES (never lowers -- that
  // would break legality). Opt-in: "Level 100 or lower" is legal.
  bool raise_to_level_100 = false;
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
                                       std::optional<int> model = std::nullopt,
                                       bool raise_to_level_100 = false);

// Joiner side, "Use my save": wrap a party bundle (PartyBundle::Extract's
// output) as a bundle payload -- the SaveBundle header, an optional Model
// header, the blank line, an empty body. No Name header is ever emitted: the
// bundle's own trainer identity is authoritative. Base64 contains no newline,
// so a bundle cannot forge framing.
std::string BuildBundleSubmissionPayload(const std::vector<u8>& bundle,
                                         std::optional<int> model = std::nullopt,
                                         bool raise_to_level_100 = false);

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
                     std::string* status, bool raise_to_level_100 = false);

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
bool InjectGuestBundle(const std::vector<u8>& bundle, int device, std::string* status,
                       bool raise_to_level_100 = false);

// The HOST's in-room counterpart of the joiner's "Name:" header: rename the
// trainer in the GBA port 2 save (the host's own socket -- the one the room
// syncs at start) and re-stamp every party mon's OT name to match, through
// the same verified write as every injection. The bundled templates ship as
// "Player1"/"Player2", and until this existed the only place a host could
// change that was the team editor's name field, which nobody found -- the
// field report was "the host is always Player 1". Refuses while a game runs
// (the mGBA core owns the file). *status always gets a human-readable line.
bool RenameHostTrainer(const std::string& name, std::string* status);

// Current trainer name of the GBA port 2 save, or "" when unreadable. For
// prefilling the host's name field.
std::string HostTrainerName();

// The HOST's in-room counterpart of a joiner's team submission: parse a
// Showdown team and write it (and, if given, the trainer name) into the GBA
// port 2 save (the host's own socket, the one the room syncs at Start) through
// the same verified write RenameHostTrainer uses.
//
// PERSISTENT, like the Team Editor's Save: no .hostteam stash and no guest
// marker, because this IS the host's own save. When the room runs on a
// disposable save because the host imported a personal save, the write lands
// in the disposable and the import comes back untouched when the room closes.
// That is by design.
//
// Refuses while a game is running (the mGBA core owns the file), refuses an
// FRLG save (its party lives at other offsets, see InjectGuestTeam), and
// applies the HOST's format gate (FormatRules::ValidateSets with
// MAIN_XD_FORMAT) before anything is written. A port with no save yet is
// seeded from the bundled template, the same way the Team Editor does it.
//
// trainer_name "" keeps the save's current name; a name that sanitizes to
// nothing is dropped, not fatal. raise_to_level_100 is honoured only in a
// level-100 format and only raises. *status always receives a one-line
// human-readable result.
bool SubmitHostTeam(const std::string& showdown_text, const std::string& trainer_name,
                    std::string* status, bool raise_to_level_100 = false);

// The legality of the two parties a room plays under `format`: the host's own
// team (the port-2 save, device 1) and the GBA 2 slot (the port-3 save,
// device 2: the host's spare team, or a guest's submitted team). A verdict
// that was not judged reads ok. judge_slot=false leaves the slot unjudged
// (its team belongs to a player who left and is reset before any Start reads
// it).
//
// Free, OU and unknown values return at once: one int compare, no file read.
// (A Multi room judges its teams with CheckMultiTeams instead.) Deliberately
// lenient about anything that is not a rules violation: no gen3data.json, a
// port with no save, an unreadable/FRLG save
// (whose party the Emerald-offset readers cannot decode) or an empty party
// all pass -- those conditions exist in Free too and have their own
// handling; this only answers "is a readable party legal".
//
// File reads only, no UI and no config writes: safe under the netplay
// server's seat mutex (NetPlayServer::SetXdFormat runs it there, so no
// TeamData write can land between the slot snapshot and this read).
struct RoomTeamCheck
{
  FormatRules::Verdict host;
  FormatRules::Verdict slot;
};
RoomTeamCheck CheckRoomTeams(int format, bool judge_slot);

// ADVISORY host check when a room opens (desktop: MainWindow::NetPlayHost;
// Android: NetplaySession's first room screen), under the MAIN_XD_FORMAT
// key: CheckRoomTeams(format, true). Returns true when both parties pass;
// false with *reason set to one line for the host, "Your team is not <Format>
// legal: <reason>." or "Your spare team is not <Format> legal: <reason>."
// (the host's own team first). The room opens either way: the format can be
// changed in the room, and Start is where legality is enforced.
bool ValidateHostPartiesForFormat(std::string* reason);

// The lines both room UIs show after the host changes the room's format
// (NetPlayServer::SetXdFormat), from the CheckRoomTeams it ran. host_lines go
// to the host only; guest_note goes to the seated opponent only, when
// slot_is_guest and their submitted team breaks the new format. Nothing is
// cleared: a resubmission overwrites the team, and switching back to a format
// it passes unblocks Start.
struct FormatChangeNotes
{
  std::vector<std::string> host_lines;
  std::string guest_note;
};
FormatChangeNotes DescribeFormatChange(int format, const RoomTeamCheck& check, bool slot_is_guest,
                                       const std::string& guest_name);

// The Start gate of both room UIs: "" when CheckRoomTeams(format, true) passed,
// else the one line the host sees ("Can't start: ..."), with the verdict's
// reason for the host's own teams. When the seated opponent's team is the
// problem, *guest_note receives the line to send them (with the reason; the
// host's line leaves it out, as DescribeFormatChange does).
std::string StartFormatRefusal(int format, const RoomTeamCheck& check, bool slot_is_guest,
                               const std::string& guest_name, std::string* guest_note);

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

// XD Netplay, host, mid-room: the guest slot holds the team of a player who no longer holds the
// opponent seat. Puts the slot back to the host's spare team (the same full purge as
// RestoreHostTeam) and forgets the guest's model pick, so the next opponent never plays or shows
// as the previous one. Only with emulation fully down: returns false and changes nothing
// otherwise. Unlike RestoreHostTeam it never arms a deferred purge, which could land after, and
// wipe, a newer submission. Idempotent. No UI and no alerts, so it is safe under the netplay
// server's seat lock.
bool ResetGuestSlot(int device);

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

// ---------------------------------------------------------------------------
// XD multi battles (four seats)
// ---------------------------------------------------------------------------
//
// A Multi room never writes a guest's team into the host's saves. Each seated
// joiner's TeamData lands in a private STAGE file keyed by that connection's
// serial (never reused, so a pid reused by a later joiner never inherits one):
//
//   <User>/XDNetplay/Multi/stage-<serial>.sav
//
// At Start, frozen and with emulation down, WriteMultiBootSaves copies the
// host's own port-2 save (read only) and the three seats' stages into the
// four BOOT COPIES NetPlayTemp1..4.sav. The server syncs exactly those, and
// the host's own cores boot from them (NetPlay::GetGBASavePath), so every
// machine boots identical bytes and the host's real saves are never opened
// for writing. The copies are scrubbed when the game stops (ArmTempPurgeOnStop),
// at once when a start fails, and by the launch heal; the stages when their
// player has left at the next Multi Start, when the room closes, and by the
// launch heal.

std::string MultiStagePath(u32 serial);

// Host side, NETPLAY thread, under the server's seat mutex. The stage
// counterparts of InjectGuestTeam / InjectGuestBundle: the same format gate
// and the same validation, built on a fresh copy of the bundled template for
// SI device `device` (the seat's port - 1). No stash, no mark: the file is
// the player's alone. *status as for the 1v1 paths.
bool InjectGuestTeamToStage(const std::string& showdown_text, const std::string& trainer_name,
                            int device, u32 serial, std::string* status,
                            bool raise_to_level_100 = false);
bool InjectGuestBundleToStage(const std::vector<u8>& bundle, int device, u32 serial,
                              std::string* status, bool raise_to_level_100 = false);

// The stage registry (its own lock; may be taken under the seat mutex): which
// serials have a stage, and the model each asked for (phase 2 uses it).
void RecordMultiStage(u32 serial, std::optional<int> model);
std::optional<int> MultiStageModel(u32 serial);
// Scrubs and deletes every stage file whose serial is not in keep.
void DropMultiStages(std::span<const u32> keep);
void DropAllMultiStages();

// Host UI thread, at a Multi Start: frozen, emulation fully down. Writes
// NetPlayTemp1..4: slot 0 = the host's port-2 save, slot k (1..3) = the stage
// of seat_serials[k-1], or the host's save for slot 1 when host_fills_seat2
// (MultiFillSeats). Then drops the stages of serials not in room_serials.
// false with *error when a source is missing or unwritable; nothing then
// boots from a half-written set (the caller scrubs).
bool WriteMultiBootSaves(const std::array<u32, 3>& seat_serials, bool host_fills_seat2,
                         std::span<const u32> room_serials, std::string* error);
// The same for a 1v1 start whose host watches: NetPlayTemp2 and NetPlayTemp3 (SI ports 2 and 3,
// GBA 1 and GBA 2) from the stages of seat_serials[0] and seat_serials[1]; the host's own saves
// are not read. Then drops the stages of serials not in room_serials.
bool WriteWatchBootSaves(const std::array<u32, 2>& seat_serials, std::span<const u32> room_serials,
                         std::string* error);
// Scrubs and deletes NetPlayTemp1..4.
void ScrubMultiBootSaves();
// Scrub the boot copies once emulation next reaches Uninitialized (the game
// that boots from them has stopped), or not.
void ArmTempPurgeOnStop();
void DisarmTempPurge();
bool IsTempPurgeArmed();

// The legality of a Multi start's four teams under `format`: the host's
// port-2 save and each seat's stage (the host's save for seat 2 when
// host_fills_seat2). Lenient like CheckRoomTeams: a missing or unreadable
// file is not a rules question.
struct MultiTeamCheck
{
  FormatRules::Verdict host;
  std::array<FormatRules::Verdict, 3> seats;
};
MultiTeamCheck CheckMultiTeams(int format, const std::array<u32, 3>& seat_serials,
                               bool host_fills_seat2);
}  // namespace XDNetplay
