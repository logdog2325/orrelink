// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Automatic Wiimmfi redirection for Pokemon Battle Revolution.
//
// PBR's online mode talks to Nintendo's long-dead GameSpy backend. The community keeps it alive
// by redirecting the hardcoded hostnames at Wiimmfi, which is exactly what a "patched" PBR disc
// is: the stock disc with 30 strings rewritten inside main.dol and nothing else changed.
//
// Rather than asking users to source a pre-patched disc, we carry that payload ourselves and
// apply it as Riivolution <memory> patches at boot. Riivolution memory patches are written into
// RAM after the DOL has been loaded, so the disc file is never touched and every container
// format (ISO/WBFS/RVZ/CISO/NFS/...) works with no conversion.
//
// See PbrWiimmfiData.h for the payload itself.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "DiscIO/RiivolutionParser.h"

struct BootParameters;

namespace DiscIO
{
class VolumeDisc;
}

namespace XDNetplay::PbrWiimmfi
{
constexpr char GAME_ID_USA[] = "RPBE01";
constexpr char GAME_ID_PAL[] = "RPBP01";
constexpr char GAME_ID_JPN[] = "RPBJ01";

enum class DiscStatus
{
  // Not Pokemon Battle Revolution.
  NotPbr,
  // Pokemon Battle Revolution, but we have no payload for this region (RPBJ01).
  UnsupportedRegion,
  // Supported region carrying the stock Nintendo hostnames; we patch it at boot.
  Clean,
  // Supported region that already carries the Wiimmfi payload (a pre-patched disc).
  AlreadyPatched,
  // Supported region, but main.dol could not be inspected. We patch anyway; see below.
  Unknown,
};

// True for any Pokemon Battle Revolution disc, including the region we cannot patch.
bool IsPbrGameID(std::string_view game_id);

// True if a built-in Wiimmfi payload exists for this game ID.
bool IsSupportedGameID(std::string_view game_id);

// The built-in Wiimmfi patch set for a game ID, or an empty vector for anything we do not know
// about (including RPBJ01, which callers should report as an unsupported region).
std::vector<DiscIO::Riivolution::Patch> GetPatches(std::string_view game_id);

// Reads the relevant fields out of the disc's main.dol to work out whether the payload is
// already present. Only ~1 KiB is read, so this is cheap even on compressed containers.
DiscStatus ExamineDisc(const DiscIO::VolumeDisc& volume);

// Attaches the built-in payload to a disc boot if it is a PBR disc we support. Returns true if
// patches were attached. Safe to call for every boot: non-PBR titles are ignored.
bool ApplyToBootParameters(BootParameters* boot_params);
}  // namespace XDNetplay::PbrWiimmfi
