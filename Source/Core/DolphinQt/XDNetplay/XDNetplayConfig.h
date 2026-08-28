// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace NetPlay
{
class NetPlayServer;
}

// Non-UI helpers shared by the XD Netplay launcher. They force the small set
// of local settings a Pokemon XD GBA-link battle needs, mirror the Android
// build's netplay start forcing, and seed the bundled Sys templates (team
// saves and the VS-mode memory card file) into the user directory.
namespace XDNetplay
{
// Pokemon XD: Gale of Darkness (USA).
constexpr char XD_GAME_ID[] = "GXXE01";
// SHA-1 of the official 16 KiB AGB boot ROM.
constexpr char OFFICIAL_GBA_BIOS_SHA1[] = "300c20df6731a33952ded8c436f7f186d25d3492";

// True if the game id belongs to Pokemon XD (USA), including derived ids that
// merely append a revision to the base id.
bool IsXdGameId(const std::string& game_id);

// ---- Public-lobby naming, shared with the Android launcher ----

// Session-name format for every XD room the launcher publishes to Dolphin's
// public session index:
//
//     XD [OC] <nickname>            ("OC" = Open Challenge)
//     [Orre] XD [OC] <nickname>     when the host's Format pick is Orre
//                                   Colosseum
//     [OU] XD [OC] <nickname>       when the host's Format pick is OU
//                                   (format_key_value = the caller's read of
//                                   Config::MAIN_XD_FORMAT)
//
// The IDENTICAL format lives on the Android side in XdMatchmaker.kt
// (SESSION_NAME_PREFIX). Keep the two in sync -- it is the only thing that
// lets a human scanning the session browser and the one-button auto-matcher
// agree on what an XD room looks like.
//
// Matching itself never keys off the name: it keys off player_count /
// in_game / has_password, so a room named anything else is still joinable.
// The tag exists purely so the lobby reads sensibly to people -- including the
// "[Orre] " and "[OU] " prefixes, which are human-readable labels only; this
// patch adds no matchmaking filter for them, and a Free room's name is
// byte-identical to before the Format feature existed.
std::string MakeOpenSessionName(const std::string& nickname, int format_key_value);

// True when an index entry's published "game" string looks like Pokemon XD.
//
// Note that the index's game field is NOT a game id. NetPlayServer::SetupIndex
// publishes m_selected_game_name, which desktop fills from GameListModel's
// netplay name ("Pokemon XD: Gale of Darkness (GXXE01, USA)") while this
// fork's Android host fills it with the plain long name (no id at all). So a
// server-side game filter cannot be trusted and we substring-match instead --
// the exact same set of needles as LobbySession.isXdBattle on Android.
bool LooksLikeXdSession(const std::string& published_game_name);

// Force the local settings an XD link battle expects: GBA ROM paths for ports
// 2/3 (re-asserted only while the imported dump still exists), SI devices
// (pad, GBA, GBA), cheats on for the $XD OU Fixes code, and netplay tuned to
// fixed-delay with a small buffer and no UPnP. Saves the config.
bool EnsureGbaConfig();

// Mirror of the Android nativeStartGame forcing: fixed two-player pad map
// (host pad + host GBA + guest GBA), GBA slots 2/3 enabled, save sync, remote
// GBA hiding, cheats and code sync on. Does NOT call RequestStartGame.
void ApplyStartForcing(NetPlay::NetPlayServer* server);

// Resolve the GBA BIOS path (config, else the GBA user directory) and verify
// it is the official dump. Optionally reports the resolved path. A negative
// result is not fatal: the bundled open-source BIOS in Sys/GBA is a working
// fallback for the XD link.
bool CheckOfficialBios(std::string* path_out);

// Validate an arbitrary file as the official GBA BIOS (size and SHA-1).
bool IsOfficialBiosFile(const std::string& path);

// Silent quality-of-life import with deliberately zero UI: if no official
// BIOS is configured yet, scan a directory for a 16 KiB *.bin whose SHA-1
// matches the official dump, copy it into the GBA user directory, and set
// MAIN_GBA_BIOS_PATH. The bundled open-source BIOS already works without
// this; it only adopts a dump the user happened to leave next to the ISO.
// Returns true once an official BIOS is configured.
bool AutoImportOfficialBios(const std::string& directory);

// Validates the given file as the official BIOS and installs it (copy into the
// GBA user dir + config). Returns false if it is not the official dump.
bool ImportOfficialBios(const std::string& path);

// True when the local player's GBA (GBA 1) has at least one button mapped.
bool GbaInputMapped();

// Loads Dolphin's default keyboard/controller mapping onto GBA 1 and saves it.
bool ApplyDefaultGbaInput();

// Copy the bundled EMERALD-2/3.sav templates next to the imported Emerald
// dump's derived save paths for ports 2 and 3. Existing saves are never
// overwritten; missing Sys templates are tolerated.
bool SeedTeamSaves();

// Copy the bundled XD VS-mode save into GC memory card A's USA directory if
// it is not already present. Missing Sys templates are tolerated.
bool SeedVsModeGci();
}  // namespace XDNetplay
