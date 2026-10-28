// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GBADetectLog.h"
#include "Core/HW/GBAMoveNames.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <atomic>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/Config/MainSettings.h"

namespace GBADetectLog
{
namespace
{
// Hard ceiling on one session's file, so a wedged session that emits summaries
// for hours can never fill the disk. 32 MiB: the first instrumented netplay
// battle hit the old 4 MiB cap at 6.8 minutes and went dark 9 minutes before
// the interesting failure; measured burn was ~10 KB/s, 90% of it byte-identical
// repeated joy lines (now suppressed post-lock, cutting battle-phase burn to
// ~1 KB/s), so this budget holds hours of real play.
constexpr u64 MAX_LOG_BYTES = 32u << 20;

std::mutex s_mutex;
std::ofstream s_file;
// Tester switch (XDNetplay/RecordLinkTraffic): every GC<->GBA data command and reply of the
// session, unfiltered, to linktrace_<same stamp>.txt. Own cap; under s_mutex like s_file.
std::ofstream s_trace;
u64 s_trace_bytes = 0;
constexpr u64 MAX_TRACE_BYTES = 96u << 20;

// Battle transcript (always on for a GBA session): XD's per-decision "turn blocks" to each GBA and
// each GBA's choice reply, collected per socket and turned into battle_<stamp>.txt when the
// session closes. Under s_mutex. Block layout (words of 4 bytes as sent, values little-endian):
// a 0x66 word, then 373 words: [0] party order + request (byte 3: slot 1/2, 0x80 = replace a
// fainted Pokemon), [4] the deciding Pokemon's species, [5..6] its four move ids, then four
// party records of 85 words from [9] (HP, item, two move words, PP, four 80-byte move texts),
// then from [349] the four battle positions (0-1 GBA 1's side, 2-3 GBA 2's), 6 words each: name
// (10 bytes, Gen 3 text), side and level, an id, species and current HP, max HP. The decider's
// own position is left empty in its copy. A reply is [move slot or party index, target position
// or 0xF7/0xFF, 1 = fight / 2 = switch, 0].
struct BattleEvent
{
  u64 tick = 0;
  int kind = 0;  // 0 = turn block, 1 = reply
  int sock = 0;
  std::vector<std::array<u8, 4>> words;
  std::array<u8, 4> reply{};
};
constexpr size_t BATTLE_BLOCK_WORDS = 373;
std::vector<BattleEvent> s_battle;
std::array<std::optional<std::vector<std::array<u8, 4>>>, 4> s_battle_building;
std::string s_session_stamp;
std::string s_path;
u64 s_bytes = 0;         // running size, enforces the cap
int s_live_devices = 0;  // ref-count -> session begin/end
// Pre-boot context lines (NoteBoot) waiting for the next session's file.
// Bounded: a user flipping dropdowns for an hour without booting must not
// grow this without limit -- older notes fall off, the freshest state wins.
std::vector<std::string> s_boot_notes;
constexpr size_t MAX_BOOT_NOTES = 160;  // raised from 32: one Battle Style
// regenerate emits ~60 lines (guard + models + bust/panel hides + menu pins +
// 36 ruleset words) and the ring was evicting the block's HEAD, which is
// exactly the part field debugging needs (the 1.5.2 preview-flash session
// could not show whether the hide lines shipped).
// Atomic so every Log* can early-out lock-free and stop FORMATTING once capped
// (Review #1 I5: the byte cap must bound CPU/lock cost, not just disk).
std::atomic<bool> s_capped{false};

// Repeat-suppression cache for joy lines, one slot per (SI channel,
// direction). Guarded by s_mutex like everything else in this file.
struct JoyRepeatSlot
{
  u8 bytes[5]{};
  int len = -1;
  u8 aux = 0;  // sent-flag for '>', jsa for '<'
  u32 repeats = 0;
};
JoyRepeatSlot s_joy_repeat[4][2];

std::string Timestamp(const char* format)
{
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buf[32] = {};
  std::strftime(buf, sizeof(buf), format, &tm);
  return buf;
}

// Keep the newest previous session logs, at most KEEP_PREVIOUS of them and at
// most KEEP_BYTES in total (the timestamped names sort lexicographically =
// chronologically), and drop the un-timestamped legacy file from older builds.
// Called before each session's file is created. A failed connection is usually
// reported a few rooms later, so a short history loses the one log that matters.
constexpr size_t KEEP_PREVIOUS = 20;
constexpr u64 KEEP_BYTES = 200ull * 1024 * 1024;

void PruneOldLogs(const std::string& dir)
{
  File::Delete(dir + "gba_detect.log");  // legacy single-file name
  std::vector<std::pair<std::string, u64>> old_logs;
  const File::FSTEntry tree = File::ScanDirectoryTree(dir, false);
  for (const File::FSTEntry& child : tree.children)
  {
    const std::string& name = child.virtualName;
    if (!child.isDirectory && name.rfind("gba_detect_", 0) == 0 &&
        name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0)
    {
      old_logs.emplace_back(name, child.size);
    }
  }
  std::sort(old_logs.begin(), old_logs.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  u64 kept_bytes = 0;
  for (size_t i = 0; i < old_logs.size(); ++i)
  {
    kept_bytes += old_logs[i].second;
    if (i >= KEEP_PREVIOUS || kept_bytes > KEEP_BYTES)
      File::Delete(dir + old_logs[i].first);
  }
}

// Must hold s_mutex. Appends one line (adds '\n'), enforces the byte cap, never
// throws. `flush` forces the stream to disk for rare, important lines.
void WriteLine(const std::string& line, bool flush)
{
  if (!s_file.is_open())
    return;
  if (s_bytes >= MAX_LOG_BYTES)
  {
    if (!s_capped.load(std::memory_order_relaxed))
    {
      s_file << "-- capped at " << MAX_LOG_BYTES << " bytes; further lines dropped --\n";
      s_file.flush();
      s_capped.store(true, std::memory_order_relaxed);
    }
    return;
  }
  s_file << line << '\n';
  s_bytes += line.size() + 1;
  if (flush)
    s_file.flush();
}

using BattleWord = std::array<u8, 4>;

u16 Le16(const BattleWord& w, int offset)
{
  return static_cast<u16>(w[offset] | (w[offset + 1] << 8));
}

// Gen 3 text to ASCII; an all-caps name (the games' default) reads as "Metagross".
std::string Gen3Name(const u8* bytes, size_t len)
{
  std::string out;
  for (size_t i = 0; i < len; ++i)
  {
    const u8 b = bytes[i];
    if (b == 0xFF)
      break;
    if (b >= 0xBB && b <= 0xD4)
      out.push_back(static_cast<char>('A' + b - 0xBB));
    else if (b >= 0xD5 && b <= 0xEE)
      out.push_back(static_cast<char>('a' + b - 0xD5));
    else if (b >= 0xA1 && b <= 0xAA)
      out.push_back(static_cast<char>('0' + b - 0xA1));
    else if (b == 0x00)
      out.push_back(' ');
    else if (b == 0xAD)
      out.push_back('.');
    else if (b == 0xAE)
      out.push_back('-');
    else
      out.push_back('?');
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  if (!out.empty() && std::ranges::none_of(out, [](char c) { return c >= 'a' && c <= 'z'; }))
  {
    for (size_t i = 1; i < out.size(); ++i)
    {
      if (out[i] >= 'A' && out[i] <= 'Z' && out[i - 1] != ' ' && out[i - 1] != '-')
        out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
  }
  return out;
}

std::string MoveName(u16 id)
{
  return id < std::size(GEN3_MOVE_NAMES) && id != 0 ? std::string(GEN3_MOVE_NAMES[id]) :
                                                       fmt::format("move {}", id);
}

// The transcript of one session's events (see BattleEvent), or "" when there is no battle.
std::string BuildBattleTranscript(std::vector<BattleEvent> events)
{
  std::ranges::stable_sort(events, [](const BattleEvent& a, const BattleEvent& b) {
    return a.tick != b.tick ? a.tick < b.tick : a.kind < b.kind;
  });
  using Key = std::pair<int, std::string>;  // side 0/1, name
  struct Turn
  {
    std::vector<std::string> acts;
    std::map<Key, std::pair<int, int>> hp;  // first value seen this turn: current, max
    std::vector<Key> order;
  };
  struct Pending
  {
    int me;
    bool replace;
    std::array<u16, 4> moves;
    std::array<u8, 8> moves_key;
    BattleWord pp;
  };
  // A chosen move, confirmed by its PP in that GBA's next block after the turn: a Pokemon that
  // fainted, flinched or slept before moving spends none.
  struct Unconfirmed
  {
    int side;
    size_t turn;
    size_t act;
    std::array<u8, 8> moves_key;
    int slot;
    u8 pp;
  };
  struct Replacement
  {
    int pos;
    size_t turn;
    size_t act;
  };
  std::array<std::string, 4> names;
  std::array<u16, 4> species{};
  std::map<Key, std::pair<int, int>> hp;
  std::array<std::map<std::array<u8, 8>, std::string>, 2> record_names;
  std::vector<Turn> turns;
  std::array<std::array<bool, 3>, 3> seen{};
  std::map<int, Pending> pending;
  std::vector<Replacement> replacements;
  std::vector<Unconfirmed> unconfirmed;

  const auto set_hp = [&](const Key& key, std::pair<int, int> value, bool normal) {
    if (!hp.contains(key) && normal && turns.size() >= 2)
    {
      // First time on the field: it came in fresh during the previous turn.
      Turn& previous = turns[turns.size() - 2];
      if (previous.hp.emplace(key, std::pair{value.second, value.second}).second)
        previous.order.push_back(key);
    }
    hp[key] = value;
    if (normal && !turns.empty() && turns.back().hp.emplace(key, value).second)
      turns.back().order.push_back(key);
  };

  for (const BattleEvent& event : events)
  {
    const int s = event.sock;
    if (s != 1 && s != 2)
      continue;
    const int side = s - 1;
    if (event.kind == 0)
    {
      const auto& w = event.words;
      const u8 req = w[0][3];
      const int slot = req & 0x7F;
      const bool replace = (req & 0x80) != 0;
      if (slot < 1 || slot > 2)
        continue;
      const int me = side * 2 + slot - 1;
      if (!replace && (seen[s][slot] || turns.empty()))
      {
        turns.emplace_back();
        seen = {};
      }
      if (!replace)
        seen[s][slot] = true;
      for (int p = 0; p < 4; ++p)
      {
        const BattleWord* e = &w[349 + 6 * p];
        if (e[0][0] != 0xFF)
        {
          std::array<u8, 10> raw{};
          std::copy(e[0].begin(), e[0].end(), raw.begin());
          std::copy(e[1].begin(), e[1].end(), raw.begin() + 4);
          raw[8] = e[2][0];
          raw[9] = e[2][1];
          const std::string name = Gen3Name(raw.data(), raw.size());
          if (names[p] != name)
          {
            std::erase_if(replacements, [&](const Replacement& r) {
              if (r.pos != p)
                return false;
              turns[r.turn].acts[r.act] = fmt::format("GBA {} sends in {}", p / 2 + 1, name);
              return true;
            });
          }
          names[p] = name;
          species[p] = Le16(e[4], 0);
          set_hp({p / 2, name}, {Le16(e[4], 2), Le16(e[5], 0)}, !replace);
        }
        else if (p != me && !names[p].empty())
        {
          // An empty position that held a Pokemon: it fainted.
          const Key key{p / 2, names[p]};
          if (const auto it = hp.find(key); it != hp.end())
            set_hp(key, {0, it->second.second}, false);
          names[p].clear();
        }
      }
      std::array<u8, 8> moves_key{};
      std::copy(w[5].begin(), w[5].end(), moves_key.begin());
      std::copy(w[6].begin(), w[6].end(), moves_key.begin() + 4);
      // Moves chosen in earlier turns. Not at a replacement: XD sends a fainted Pokemon's
      // replacement in mid-turn, before the rest have moved.
      std::erase_if(unconfirmed, [&](const Unconfirmed& u) {
        if (u.side != side || replace || u.turn + 1 >= turns.size())
          return false;
        for (int i = 0; i < 4; ++i)
        {
          std::array<u8, 8> record_moves{};
          std::copy(w[11 + 85 * i].begin(), w[11 + 85 * i].end(), record_moves.begin());
          std::copy(w[12 + 85 * i].begin(), w[12 + 85 * i].end(), record_moves.begin() + 4);
          if (record_moves != u.moves_key)
            continue;
          if (w[13 + 85 * i][u.slot] == u.pp)
            turns[u.turn].acts[u.act] += " (did not move)";
          return true;
        }
        return false;
      });
      if (!replace && species[me] == Le16(w[4], 0) && !names[me].empty())
        record_names[side][moves_key] = names[me];
      for (int i = 0; i < 4; ++i)
      {
        std::array<u8, 8> record_moves{};
        std::copy(w[11 + 85 * i].begin(), w[11 + 85 * i].end(), record_moves.begin());
        std::copy(w[12 + 85 * i].begin(), w[12 + 85 * i].end(), record_moves.begin() + 4);
        const auto it = record_names[side].find(record_moves);
        if (it == record_names[side].end())
          continue;
        const Key key{side, it->second};
        if (const auto known = hp.find(key); known != hp.end())
          set_hp(key, {Le16(w[9 + 85 * i], 0), known->second.second}, !replace);
      }
      pending[s] = {me,      replace, {Le16(w[5], 0), Le16(w[5], 2), Le16(w[6], 0), Le16(w[6], 2)},
                    moves_key, w[7]};
    }
    else
    {
      const auto it = pending.find(s);
      if (it == pending.end() || turns.empty())
        continue;
      const Pending decision = it->second;
      pending.erase(it);
      const auto& r = event.reply;
      std::vector<std::string>& acts = turns.back().acts;
      if (r[2] == 2)
      {
        acts.push_back(fmt::format("GBA {} sends in a replacement", s));
        replacements.push_back({decision.me, turns.size() - 1, acts.size() - 1});
        continue;
      }
      const std::string move = r[0] < 4 ? MoveName(decision.moves[r[0]]) : std::string("?");
      const std::string actor = names[decision.me].empty() ? "?" : names[decision.me];
      std::string target;
      if (r[1] <= 3)
      {
        target = fmt::format(" on GBA {} {}", r[1] / 2 + 1,
                             names[r[1]].empty() ? std::string("?") : names[r[1]]);
      }
      acts.push_back(fmt::format("GBA {} {}: {}{}", s, actor, move, target));
      if (r[0] < 4)
      {
        unconfirmed.push_back(
            {side, turns.size() - 1, acts.size() - 1, decision.moves_key, r[0], decision.pp[r[0]]});
      }
    }
  }
  if (turns.empty())
    return {};

  std::string out;
  for (size_t n = 0; n < turns.size(); ++n)
  {
    out += fmt::format("Turn {}\n", n + 1);
    for (const std::string& act : turns[n].acts)
      out += fmt::format("  {}\n", act);
    std::vector<std::string> changes;
    for (const Key& key : turns[n].order)
    {
      const int before = turns[n].hp.at(key).first;
      int after = hp.at(key).first;
      if (n + 1 < turns.size())
      {
        if (const auto it = turns[n + 1].hp.find(key); it != turns[n + 1].hp.end())
          after = it->second.first;
      }
      if (after != before)
      {
        changes.push_back(fmt::format("GBA {} {} {} -> {}{}", key.first + 1, key.second, before,
                                      after, after == 0 ? " (fainted)" : ""));
      }
    }
    out += "  Result: ";
    if (n + 1 == turns.size())
      out += "not known (XD sends the GBAs nothing after the last turn)";
    else if (changes.empty())
    {
      out += "no HP changes";
    }
    else
    {
      for (size_t i = 0; i < changes.size(); ++i)
        out += (i ? "; " : "") + changes[i];
    }
    out += '\n';
  }
  std::array<bool, 2> alive{};
  for (const auto& [key, value] : hp)
    alive[key.first] = alive[key.first] || value.first > 0;
  out += "Battle over.";
  if (alive[0] != alive[1])
    out += fmt::format(" GBA {} wins.", alive[0] ? 1 : 2);
  out += '\n';
  return out;
}

// Must hold s_mutex. At session close: battle_<stamp>.txt beside the log, when there was a battle.
void WriteBattleTranscript()
{
  const std::string body = BuildBattleTranscript(std::move(s_battle));
  s_battle.clear();
  if (body.empty())
    return;
  const std::string path = File::GetUserPath(D_GBAUSER_IDX) + "Battle Records/battle_" +
                           s_session_stamp + ".txt";
  File::CreateFullPath(path);
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out)
    return;
  out << "OrreLink battle transcript, session " << s_session_stamp << "\n";
  out << "Moves are each Pokemon's choice; Result is the HP change over the turn.\n\n";
  out << body;
}
}  // namespace

void Flush()
{
  std::lock_guard lock(s_mutex);
  if (s_file.is_open())
    s_file.flush();
  if (s_trace.is_open())
    s_trace.flush();
}

void BattleLinkWrite(int channel, u64 tick, const u8* data)
{
  if (channel < 0 || channel > 3)
    return;
  const BattleWord word{data[0], data[1], data[2], data[3]};
  // Keep-alives between messages.
  if (word == BattleWord{0x47, 0x58, 0x58, 0x45} || word == BattleWord{0, 0, 0, 0xAA} ||
      word == BattleWord{0, 0, 0, 0x11})
  {
    return;
  }
  std::lock_guard lock(s_mutex);
  if (!s_file.is_open())
    return;
  auto& building = s_battle_building[channel];
  if (word == BattleWord{0, 0, 0, 0x66})
  {
    building.emplace();
    building->reserve(BATTLE_BLOCK_WORDS);
    return;
  }
  if (!building)
    return;
  building->push_back(word);
  if (building->size() == BATTLE_BLOCK_WORDS)
  {
    s_battle.push_back({tick, 0, channel, std::move(*building), {}});
    building.reset();
  }
}

void BattleLinkRead(int channel, u64 tick, const u8* data)
{
  // A choice: [slot or party index, target, 1 fight / 2 switch, 0].
  if (data[3] != 0 || (data[2] != 1 && data[2] != 2) || data[0] > 5)
    return;
  std::lock_guard lock(s_mutex);
  if (!s_file.is_open())
    return;
  s_battle.push_back({tick, 1, channel, {}, {data[0], data[1], data[2], data[3]}});
}

bool IsTracingLink()
{
  std::lock_guard lock(s_mutex);
  return s_trace.is_open();
}

void TraceLink(int channel, u64 tick, char dir, const u8* bytes, int len, int jstat)
{
  std::lock_guard lock(s_mutex);
  if (!s_trace.is_open() || s_trace_bytes >= MAX_TRACE_BYTES || len <= 0)
    return;
  std::string line = fmt::format("{} {} {} ", tick, channel, dir);
  for (int i = 0; i < len; ++i)
    line += fmt::format("{:02x}", bytes[i]);
  if (jstat >= 0)
    line += fmt::format(" {:02x}", jstat);
  line += '\n';
  s_trace_bytes += line.size();
  s_trace << line;
}

std::string FilePath()
{
  std::lock_guard lock(s_mutex);
  return s_path;
}

void OnDeviceCreated()
{
  std::lock_guard lock(s_mutex);
  if (s_live_devices++ != 0)
    return;  // session already open; just count this device

  // D_GBAUSER_IDX already ends in a path separator (".../GBA/").
  const std::string dir = File::GetUserPath(D_GBAUSER_IDX);
  PruneOldLogs(dir);
  s_session_stamp = Timestamp("%Y%m%d_%H%M%S");
  s_path = dir + "gba_detect_" + s_session_stamp + ".log";
  s_battle.clear();
  for (auto& building : s_battle_building)
    building.reset();
  s_file.close();
  s_file.clear();
  s_file.open(s_path, std::ios::out | std::ios::trunc);
  s_trace.close();
  s_trace.clear();
  s_trace_bytes = 0;
  if (Config::Get(Config::MAIN_XD_RECORD_LINK_TRAFFIC))
  {
    s_trace.open(dir + "linktrace_" + Timestamp("%Y%m%d_%H%M%S") + ".txt",
                 std::ios::out | std::ios::trunc);
    if (s_trace.is_open())
      s_trace << "cols: tick sock dir(>=GC to GBA, <=GBA to GC) bytes(hex) [jstat]\n";
  }
  s_bytes = 0;
  s_capped.store(false, std::memory_order_relaxed);
  for (auto& per_channel : s_joy_repeat)
    for (auto& slot : per_channel)
      slot = {};

  WriteLine(fmt::format("gba_detect log  path={}", s_path), true);
  WriteLine(fmt::format("session start  {}", Timestamp("%Y-%m-%d %H:%M:%S")), true);
  WriteLine("cols: t=<gc_tick|boot> sock=<SI channel; link GBAs=2,3> <event> <key=val...>", true);
  WriteLine("note: a per-socket 'role' line prints once under netplay; rd=/wr= in sum lines are "
            "ungated data-command counts",
            true);
  // Pre-boot context recorded before this file existed (Battle Style picks,
  // forced flags): flushed here so the log that describes a battle also says
  // what was configured into it.
  for (const std::string& note : s_boot_notes)
    WriteLine(fmt::format("t=boot {}", note), true);
  s_boot_notes.clear();
  // The path is deliberately not announced on screen. FilePath() is what the UI's share/reveal
  // action uses, and the release notes give the folder per platform -- neither needs a banner
  // across the game at every boot.
}

void OnDeviceDestroyed()
{
  std::lock_guard lock(s_mutex);
  if (s_live_devices > 0 && --s_live_devices == 0)
  {
    WriteLine(fmt::format("session end  {}", Timestamp("%Y-%m-%d %H:%M:%S")), true);
    s_file.flush();
    s_file.close();
    if (s_trace.is_open())
    {
      s_trace.flush();
      s_trace.close();
    }
    WriteBattleTranscript();
  }
}

bool IsSessionOpen()
{
  std::lock_guard lock(s_mutex);
  return s_file.is_open();
}

void NoteBoot(const std::string& line)
{
  std::lock_guard lock(s_mutex);
  if (s_file.is_open())
  {
    WriteLine(fmt::format("t=boot {}", line), true);
    return;
  }
  if (s_boot_notes.size() >= MAX_BOOT_NOTES)
    s_boot_notes.erase(s_boot_notes.begin());
  s_boot_notes.push_back(line);
}

void LogPostSession(const std::string& detail)
{
  std::lock_guard lock(s_mutex);
  if (s_path.empty())
    return;

  const std::string line = fmt::format("t=post {}", detail);

  // A room can close while the session is still live (host closed the window
  // mid-battle); then the normal sink is still the right one.
  if (s_file.is_open())
  {
    WriteLine(line, true);
    return;
  }

  // Session already closed: reopen just long enough to append. Not routed
  // through WriteLine because the byte cap is accounted against the live
  // stream, and these lines are few, fixed in number, and the whole point of
  // the log for this feature.
  std::ofstream out(s_path, std::ios::out | std::ios::app);
  if (!out.is_open())
    return;
  out << line << '\n';
  out.flush();
}

void LogBios(int channel, bool official, const std::string& path, u64 size, bool netplay,
             bool load_ok)
{
  if (s_capped.load(std::memory_order_relaxed))
    return;
  std::lock_guard lock(s_mutex);
  WriteLine(fmt::format("t=boot sock={} BIOS {} size={} netplay={} load={} path={}", channel,
                        official ? "official" : "bundled", size, netplay ? 1 : 0,
                        load_ok ? "ok" : "FAIL", path.empty() ? "<none>" : path),
            true);
}

void LogEvent(int channel, u64 tick, const char* event, const std::string& detail, bool flush)
{
  if (s_capped.load(std::memory_order_relaxed))
    return;
  std::lock_guard lock(s_mutex);
  WriteLine(fmt::format("t={} sock={} {} {}", tick, channel, event, detail), flush);
}

void LogJoybus(int channel, u64 tick, char dir, const u8* bytes, int len, bool sent, u8 jsa,
               bool suppress_repeats)
{
  if (s_capped.load(std::memory_order_relaxed))
    return;
  std::lock_guard lock(s_mutex);
  const int di = dir == '>' ? 0 : 1;
  JoyRepeatSlot* slot =
      (channel >= 0 && channel < 4) ? &s_joy_repeat[channel][di] : nullptr;
  const u8 aux = dir == '>' ? static_cast<u8>(sent ? 1 : 0) : jsa;
  std::string rep_field;
  if (slot)
  {
    const bool same = slot->len == len && slot->aux == aux &&
                      std::equal(slot->bytes, slot->bytes + 5, bytes);
    if (suppress_repeats && same)
    {
      ++slot->repeats;  // counted, not written -- 96.8% of battle joy bytes
      return;
    }
    if (slot->repeats != 0)
    {
      rep_field = fmt::format(" rep={}", slot->repeats);
      slot->repeats = 0;
    }
    std::copy(bytes, bytes + 5, slot->bytes);
    slot->len = len;
    slot->aux = aux;
  }
  if (dir == '>')
    WriteLine(fmt::format("t={} sock={} joy > {:02x}{:02x}{:02x}{:02x}{:02x} len={} sent={}{}",
                          tick, channel, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], len,
                          sent ? 1 : 0, rep_field),
              false);  // bounded burst; flushed by the next event/summary line
  else
    WriteLine(fmt::format("t={} sock={} joy < {:02x}{:02x}{:02x}{:02x}{:02x} len={} jsa={:02x}{}",
                          tick, channel, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], len, jsa,
                          rep_field),
              false);
}

void LogSummary(int channel, u64 tick, bool rom, bool gba, bool loc, bool lk, u32 win, u32 rst,
                u32 prb, bool est, bool lck, u8 cmd, u32 rd, u32 wr)
{
  if (s_capped.load(std::memory_order_relaxed))
    return;
  std::lock_guard lock(s_mutex);
  WriteLine(fmt::format("t={} sock={} sum rom={} gba={} loc={} lk={} win={} rst={} prb={} est={} "
                        "lck={} cmd={:02x} rd={} wr={}",
                        tick, channel, rom ? 1 : 0, gba ? 1 : 0, loc ? 1 : 0, lk ? 1 : 0, win, rst,
                        prb, est ? 1 : 0, lck ? 1 : 0, cmd, rd, wr),
            true);
}
namespace
{
std::atomic<u64> s_last_pad_press{0};
}  // namespace

void NotePadPress(u64 tick)
{
  s_last_pad_press.store(tick, std::memory_order_relaxed);
}

u64 LastPadPress()
{
  return s_last_pad_press.load(std::memory_order_relaxed);
}
}  // namespace GBADetectLog
