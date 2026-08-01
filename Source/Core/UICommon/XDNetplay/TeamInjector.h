// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace XDNetplay
{
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
bool InjectGuestTeam(const std::string& showdown_text, int device, std::string* status);

// Put the host's own team back after a netplay room closes.
//
// A guest's submitted team overwrites the host's socket-3 save, and without
// this it would still be there next time the host played solo (or hosted
// someone who submitted nothing, who would then battle with a stranger's
// team). The first injection of a session stashes the host's save alongside
// it; this restores that stash and removes it. No stash means the host never
// had a guest team written this session, so nothing happens.
void RestoreHostTeam(int device);
}  // namespace XDNetplay
