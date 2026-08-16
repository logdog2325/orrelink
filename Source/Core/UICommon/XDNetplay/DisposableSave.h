// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

// Disposable host saves for netplay (completes the community privacy review's
// original design).
//
// THE GAP THIS CLOSES: Dolphin netplay syncs the HOST's GBA saves to every
// client byte-for-byte (NetPlayServer::SyncSaveData reads the socket save
// files at start). With the bundled template saves that was privacy-neutral;
// since "Import my save" a host's COMPLETE real save -- boxes, inventory,
// progress, secret ID -- was copied to every joiner's machine for the session.
//
// THE FIX: when a hosting flow begins on a machine whose port-2 and/or port-3
// save is a USER IMPORT (the MAIN_XD_IMPORTED_SAVE_2/3 config keys), that
// port's save is replaced FOR THE SESSION by a disposable save built with the
// existing party-bundle machinery: PartyBundle::Extract(imported save) ->
// PartyBundle::BuildSave(bundled Emerald template, bundle). The disposable
// carries exactly the party and trainer identity -- what a battle reveals
// anyway -- and the full save never enters the synced save path. The import is
// stashed as <save>.netplayorig and put back when the room closes.
//
// LAYERING: strictly OUTSIDE TeamInjector's guest-team stash/restore, which is
// untouched. A guest submission during the session injects into the port-3
// DISPOSABLE: TeamInjector stashes the disposable as <save>.hostteam and its
// end-of-session purge restores it; this layer then restores the ORIGINAL
// import over that. Ordering is guaranteed by construction: the OnRoomClosed
// hooks call RestoreHostTeam(2) first and EndNetplaySession() second, so this
// layer's deferred-restore callback is registered after TeamInjector's on the
// same Core-state event and always runs after it (Common::HookableEvent
// triggers listeners in registration order).
//
// SOLO boots keep playing the real imported save -- that is the point of
// importing. Only hosting flows swap in disposables. Non-imported ports
// (bundled/team-editor saves) are never touched.
//
// The ".netplayorig" name is PROVABLY outside every cleanup set, verified the
// same way ".preimport" was: TeamInjector's purge deletes only .hostteam /
// .guestteam / .bak / .tmp / NetPlayTemp*.sav (TeamInjector.cpp PurgeNow),
// SaveImport writes .preimport and never deletes anything else, the verified
// writers touch only .tmp / .bak, and netplay's own temp handling deletes only
// its NetPlayTemp*.sav staging copies. No code path in the tree globs or
// deletes *.netplayorig except this module's own restore.
namespace XDNetplay::DisposableSave
{
// Stash of the imported save for the duration of one hosted netplay session.
constexpr const char* NETPLAY_ORIG_SUFFIX = ".netplayorig";

// Crash self-heal: if a previous session died before its restore ran, a
// <save>.netplayorig is still on disk and the socket save still holds the
// disposable. Restore the original (first-stash-wins: the leftover stash is
// restored, never re-stashed over). Call at every session boundary that could
// precede a boot -- the same hooks BattleCustomizer::BeginSession uses
// (desktop: EnsureGbaConfig; Android: nativeHost and the solo-boot
// nativePrepareForBoot) -- so a solo boot after a crash plays the real import
// again. For the port-3 slot a leftover TeamInjector lifecycle (.guestteam /
// .hostteam) is finished first via RestoreHostTeam, then the import goes back
// over its result. No-op when emulation is running or nothing is stashed.
void HealLeftoverSession();

// HOST side, called when a hosting flow begins -- BEFORE the NetPlayServer
// exists, so the disposable is on disk before any guest can join, before any
// TeamData submission can inject into port 3 (the injection then stashes the
// DISPOSABLE, not the import), and long before CollectSaveSyncInfo /
// SyncSaveData read the socket saves at start. Desktop: MainWindow::
// NetPlayHost; Android: nativeHost.
//
// For each imported port: heal any leftover stash, extract the party bundle
// from the import, build the disposable from the bundled Emerald template,
// stash the import ONCE as <save>.netplayorig (first-stash-wins), and write
// the disposable through the verified-write path.
//
// Returns false (with *error user-displayable) and unwinds any port already
// swapped when a disposable cannot be built -- notably for a FireRed/LeafGreen
// import, which PartyBundle::Extract refuses (its party lives at different
// section offsets): hosting is refused loudly rather than silently syncing the
// full FRLG save. Ports without an import are untouched and never fail.
bool PrepareForHosting(std::string* error);

// End-of-session restore: put the original imports back and delete the
// stashes. Call from the OnRoomClosed hooks on BOTH platforms, strictly AFTER
// XDNetplay::RestoreHostTeam(2) (see the layering note above). Idempotent and
// harmless on machines that never swapped (joiners, non-imported hosts).
//
// Rides the same deferred-until-Uninitialized pattern as the team cleanup: the
// live mGBA cores rewrite the socket saves at teardown, so when a room closes
// mid-battle the work waits for Core::State::Uninitialized and runs then.
void EndNetplaySession();
}  // namespace XDNetplay::DisposableSave
