// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/TeamInjector.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GBADetectLog.h"
#include "Core/System.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif

#include "UICommon/XDNetplay/Gen3Data.h"
#include "UICommon/XDNetplay/Gen3Mon.h"
#include "UICommon/XDNetplay/Gen3Save.h"
#include "UICommon/XDNetplay/MonFactory.h"
#include "UICommon/XDNetplay/ShowdownParser.h"

namespace XDNetplay
{
namespace
{
constexpr const char* HOST_STASH_SUFFIX = ".hostteam";
// Written next to the save on every successful injection, removed by the
// end-of-session purge. Its presence means "this socket's save -- and the
// throwaway files VerifiedWriteSaveFile leaves beside it -- may hold a guest's
// party". It survives a crash, which is what lets the next room close finish a
// cleanup that a killed process never got to run.
constexpr const char* GUEST_MARK_SUFFIX = ".guestteam";
// One level of undo, created by VerifiedWriteSaveFile (so: by both the team
// editors and by every injection).
constexpr const char* BACKUP_SUFFIX = ".bak";
constexpr const char* TMP_SUFFIX = ".tmp";

// See TeamInjector.h for the payload grammar.
constexpr std::string_view NAME_HEADER_KEY = "Name";
constexpr std::string_view MODEL_HEADER_KEY = "Model";
// Headers are framing, not a payload channel: both value lines stay bounded.
constexpr size_t MAX_NAME_LINE = 64;
constexpr size_t MAX_MODEL_VALUE = 32;

// A line containing only spaces/tabs/CR (or nothing) terminates the header
// block. Same blank test the original single-header grammar used.
bool IsBlankLine(std::string_view line)
{
  return line.find_first_not_of(" \t\r") == std::string_view::npos;
}

// Split "Key: value" where the key has the exact shape
// [A-Za-z][A-Za-z0-9_-]* immediately followed by ':'. Returns false when the
// line does not fit -- which, per the grammar, aborts the whole header block.
bool SplitHeaderLine(std::string_view line, std::string_view* key, std::string_view* value)
{
  if (line.empty() || !Common::IsAlpha(line[0]))
    return false;
  size_t i = 1;
  while (i < line.size() && (Common::IsAlnum(line[i]) || line[i] == '_' || line[i] == '-'))
    ++i;
  if (i >= line.size() || line[i] != ':')
    return false;
  *key = line.substr(0, i);
  *value = line.substr(i + 1);
  return true;
}

// "Model:" value: an integer, decimal or 0xHH hex. Untrusted remote text --
// anything that does not parse cleanly is "no preference", never an error.
std::optional<int> ParseModelValue(std::string_view value)
{
  value = StripWhitespace(value);
  if (value.empty() || value.size() > MAX_MODEL_VALUE)
    return std::nullopt;

  int base = 10;
  if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
  {
    base = 16;
    value.remove_prefix(2);
  }

  int result = 0;
  const char* const first = value.data();
  const char* const last = first + value.size();
  const auto [ptr, ec] = std::from_chars(first, last, result, base);
  if (ec != std::errc{} || ptr != last)
    return std::nullopt;
  return result;
}

// The save path netplay will read for this socket, or empty when the setup
// isn't complete enough to have one.
std::string SavePathForDevice(int device)
{
#ifdef HAS_LIBMGBA
  std::string rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device]);
  if (rom.empty() || !File::Exists(rom))
    rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device == 1 ? 2 : 1]);
  if (rom.empty() || !File::Exists(rom))
    return {};
  return HW::GBA::Core::GetSavePath(rom, device);
#else
  return {};
#endif
}

// Zero a file's bytes before unlinking it, so an opponent's spread is not left
// sitting in freed disk blocks. Best effort: a failed scrub never stops the
// delete, and the return value reports only whether a file was there to remove.
bool ScrubAndDelete(const std::string& path)
{
  if (!File::Exists(path))
    return false;

  {
    File::IOFile file(path, "r+b");
    if (file)
    {
      const u64 size = file.GetSize();
      const std::vector<u8> zeros(static_cast<size_t>(std::min<u64>(size, 64 * 1024)), 0);
      u64 done = 0;
      while (!zeros.empty() && done < size)
      {
        const size_t chunk = static_cast<size_t>(std::min<u64>(zeros.size(), size - done));
        if (!file.WriteBytes(zeros.data(), chunk))
          break;
        done += chunk;
      }
      file.Flush();
    }
  }

  return File::Delete(path);
}

