// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>

namespace XDNetplay::BattleCustomizer
{
// ---------------------------------------------------------------------------
// Cosmetic battle selectors ("$OrreLink Battle Style")
// ---------------------------------------------------------------------------
//
// Three purely cosmetic choices for the XD GBA-vs-GBA netplay battle -- the
// trainer models, the battle music and the battle location -- realized as ONE
// host-assembled ActionReplay code that rides the EXISTING enabled-codes sync
// (NetPlayServer::SyncCodes ships codes by full text, NETPLAY_SYNC_CODES is
// already forced on). The per-player choice happens at ASSEMBLY time on the
// host, never as per-client codes, so both clients always execute an identical
// instruction stream and the feature cannot introduce a desync.
//
// Who picks what:
//  - the HOST picks its own model, the music and the location in the launcher
//    (persisted in the MAIN_XD_STYLE_* config keys);
//  - the GUEST picks its own model. The choice travels as a "Model:" header
//    line in the existing MessageID::TeamData payload (grammar documented in
//    TeamInjector.h) and is stashed here via SetGuestModel;
//  - the host's "Guest model" dropdown is ONLY the fallback for a guest that
//    never submitted one -- mirroring exactly how the socket-3 team fallback
//    works.
//
// "Game default" genuinely means the code is ABSENT. No section ever writes a
// vanilla value back, so an all-default session is byte-for-byte a stock
// session and the feature can never regress one. With zero enabled codes,
// SyncCodes still runs and sends an empty list -- the guest clears its synced
// set, which is equally stock.
//
// Where the code lives: the on-disk local game INI, User/GameSettings/
// GXXE01.ini. That is the ONE place a session-generated code can go, because
// both consumers read it straight from disk: NetPlayServer::SyncCodes builds
// its own IniFile pair from Sys + User GameSettings, and the host's own boot
// re-reads the same files (PatchEngine::LoadPatches -> ActionReplay::
// LoadCodes). Host and wire therefore always agree. The block is written with
// surgical GetLines/SetLines edits (the ARCodeWidget::SaveCodes idiom) so the
// user's own codes in that file are never touched.
//
// Cheats gating: nothing ships unless the host's MAIN_ENABLE_CHEATS is true
// when RequestStartGame runs (SetupNetSettings snapshots it; guests inherit
// it through the netplay config layer). The launcher equates that flag with
// its "OU rules ($XD OU Fixes)" toggle, so when a cosmetic is active while
// the OU toggle is OFF, PrepareForStart forces cheats on and RegenerateIni
// writes "$XD OU Fixes" into the local [ActionReplay_Disabled] -- the local
// pass of ReadEnabledAndDisabled can disable a Sys-bundled code -- keeping
// the OU patches off while the cosmetic block runs. EndSession undoes both.
//
// Every address in the generated block is NTSC-U (GXXE01) ONLY. The block is
// keyed to the GXXE01 local INI, so a future PAL/JP experiment cannot inherit
// these patches.

// "tested-safe" entries are things the vanilla game itself does in this
// context (vanilla battling trainers covered by Ralf's community-circulated
// modifier codes; the retail VS colosseum pool; looping battle-context BGM
// streams). "experimental" entries are valid table data that vanilla never
// uses quite this way -- offer them behind an "Experimental" divider until
// smoke-tested. IDs the research EXCLUDED (crash/hang/silence risks) are not
// in these tables at all: validation is table membership, so they can neither
// be offered by the launcher nor smuggled in by a guest submission.
enum class Tier
{
  TestedSafe,
  Experimental,
};

struct StyleOption
{
  // models: trainer model id (index into the 0x44-entry trainer_pkx_data
  //         table at 0x8040CE88);
  // music:  BGM stream id (the id space of XD's RoomBGM/battle BGM fields);
  // venues: battlefield-table index (consumed via GetBattleField).
  int id;
  // Launcher display name. Music names marked provisional in the .cpp must be
  // finalized by a listening pass through the selector before release.
  const char* name;
  Tier tier;
};

// The tables, tested-safe entries first, then experimental -- a dropdown can
// insert its divider at the first tier change. "Game default" is NOT an entry:
// it is the absence of a selection (id <= 0 / std::nullopt everywhere here).
std::span<const StyleOption> ModelTable();
std::span<const StyleOption> MusicTable();
std::span<const StyleOption> VenueTable();

// Table membership. Anything else -- including the excluded ids 0x00/0x0A
// (no-model enum entries, expect invisible trainer or hang), ids past the
// 0x44-entry model table (garbage pointer read), venue 0 / >= 66 (NULL
// battlefield -> hang risk) and non-looping jingle BGM (permanent silence) --
// must be treated as ABSENT by callers: silently fall back, never clamp.
bool IsValidModelId(int id);
bool IsValidMusicId(int id);
bool IsValidVenueId(int id);

// Pure assembly: returns the op lines of the "$OrreLink Battle Style" AR code
// (one "AAAAAAAA VVVVVVVV" per line, '\n'-separated, no "$" name line), or an
// EMPTY string when every field is absent -- the caller must then emit no code
// at all. std::nullopt (or any id that fails the predicates above) means
// "game default" for that field and contributes NO lines; each field is fully
// independent.
//
// p1_model patches the "Player" side of the battle, p2_model the "Opponent"
// side. Which physical player (host vs guest) renders on which side in the
// GBA-vs-GBA mode is still unverified -- if an emulator test shows it
// reversed, swap the two arguments AT THE CALL SITE (RegenerateIni); the code
// lines themselves do not change.
std::string GenerateCodeBlock(std::optional<int> p1_model, std::optional<int> p2_model,
                              std::optional<int> bgm, std::optional<int> venue);

// The host's four launcher selections. 0 (the config default) = game default.
struct Selection
{
  int host_model = 0;
  int guest_model_fallback = 0;
  int music = 0;
  int venue = 0;
};

// Reads the four MAIN_XD_STYLE_* config keys.
Selection ConfigSelection();

// Session-boundary scrub: clears the stashed guest model and removes any
// "$OrreLink Battle Style" block a crashed session left orphaned in
// User/GameSettings/GXXE01.ini (plus its enabled line and the OU-Fixes local
// disable), so stale cosmetics can never leak into a solo boot or the next
// session. Call when a hosting flow begins (desktop: EnsureGbaConfig;
// Android: nativeHost). Do NOT call while a room is open -- it would discard
// a guest's submitted model.
void BeginSession();

// Stash the guest's submitted model (host side, netplay thread). Called on
// EVERY TeamData arrival: a value that fails IsValidModelId -- including
// nullopt, i.e. no "Model:" header -- RESETS the stash to "no preference", so
// the latest submission is always the guest's current word and the host's
// fallback dropdown wins again. The id is untrusted remote input; invalid
// values are dropped here, never clamped. Cleared by Begin/EndSession.
void SetGuestModel(std::optional<int> id);

// Rewrites the "$OrreLink Battle Style" block in User/GameSettings/GXXE01.ini
// from sel (+ the stashed guest model, which wins over sel.guest_model_
// fallback), enables it via [ActionReplay_Enabled], and -- only while the
// block is active AND ou_enabled is false -- disables the Sys-bundled
// "$XD OU Fixes" via [ActionReplay_Disabled]. An all-default call is a pure
// removal. status, when given, receives a one-line human-readable summary.
bool RegenerateIni(const Selection& sel, bool ou_enabled, std::string* status);

// RegenerateIni driven entirely by config: selection from the MAIN_XD_STYLE_*
// keys, ou_enabled from MAIN_ENABLE_CHEATS (corrected for a force applied by
// PrepareForStart earlier in this session, so a second Start in the same room
// cannot mistake our own forcing for the user's OU choice). This is the call
// for the host's TeamData handlers: regenerate on every submission arrival so
// a model submitted any time before Start is honored.
bool RegenerateFromConfig(std::string* status);

// The pre-start hook, host side, strictly before RequestStartGame (desktop:
// ApplyStartForcing; Android: nativeStartGame): regenerates the block once
// more, then -- only when the block is non-empty -- forces MAIN_ENABLE_CHEATS
// on so the block actually loads and ships (see the gating note above).
// A submission that arrives after this point is rejected by the server while
// the battle runs, so the synced set can never diverge mid-session.
void PrepareForStart();

// End-of-session cleanup, next to RestoreHostTeam in the OnRoomClosed hooks:
// clears the guest stash, removes the block/enabled/disabled lines (pure
// removal -- the file is stock again), and gives MAIN_ENABLE_CHEATS back to
// the user if PrepareForStart forced it on this session.
void EndSession();
}  // namespace XDNetplay::BattleCustomizer
