// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GBADetectLog.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"

#include "VideoCommon/OnScreenDisplay.h"

namespace GBADetectLog
{
namespace
{
// Hard ceiling on one session's file, so a wedged session that emits summaries
// for hours can never fill the disk. 4 MiB: a 35-minute session on the old
// 1 MiB budget hit the cap after ~3.5 minutes of XD's connected-idle RESET
// polling and went dark for the interesting part; the new budget plus the
// smaller post-reset bursts holds roughly half an hour of worst-case traffic.
constexpr u64 MAX_LOG_BYTES = 4u << 20;

std::mutex s_mutex;
std::ofstream s_file;    // held open for the session; never fopen'd per line
std::string s_path;
u64 s_bytes = 0;         // running size, enforces the cap
int s_live_devices = 0;  // ref-count -> session begin/end
// Atomic so every Log* can early-out lock-free and stop FORMATTING once capped
// (Review #1 I5: the byte cap must bound CPU/lock cost, not just disk).
std::atomic<bool> s_capped{false};

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

// Keep only the newest KEEP_PREVIOUS previous session logs (the timestamped
// names sort lexicographically = chronologically), and drop the un-timestamped
// legacy file from older builds. Called before each session's file is created,
// so at most KEEP_PREVIOUS + 1 logs ever exist.
constexpr size_t KEEP_PREVIOUS = 2;

void PruneOldLogs(const std::string& dir)
{
  File::Delete(dir + "gba_detect.log");  // legacy single-file name
  std::vector<std::string> old_logs;
  const File::FSTEntry tree = File::ScanDirectoryTree(dir, false);
  for (const File::FSTEntry& child : tree.children)
  {
    const std::string& name = child.virtualName;
    if (!child.isDirectory && name.rfind("gba_detect_", 0) == 0 &&
        name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0)
    {
      old_logs.push_back(name);
    }
  }
  std::sort(old_logs.begin(), old_logs.end(), std::greater<std::string>());
  for (size_t i = KEEP_PREVIOUS; i < old_logs.size(); ++i)
    File::Delete(dir + old_logs[i]);
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
}  // namespace

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
  s_path = dir + "gba_detect_" + Timestamp("%Y%m%d_%H%M%S") + ".log";
  s_file.close();
  s_file.clear();
  s_file.open(s_path, std::ios::out | std::ios::trunc);
  s_bytes = 0;
  s_capped.store(false, std::memory_order_relaxed);

  WriteLine(fmt::format("gba_detect log  path={}", s_path), true);
  WriteLine(fmt::format("session start  {}", Timestamp("%Y-%m-%d %H:%M:%S")), true);
  WriteLine("cols: t=<gc_tick|boot> sock=<SI channel; link GBAs=2,3> <event> <key=val...>", true);
  WriteLine("note: a per-socket 'role' line prints once under netplay; rd=/wr= in sum lines are "
            "ungated data-command counts",
            true);

  OSD::AddMessage(fmt::format("GBA detect log: {}", s_path), 12000, OSD::Color::CYAN);
}

void OnDeviceDestroyed()
{
  std::lock_guard lock(s_mutex);
  if (s_live_devices > 0 && --s_live_devices == 0)
  {
    WriteLine(fmt::format("session end  {}", Timestamp("%Y-%m-%d %H:%M:%S")), true);
    s_file.flush();
    s_file.close();
  }
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

void LogJoybus(int channel, u64 tick, char dir, const u8* bytes, int len, bool sent, u8 jsa)
{
  if (s_capped.load(std::memory_order_relaxed))
    return;
  std::lock_guard lock(s_mutex);
  if (dir == '>')
    WriteLine(fmt::format("t={} sock={} joy > {:02x}{:02x}{:02x}{:02x}{:02x} len={} sent={}", tick,
                          channel, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], len,
                          sent ? 1 : 0),
              false);  // bounded burst; flushed by the next event/summary line
  else
    WriteLine(fmt::format("t={} sock={} joy < {:02x}{:02x}{:02x}{:02x}{:02x} len={} jsa={:02x}",
                          tick, channel, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], len, jsa),
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
}  // namespace GBADetectLog
