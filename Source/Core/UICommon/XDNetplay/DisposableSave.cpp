// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/DisposableSave.h"

#include "UICommon/XDNetplay/BattleCustomizer.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/IOFile.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GBADetectLog.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif

#include "UICommon/XDNetplay/Gen3Save.h"
#include "UICommon/XDNetplay/PartyBundle.h"
#include "UICommon/XDNetplay/TeamInjector.h"

namespace XDNetplay::DisposableSave
{
namespace
{
#ifdef HAS_LIBMGBA
// TeamInjector's session bookkeeping suffixes. Their presence next to the
// port-3 save means that layer's injection round trip owns the file right now;
// this layer must never restore OVER a pending inner cleanup (the inner retry
// would later copy its stash -- the disposable -- straight back over the
// restored import). MUST stay in step with TeamInjector.cpp.
constexpr const char* HOST_STASH_SUFFIX = ".hostteam";
constexpr const char* GUEST_MARK_SUFFIX = ".guestteam";

// Same ROM resolution as TeamInjector / SaveImport: the socket's own ROM,
// falling back to the other socket's (a launcher-made setup points both at one
// dump). This matches what SyncSaveData reads at start in every configured
// setup, so the file swapped here is provably the file that would be synced.
std::string SavePathForDevice(int device)
{
  std::string rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device]);
  if (rom.empty() || !File::Exists(rom))
    rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device == 1 ? 2 : 1]);
  if (rom.empty() || !File::Exists(rom))
    return {};
  return HW::GBA::Core::GetSavePath(rom, device);
}

const Config::Info<std::string>& ImportedSaveKey(int device)
{
  return device == 1 ? Config::MAIN_XD_IMPORTED_SAVE_2 : Config::MAIN_XD_IMPORTED_SAVE_3;
}

// The bundled Emerald template the disposable is built from -- the same file
// the guest-bundle injection path uses for its disposable saves.
std::string TemplatePath(int device)
{
  return File::GetSysDirectory() +
         (device == 1 ? "XDNetplay/EMERALD-2.sav" : "XDNetplay/EMERALD-3.sav");
}

bool ReadFileBytes(const std::string& path, std::vector<u8>* out)
{
  File::IOFile file(path, "rb");
  if (!file)
    return false;
  out->resize(file.GetSize());
  return out->empty() || file.ReadBytes(out->data(), out->size());
}

void Log(const std::string& detail)
{
  GBADetectLog::LogPostSession("disposable " + detail);
}

// Whether TeamInjector's injection lifecycle currently owns this save.
bool InnerLifecyclePending(const std::string& save_path)
{
  return File::Exists(save_path + HOST_STASH_SUFFIX) ||
         File::Exists(save_path + GUEST_MARK_SUFFIX);
}

std::mutex s_restore_mutex;
// True while a room's end-of-session restore is armed but not yet run.
std::atomic<bool> s_restore_pending{false};
// Registered once, kept for the life of the process (same idiom as
// TeamInjector's purge hook).
Common::EventHook s_state_hook;

// Put the original import back for one port. Idempotent; a no-op when nothing
// is stashed. MUST NOT run while the emulated GBAs are alive -- the live mGBA
// core rewrites the socket save at teardown, so anything done earlier is
// simply overwritten. EndNetplaySession/HealLeftoverSession enforce that.
void RestoreNow(int device)
{
  const std::string save_path = SavePathForDevice(device);
  if (save_path.empty())
    return;
  const std::string orig = save_path + NETPLAY_ORIG_SUFFIX;
  if (!File::Exists(orig))
    return;

  if (device == 2 && InnerLifecyclePending(save_path))
  {
    // TeamInjector's cleanup has not finished for this socket (its own restore
    // failed and kept .hostteam for a retry, or its purge never ran). Restoring
    // the import now and deleting the stash would let that retry later copy the
    // disposable straight over the restored import -- data loss. Keep the stash;
    // the next session boundary's HealLeftoverSession finishes the inner
    // lifecycle first and then restores this.
    Log(fmt::format("sock={} restore=deferred-inner-pending", device));
    return;
  }

  if (File::CopyRegularFile(orig, save_path))
  {
    File::Delete(orig);
    Log(fmt::format("sock={} restored=import", device));
  }
  else
  {
    // Keep the stash so a later boundary can retry, rather than destroying the
    // only copy of the import's session-start state because one copy failed.
    Log(fmt::format("sock={} restore=FAILED", device));
  }
}

void RunPendingRestore()
{
  if (!s_restore_pending.exchange(false, std::memory_order_acq_rel))
    return;
  std::lock_guard lock(s_restore_mutex);
  RestoreNow(1);
  RestoreNow(2);
}

