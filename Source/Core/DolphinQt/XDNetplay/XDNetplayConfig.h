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

// Copy the bundled EMERALD-2/3.sav templates next to the imported Emerald
// dump's derived save paths for ports 2 and 3. Existing saves are never
// overwritten; missing Sys templates are tolerated.
bool SeedTeamSaves();

// Copy the bundled XD VS-mode save into GC memory card A's USA directory if
// it is not already present. Missing Sys templates are tolerated.
bool SeedVsModeGci();
}  // namespace XDNetplay