// Netplay's per-session staging copies of the HOST's GBA saves. On a joiner
// these are the files the emulated GBAs actually run from, so NetPlayTemp2.sav
// is a byte-for-byte copy of the HOST's party -- the opponent's team, sitting
// in the same GBA folder users are told to fetch detect logs from. Netplay only
// ever deletes them on the way IN to the next session, so without this they
// outlive the room by however long the user goes without playing again.
//
// Format must stay in step with NetPlayClient.cpp (GetGBASavePath and
// OnSyncSaveDataGBA both build this name).
std::string NetPlayTempSavePath(int slot)
{
  return fmt::format("{}{}{}.sav", File::GetUserPath(D_GBAUSER_IDX), GBA_SAVE_NETPLAY, slot + 1);
}

std::mutex s_purge_mutex;
// Device awaiting a purge once emulation is fully down; -1 when none is armed.
std::atomic<int> s_pending_device{-1};
// Registered once, kept for the life of the process. Destroying it from inside
// its own callback would be legal (HookableEvent defers removal), but there is
// nothing to gain from unregistering and re-registering per room.
Common::EventHook s_state_hook;

// Every place an opponent's party can live on this machine, cleared in one
// pass. Idempotent, and a no-op when nothing was ever injected.
//
// MUST NOT run while the emulated GBAs are alive: the live mGBA core owns the
// save file and rewrites it from its in-memory copy when it is torn down, so
// anything done earlier is simply overwritten. RestoreHostTeam is what enforces
// that ordering.
void PurgeNow(int device)
{
  std::vector<std::string> notes;

  const std::string save_path = SavePathForDevice(device);
  if (!save_path.empty())
  {
    const std::string stash = save_path + HOST_STASH_SUFFIX;
    const std::string mark = save_path + GUEST_MARK_SUFFIX;
    const bool had_guest = File::Exists(mark);

    if (File::Exists(stash))
    {
      // The host had a save of their own before the first injection: put it
      // back. Both images are the same size, so nothing of the guest's is left
      // past the end of the copy.
      if (File::CopyRegularFile(stash, save_path))
      {
        File::Delete(stash);
        notes.emplace_back("restored=host");
      }
      else
      {
        // Keep the stash so a later close can retry, rather than destroying the
        // host's own team because one copy failed.
        notes.emplace_back("restore=FAILED");
      }
    }
    else if (had_guest)
    {
      // A guest team was injected and there is no stash, which means this
      // socket had no save at all beforehand: the file was seeded from the
      // bundled template purely to carry the guest's party. There is no host
      // team to put back -- the file IS the opponent's data. (The old
      // RestoreHostTeam returned early in exactly this case, leaving a
      // stranger's spread as the host's permanent socket save.)
      //
      // Put the bundled template back rather than just deleting, so the
      // launcher's "Team saves installed" check stays satisfied and the user is
      // left where they started instead of one step behind it.
      if (ScrubAndDelete(save_path))
      {
        const std::string template_path =
            File::GetSysDirectory() +
            (device == 1 ? "XDNetplay/EMERALD-2.sav" : "XDNetplay/EMERALD-3.sav");
        if (File::Exists(template_path) && File::CopyRegularFile(template_path, save_path))
          notes.emplace_back("reset=template");
        else
          notes.emplace_back("removed=seeded-save");
      }
    }

    if (had_guest)
    {
      // .bak is whatever the save held BEFORE the most recent injection. On the
      // first injection of a room that is the host's own team (harmless; the
      // stash holds the same bytes). On the second and later injections -- a
      // guest resubmitting, or a second guest in the same room -- it is a
      // previous guest's complete party, and nothing ever deleted it.
      if (ScrubAndDelete(save_path + BACKUP_SUFFIX))
        notes.emplace_back("removed=bak");
      // Only survives a failed rename, but when it does it is a full copy of
      // the injected save.
      if (ScrubAndDelete(save_path + TMP_SUFFIX))
        notes.emplace_back("removed=tmp");
      File::Delete(mark);
    }
  }

  // Runs on hosts too: a player who joined someone's room earlier still has
  // that host's party in NetPlayTemp2.sav.
  for (int slot = 0; slot < 4; ++slot)
  {
    if (ScrubAndDelete(NetPlayTempSavePath(slot)))
      notes.emplace_back(fmt::format("removed=netplaytemp{}", slot + 1));
  }

#ifdef HAS_LIBMGBA
  // GBADetectLog is compiled only when USE_MGBA is on; the purge itself is not,
  // because the files it removes can outlive an mGBA-less rebuild.
  GBADetectLog::LogPostSession(fmt::format(
      "sock={} teamcleanup {}", device,
      notes.empty() ? std::string{"nothing-to-clean"} : fmt::format("{}", fmt::join(notes, " "))));
#endif
}