// Build the disposable image for one imported port from `source_bytes` (the
// imported save). Returns std::nullopt with *error on any failure; the FRLG
// case gets a hosting-specific message (Extract's own text explains submission,
// not hosting).
std::optional<EmeraldSave> BuildDisposable(int device, std::vector<u8> source_bytes,
                                           std::string* error)
{
  const auto fail = [error](std::string message) {
    if (error)
      *error = std::move(message);
    return std::nullopt;
  };

  std::string parse_error;
  const auto imported = EmeraldSave::Create(std::move(source_bytes), &parse_error);
  if (!imported)
    return fail(fmt::format("the imported save on GBA port {} is unreadable ({})", device + 1,
                            parse_error));

  // Mirror of the FRLG submission refusal, for the mirror-image reason: the
  // party-bundle extractor cannot read a FireRed/LeafGreen save (its party
  // lives at different section offsets), so no disposable can be built -- and
  // hosting without one would sync the complete FRLG save to the guest, the
  // exact leak this module exists to close. Refuse hosting loudly instead.
  if (EmeraldSave::DetectGame(*imported) == Gen3Game::FireRedLeafGreen)
  {
    return fail(fmt::format(
        "the imported save on GBA port {} is FireRed/LeafGreen. Hosting shares a rebuilt save "
        "carrying only your party and trainer identity, and that rebuild cannot read a "
        "FireRed/LeafGreen save -- so hosting would send your complete save file instead. "
        "Restore the default save for that port (or import an Emerald or Ruby/Sapphire save), "
        "then host.",
        device + 1));
  }

  std::string extract_error;
  const auto bundle = PartyBundle::Extract(*imported, &extract_error);
  if (!bundle)
  {
    return fail(fmt::format(
        "cannot build the netplay save for GBA port {}: {}. Hosting would otherwise send your "
        "complete save file, so it was refused.",
        device + 1, extract_error));
  }

  std::vector<u8> template_bytes;
  if (!ReadFileBytes(TemplatePath(device), &template_bytes))
    return fail("bundled template missing: " + TemplatePath(device));

  std::string build_error;
  auto disposable = PartyBundle::BuildSave(std::move(template_bytes), *bundle, &build_error);
  if (!disposable)
    return fail(fmt::format("could not build the netplay save for GBA port {} ({})", device + 1,
                            build_error));
  return disposable;
}

// Crash self-heal, part 1, is XDNetplay::HealLeftoverGuestState (TeamInjector):
// if the crashed session left the guest-team lifecycle unfinished (a guest's
// party may still be in a socket save, with .bak/.tmp/.guestteam beside it),
// its purge restores the pre-injection save -- for an imported port that is
// the session's disposable -- and scrubs the guest copies; part 2 (RestoreNow)
// then puts the import back over the result. It goes through RestoreHostTeam,
// which keeps TeamInjector's Core-state hook registered ahead of this
// module's, preserving the restore-after-purge callback order.
//
// MUST be called with s_restore_mutex NOT held: RestoreHostTeam's first call
// registers a Core-state callback (HookableEvent's own lock), and this
// module's callback takes s_restore_mutex under that same lock -- taking the
// two in the opposite order here could deadlock (same note as TeamInjector's
// out-of-lock registration).

// Crash self-heal, part 2, for one port. First-stash-wins: the leftover
// .netplayorig IS the import; it is restored, never re-stashed over (a
// re-stash would capture the disposable and lose the import for good -- the
// same rule .preimport follows). Inline-only: the callers guarantee emulation
// is down (all the session-boundary hooks run with no game booted; if one
// ever does not, the heal simply waits for the next boundary).
void HealDeviceNow(int device)
{
  const std::string save_path = SavePathForDevice(device);
  if (save_path.empty() || !File::Exists(save_path + NETPLAY_ORIG_SUFFIX))
    return;
  RestoreNow(device);
}
#endif  // HAS_LIBMGBA
}  // namespace

void HealLeftoverSession()
{
#ifdef HAS_LIBMGBA
  // While a room is open, a .netplayorig stash is not a leftover -- it is the
  // LIVE session's stash, and "healing" it would put the full import back into
  // the very save path the room is about to sync. Same guard for a running
  // game: the mGBA cores own the socket saves.
  // Room-scoped guard first: IsNetPlayRunning is GAME-scoped and cannot see a
  // lobby or a between-battles window, which is exactly when a live room's
  // .netplayorig stash and guest-team markers exist (see the TeamInjector
  // heal's guard note). BeginSession/EndSession bracket the room on both
  // platforms.
  if (XDNetplay::BattleCustomizer::IsNetplaySessionActive())
    return;
  if (NetPlay::IsNetPlayRunning() || !Core::IsUninitialized(Core::System::GetInstance()))
    return;
  // The guest-team lifecycle heals STANDALONE, not only under a .netplayorig:
  // a template-save host (no import, so no .netplayorig ever exists) whose
  // session was killed still has the opponent's party as the socket save, and
  // the old .netplayorig-gated finish walked right past it -- the next solo
  // boot then played the guest's team (the 1.4.2 field report). Same
  // out-of-lock ordering note as the part-1 comment above.
  XDNetplay::HealLeftoverGuestState();
  std::lock_guard lock(s_restore_mutex);
  HealDeviceNow(1);
  HealDeviceNow(2);
#endif
}

