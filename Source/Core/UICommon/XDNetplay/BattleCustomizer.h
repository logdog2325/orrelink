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
// it through the netplay config layer). Since the Format dropdown absorbed
// the old standalone "OU Fixes" toggle, that flag is DERIVED state for XD
// sessions: PrepareForStart reconciles it to (block active || format == OU),
// in both directions, remembering the pre-session value. When the block is
// active while the format is NOT OU, RegenerateIni writes "$XD OU Fixes"
// into the local [ActionReplay_Disabled] -- the local pass of
// ReadEnabledAndDisabled can disable a Sys-bundled code -- keeping the OU
// patches off while the block runs. EndSession restores the remembered flag.
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

// Whether the disc ships a pre-rendered pre-battle bust for this model: true
// exactly for the six GBA player models (0x04..0x09). Every other model
// battles fine (field-proven) but has no close-up portrait, so the connection
// and team screens show none for it -- the UI labels such picks
// "(no portrait)" instead of presenting a tier.
bool ModelHasPortrait(int model_id);

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
// p1_class/p2_class: each side's SAVE class (1 FRLG-m .. 6 E-f, 0 unknown),
// derived from the save that will occupy the port; used to remap the
// pre-battle bust widget tables so the connection-screen close-up matches the
// picked model. Bust remap is limited to the six GBA player models (the menu
// binds only those six bust widgets) and needs the two sides' classes to
// differ; outside those bounds the bust stays vanilla while the battle model
// still changes.
// hide_default_busts: also zero the class-0 default bust pair (widgets
// 0x201/0x208 -- the GC-protagonist portrait every bust descriptor defaults
// to). The team-preview screen draws its busts from those defaults before any
// per-class re-crop, so in a GBA-vs-GBA session they are ALWAYS the wrong
// head; the format pins guarantee GBA-vs-GBA, so RegenerateIni passes true
// whenever a format rules-pin is active. Portrait-less picks force the same
// zeroing regardless (the 1.4.2 behavior).
std::string GenerateCodeBlock(std::optional<int> p1_model, std::optional<int> p2_model,
                              std::optional<int> bgm, std::optional<int> venue,
                              int p1_class = 0, int p2_class = 0,
                              bool hide_default_busts = false);

// ---- FORMAT seam ------------------------------------------------------------
// The IN-GAME rules layer of the community formats -- battle type, level
// preset, entries/entry mode, clauses -- is pinned by AR writes emitted from
// FormatRuleLines below. THIS is the seam that
// patch lands in: RegenerateIni appends whatever this helper returns to the
// assembled "$OrreLink Battle Style" block, and the helper consults the format
// key (Config::MAIN_XD_FORMAT, values in FormatRules.h) to decide what to
// emit: the six community formats pin the menu globals, the battle type
// (Double for Orre shapes, Single for Hoenn shapes) and the whole Custom-1
// ruleset slot (level preset, entries, entry mode 0); Free/OU emit nothing
// and stay byte-identical to stock. Species/item ban legality is NOT this
// seam's business -- FormatRules validates that at the submission/host gates
// -- but the battle-time clauses (Sleep, Freeze, Self-KO, Species, Item)
// ARE covered here: the pinned stock clause bytes have them all ON, so the
// game itself enforces them in battle.
std::string FormatRuleLines();

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

// Room-open entry: performs the same scrub as ScrubLeftovers AND marks the
// netplay session active (IsNetplaySessionActive). Call when the netplay room
// actually opens -- desktop: NetPlayDialog::show (host and joiner alike);
// Android: the end of nativeHost, and nativeJoin's external-join branch after
// the client connects. Do NOT call from solo-boot or launcher boundaries
// (that is ScrubLeftovers' job -- claiming the lifecycle there makes the
// solo-cleanup hook stand down and strands the cheats-flag restore), and NOT
// while a room is open -- it would discard a guest's submitted model.
void BeginSession();

// Stash the guest's submitted model (host side, netplay thread). Called on
// EVERY TeamData arrival: a value that fails IsValidModelId -- including
// nullopt, i.e. no "Model:" header -- RESETS the stash to "no preference", so
// the latest submission is always the guest's current word and the host's
// fallback dropdown wins again. The id is untrusted remote input; invalid
// values are dropped here, never clamped. Cleared by Begin/EndSession.
void SetGuestModel(std::optional<int> id);

// Boundary scrub WITHOUT claiming the netplay lifecycle: clears the stashes
// and strips any leftover "$OrreLink Battle Style" block a crashed session
// left in the local INI. This is the call for solo-boot and launcher-open
// boundaries -- BeginSession does the same scrub but also marks a netplay
// session as active, which makes the solo-cleanup core-state hook stand down
// and EndSession (the cheats-flag restore) wait for a room-closed event that
// a solo boot never gets. No-op while a netplay session is active: whatever
// is stashed then belongs to the LIVE room, not to a crashed one.
void ScrubLeftovers();

// True from BeginSession (a netplay room opened in this process) until
// EndSession. The heal machinery consults this: NetPlay::IsNetPlayRunning()
// is GAME-scoped (true only while a netplay game runs), yet guest teams are
// injected exactly while it is false -- in the lobby and between battles --
// so lifecycle files on disk are only "leftovers" when this is false too.
bool IsNetplaySessionActive();

// Rewrites the "$OrreLink Battle Style" block in User/GameSettings/GXXE01.ini
// from sel (+ the stashed guest model, which wins over sel.guest_model_
// fallback), enables it via [ActionReplay_Enabled], and -- only while the
// block is active AND ou_enabled is false -- disables the Sys-bundled
// "$XD OU Fixes" via [ActionReplay_Disabled]. An all-default call is a pure
// removal. status, when given, receives a one-line human-readable summary.
// include_format_rules=false is the REMOVAL mode: the format's rules-pin
// lines are skipped even when a format is picked, so an all-default call
// really does strip everything of ours from the INI. Session-boundary scrubs
// and end-of-session cleanup use it -- without it, a picked format would
// leave its rules block at rest in the file between sessions, where any
// cheats-on GXXE01 boot outside our flows would load it.
bool RegenerateIni(const Selection& sel, bool ou_enabled, std::string* status,
                   bool include_format_rules = true);

// RegenerateIni driven entirely by config: selection from the MAIN_XD_STYLE_*
// keys, ou_enabled from MAIN_XD_FORMAT == FORMAT_OU (the Format dropdown is
// the OU choice now; the flag PrepareForStart reconciles cannot distort it).
// This is the call for the host's TeamData handlers: regenerate on every
// submission arrival so a model submitted any time before Start is honored.
bool RegenerateFromConfig(std::string* status);

// The pre-start hook, host side, strictly before RequestStartGame (desktop:
// ApplyStartForcing; Android: nativeStartGame): regenerates the block once
// more, then reconciles MAIN_ENABLE_CHEATS to what this session needs -- on
// when the block is non-empty or the format is OU, off otherwise (see the
// gating note above; the off-direction is what keeps a stale flag from the
// removed OU toggle from silently shipping OU patches under "Free").
// A submission that arrives after this point is rejected by the server while
// the battle runs, so the synced set can never diverge mid-session.
void PrepareForStart();

// End-of-session cleanup, next to RestoreHostTeam in the OnRoomClosed hooks:
// clears the guest stash, removes the block/enabled/disabled lines (pure
// removal -- the file is stock again), and puts MAIN_ENABLE_CHEATS back to
// its pre-session value if PrepareForStart reconciled it this session.
void EndSession();
}  // namespace XDNetplay::BattleCustomizer