void RunPendingPurge()
{
  const int device = s_pending_device.exchange(-1, std::memory_order_acq_rel);
  if (device < 0)
    return;
  std::lock_guard lock(s_purge_mutex);
  PurgeNow(device);
}
}  // namespace

#ifdef HAS_LIBMGBA
namespace
{
bool ReadFileBytes(const std::string& path, std::vector<u8>* out)
{
  File::IOFile file(path, "rb");
  if (!file)
    return false;
  out->resize(file.GetSize());
  return out->empty() || file.ReadBytes(out->data(), out->size());
}
}  // namespace
#endif

std::string BuildTeamSubmissionPayload(const std::string& showdown_text,
                                       const std::string& trainer_name, std::optional<int> model)
{
  // A newline in the name would forge extra header/blank lines, so flatten
  // any line break to a space before it goes on the wire. The 7-character Gen 3
  // clamp is deliberately NOT applied here: the host sanitizes what it receives
  // and is the only side whose opinion counts.
  std::string one_line;
  one_line.reserve(trainer_name.size());
  for (const char c : trainer_name)
    one_line.push_back(c == '\n' || c == '\r' ? ' ' : c);

  if (one_line.size() > MAX_NAME_LINE)
    one_line.resize(MAX_NAME_LINE);

  std::string headers;
  if (one_line.find_first_not_of(" \t") != std::string::npos)
    headers += std::string(NAME_HEADER_KEY) + ": " + one_line + "\n";
  // "Game default" (or anything nonsensical) means no Model line at all; the
  // host's own fallback dropdown then decides.
  if (model && *model > 0)
    headers += std::string(MODEL_HEADER_KEY) + ": " + std::to_string(*model) + "\n";

  if (headers.empty())
    return showdown_text;  // nothing to say -- emit the old bare format

  return headers + "\n" + showdown_text;
}

TeamSubmission ParseTeamSubmissionPayload(const std::string& payload)
{
  TeamSubmission out;
  // Until a complete, blank-line-terminated header block proves otherwise,
  // the whole payload is team text (the pre-header behavior).
  out.showdown_text = payload;

  std::string name;
  std::optional<int> model;
  bool any_header = false;
  size_t pos = 0;
  while (pos < payload.size())
  {
    const size_t eol = payload.find('\n', pos);
    if (eol == std::string::npos)
      return out;  // payload ended before the blank line: no header block
    const std::string_view line(payload.data() + pos, eol - pos);

    if (IsBlankLine(line))
    {
      if (!any_header)
        return out;  // a blank FIRST line is not a header block
      out.trainer_name = std::move(name);  // trimmed/sanitized by the caller
      out.model = model;
      out.showdown_text = payload.substr(eol + 1);
      return out;
    }

    std::string_view key;
    std::string_view value;
    if (!SplitHeaderLine(line, &key, &value))
      return out;  // the run broke before a blank line: team text after all

    // Recognized keys; unknown ones are skipped harmlessly (TeamInjector.h).
    if (key == NAME_HEADER_KEY)
      name = std::string(value);
    else if (key == MODEL_HEADER_KEY)
      model = ParseModelValue(value);

    any_header = true;
    pos = eol + 1;
  }
  return out;  // ran off the end without a blank line
}