bool PrepareForHosting(std::string* error)
{
#ifndef HAS_LIBMGBA
  return true;  // no GBA support: nothing is synced, nothing to protect
#else
  if (!Core::IsUninitialized(Core::System::GetInstance()))
  {
    if (error)
    {
      *error = "A game is running — the emulated GBA owns its save file, so the netplay save "
               "swap cannot run. Stop emulation first.";
    }
    return false;
  }

  // A previous session's restore may still be ARMED but not yet run (the
  // Uninitialized notification races the UI thread by a hair). Run it now so
  // it can never fire after this session's swap and put the imports back into
  // the synced path mid-room.
  RunPendingRestore();

  // A crashed previous session next: restore before anything is rebuilt, so
  // the extraction below always reads the real import, never a stale
  // disposable -- and so a leftover GUEST team can never be what SyncSaveData
  // ships to this room's joiners (the standalone heal covers hosts with no
  // import, where no .netplayorig gates the finish). Runs before the lock
  // (see the part-1 deadlock note above).
  XDNetplay::HealLeftoverGuestState();

  std::lock_guard lock(s_restore_mutex);
  HealDeviceNow(1);
  HealDeviceNow(2);

  std::vector<int> swapped;
  const auto unwind = [&swapped] {
    for (const int device : swapped)
      RestoreNow(device);
  };

  for (const int device : {1, 2})
  {
    // The per-port import key is the "this socket holds a user import" signal:
    // set by ImportUserSave, cleared by RestoreDefaultSave. Bundled/team-editor
    // saves leave it empty and are deliberately untouched -- syncing those is
    // privacy-neutral, and zero behavior change is the requirement.
    if (Config::Get(ImportedSaveKey(device)).empty())
      continue;

    const std::string save_path = SavePathForDevice(device);
    if (save_path.empty() || !File::Exists(save_path))
      continue;  // nothing on disk to leak

    std::vector<u8> import_bytes;
    if (!ReadFileBytes(save_path, &import_bytes))
    {
      if (error)
        *error = "could not read " + save_path;
      unwind();
      return false;
    }

    auto disposable = BuildDisposable(device, std::move(import_bytes), error);
    if (!disposable)
    {
      unwind();
      return false;
    }

    // Stash the import ONCE (first-stash-wins; after the heal above a stash
    // should never pre-exist, but never overwrite one regardless).
    const std::string orig = save_path + NETPLAY_ORIG_SUFFIX;
    if (!File::Exists(orig) && !File::CopyRegularFile(save_path, orig))
    {
      // Remove whatever the failed copy left: a truncated stash is worse than
      // none, because every later boundary's heal would blindly copy it back
      // over the intact import. No stash means the heal has nothing to do.
      File::Delete(orig);
      if (error)
        *error = "could not back up the imported save to " + orig + " — hosting refused";
      unwind();
      return false;
    }

    // The same verified-write path as every injection: re-parse, checksum
    // check, tmp write, readback compare, .bak, rename.
    std::string write_error;
    if (!VerifiedWriteSaveFile(save_path, *disposable, &write_error))
    {
      if (error)
        *error = fmt::format("netplay save NOT written ({})", write_error);
      // The stash for THIS port already exists (same bytes as the untouched
      // save); include it in the unwind so no .netplayorig lingers after a
      // refusal.
      swapped.push_back(device);
      unwind();
      return false;
    }

    swapped.push_back(device);
    Log(fmt::format("sock={} swapped=disposable", device));
  }
  return true;
#endif
}

void EndNetplaySession()
{
#ifdef HAS_LIBMGBA
  // Deferred exactly like TeamInjector's purge, and for the same reason: when
  // a room closes mid-battle, Core::Stop is asynchronous and the mGBA cores
  // rewrite the socket saves from memory at teardown -- a restore done now
  // would be overwritten moments later. Arm the restore and run it once
  // emulation reaches Uninitialized (inline immediately in the common
  // stopped-first case).
  //
  // ORDERING vs the inner .hostteam restore: the OnRoomClosed hooks call
  // RestoreHostTeam(2) before this function, so TeamInjector's Uninitialized
  // callback is registered before this one and HookableEvent triggers them in
  // registration order -- the inner purge puts the disposable back and scrubs
  // the guest copies, THEN this restore replaces the disposable with the
  // original import. RestoreNow additionally refuses to run over a pending
  // inner lifecycle, so even a failed inner restore cannot be clobbered.
  static std::once_flag hook_once;
  std::call_once(hook_once, [] {
    // Registered outside s_restore_mutex: Register() takes the event's own
    // lock and the callback takes s_restore_mutex under it, so taking them in
    // the other order here could deadlock (same note as TeamInjector).
    s_state_hook = Core::AddOnStateChangedCallback([](Core::State state) {
      if (state == Core::State::Uninitialized)
        RunPendingRestore();
    });
  });

  s_restore_pending.store(true, std::memory_order_release);

  if (Core::IsUninitialized(Core::System::GetInstance()))
    RunPendingRestore();
#endif
}
}  // namespace XDNetplay::DisposableSave