bool InjectGuestTeam(const std::string& showdown_text, const std::string& trainer_name, int device,
                     std::string* status)
{
  const auto fail = [status](std::string message) {
    if (status)
      *status = std::move(message);
    return false;
  };

#ifndef HAS_LIBMGBA
  return fail("this build has no GBA support");
#else
  std::string error;
  const auto data = Gen3Data::LoadBundled(&error);
  if (!data)
    return fail(fmt::format("game data unavailable ({})", error));

  // Same ROM resolution the team editor uses: the socket's own ROM, falling
  // back to the other socket's (a launcher-made setup points both at one dump).
  std::string rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device]);
  if (rom.empty() || !File::Exists(rom))
    rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device == 1 ? 2 : 1]);
  if (rom.empty() || !File::Exists(rom))
    return fail("no Emerald ROM configured on the host");

  // GetSavePath is exactly what SyncSaveData reads at start, so writing here
  // is guaranteed to be the file the guest receives.
  const std::string save_path = HW::GBA::Core::GetSavePath(rom, device);

  // Self-heal: if a previous session was killed before its cleanup ran, the
  // save still holds THAT guest's party and .bak/.guestteam are still lying
  // around. Finish that cleanup now, before this injection stashes anything --
  // otherwise the stale party would be preserved as if it were the host's own.
  // Only safe with emulation fully down, which is the case here: the server
  // refuses team submissions while a battle is running.
  if (File::Exists(save_path + GUEST_MARK_SUFFIX) &&
      Core::IsUninitialized(Core::System::GetInstance()))
  {
    std::lock_guard lock(s_purge_mutex);
    PurgeNow(device);
  }

  std::vector<u8> bytes;
  if (!(File::Exists(save_path) && ReadFileBytes(save_path, &bytes)))
  {
    const std::string template_path =
        File::GetSysDirectory() +
        (device == 1 ? "XDNetplay/EMERALD-2.sav" : "XDNetplay/EMERALD-3.sav");
    if (!ReadFileBytes(template_path, &bytes))
      return fail("host has no save for that socket and no bundled template");
  }

  // Stash the host's own team once per session, so it can be put back when the
  // room closes. Only the FIRST injection stashes: a second submission must not
  // overwrite the stash with the first guest's team.
  if (File::Exists(save_path) && !File::Exists(save_path + HOST_STASH_SUFFIX))
    File::CopyRegularFile(save_path, save_path + HOST_STASH_SUFFIX);

  auto save = EmeraldSave::Create(std::move(bytes), &error);
  if (!save)
    return fail(fmt::format("host save unreadable ({})", error));

  const std::vector<ShowdownSet> sets = ShowdownParser::ParseTeam(showdown_text);
  if (sets.empty())
    return fail("nothing recognizable in that paste");

  // ORDER IS LOAD-BEARING: the trainer name goes in BEFORE the party is built.
  // MonFactory::Build stamps each Pokemon's OT name from the DESTINATION save
  // (save->GetTrainerName() below), so building first would give every mon the
  // host's old OT while the trainer block said something else -- in-game they
  // would read as traded outsiders (disobedience above the badge cap, "it does
  // not seem to want to obey"), which is exactly the bug this ordering avoids.
  //
  // trainer_name is untrusted remote text, so it is sanitized, never rejected:
  // a name that survives sanitizing is written, one that does not leaves the
  // save's existing name in place. Either way the outcome is reported below.
  std::string applied_name;
  std::string dropped_name_note;
  // A name of only spaces (or nothing at all) means "no name was asked for" and
  // is not worth a line in chat; a name that HAD content but sanitized away to
  // nothing is, because the guest will otherwise wonder why it did not take.
  if (trainer_name.find_first_not_of(" \t\r\n") != std::string::npos)
  {
    const std::string sanitized = EmeraldSave::SanitizeTrainerName(trainer_name);
    if (sanitized.empty())
    {
      dropped_name_note = "name unusable in-game, kept " + save->GetTrainerName();
    }
    else if (!save->SetTrainerName(sanitized, &error))
    {
      // Unreachable in practice -- SanitizeTrainerName only emits names
      // SetTrainerName accepts -- but never silently pretend it worked.
      dropped_name_note = fmt::format("name not set ({})", error);
    }
    else
    {
      applied_name = sanitized;
    }
  }

  // Any party size is accepted: players run whatever format they like, and the
  // game itself enforces its own rules at battle setup.
  std::vector<Gen3Mon> built;
  size_t skipped = 0;
  for (const ShowdownSet& set : sets)
  {
    if (built.size() == EmeraldSave::PARTY_MAX)
    {
      ++skipped;
      continue;
    }
    std::string mon_error;
    // GetTrainerName() here is the name set just above, which is the point.
    auto mon = MonFactory::Build(set, *data, save->GetTrainerName(), save->GetTrainerId(),
                                 &mon_error);
    if (mon)
      built.push_back(std::move(*mon));
    else
      ++skipped;
  }
  if (built.empty())
    return fail("no usable Pokemon in that team");

  const size_t count = built.size();
  if (!save->WriteParty(built))
    return fail("party too large");
  if (!VerifiedWriteSaveFile(save_path, *save, &error))
    return fail(fmt::format("save NOT written ({})", error));

  // From here on this save, its fresh .bak (the pre-injection image) and any
  // stray .tmp are opponent data. Mark them so the end-of-session purge knows
  // to take them, even if this process never gets to run it.
  if (!File::Exists(save_path + GUEST_MARK_SUFFIX))
  {
    File::IOFile mark(save_path + GUEST_MARK_SUFFIX, "wb");
    if (mark)
      mark.WriteString("A guest's team is in this socket's save. Removed when the room closes.\n");
  }

  if (status)
  {
    *status = skipped == 0 ?
                  fmt::format("{} Pokemon written to the guest save", count) :
                  fmt::format("{} Pokemon written to the guest save ({} entries skipped)", count,
                              skipped);
    // Always show the name that actually landed -- it is what the opponent
    // will see across the link, and it may not be what was asked for.
    if (!applied_name.empty())
      *status += fmt::format(", playing as {}", applied_name);
    else if (!dropped_name_note.empty())
      *status += fmt::format(" ({})", dropped_name_note);
  }
  return true;
#endif
}

void RestoreHostTeam(int device)
{
  // THE ORDERING THIS EXISTS TO FIX
  //
  // ~NetPlayClient() calls StopGame() when a battle is live, which reaches
  // Core::Stop() -- and Core::Stop() is ASYNCHRONOUS: it flips the state to
  // Stopping and stops the CPU, then returns. The emulation thread tears the
  // hardware down afterwards, and only then does ~CSIDevice_GBAEmu run
  // Core::Stop() on the mGBA core, whose deinit rewrites the .sav from its
  // in-memory savedata.
  //
  // MainWindow::NetPlayQuit() (and the Android equivalent) then does
  // ResetNetPlayClient() immediately followed by ResetNetPlayServer(), so
  // ~NetPlayServer -> OnRoomClosed() -> here lands on the UI thread while that
  // teardown is still in flight. Doing the restore right now would copy the
  // host's team back and delete the stash, and moments later the mGBA core
  // would write the guest's party straight over it -- losing the host's team
  // AND keeping the opponent's, the exact opposite of the intent.
  //
  // So: arm the purge, and run it only once emulation is Uninitialized. That
  // state is notified from a scope guard at the very end of EmuThread, i.e.
  // after HW::Shutdown() has destroyed the SI devices and the GBA cores have
  // flushed. If emulation is already down (the common case -- stop the game,
  // then close the room) it runs inline and the hook has nothing left to do.
  static std::once_flag hook_once;
  std::call_once(hook_once, [] {
    // Registered outside s_purge_mutex on purpose: Register() takes the event's
    // own lock, and the callback takes s_purge_mutex while that lock is held,
    // so acquiring them in the other order here could deadlock.
    s_state_hook = Core::AddOnStateChangedCallback([](Core::State state) {
      if (state == Core::State::Uninitialized)
        RunPendingPurge();
    });
  });

  s_pending_device.store(device, std::memory_order_release);

  if (Core::IsUninitialized(Core::System::GetInstance()))
    RunPendingPurge();
}
}  // namespace XDNetplay
