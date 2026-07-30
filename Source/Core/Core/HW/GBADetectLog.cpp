// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GBADetectLog.h"

#include <atomic>
#include <ctime>
#include <fstream>
#include <mutex>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"

#include "VideoCommon/OnScreenDisplay.h"

namespace GBADetectLog
{
namespace
{
// Hard ceiling on one session's file, so a wedged session that emits summaries
// for hours can never fill the disk. 1 MiB is tens of thousands of lines.
constexpr u64 MAX_LOG_BYTES = 1u << 20;

std::mutex s_mutex;
std::ofstream s_file;    // held open for the session; never fopen'd per line
std::string s_path;
u64 s_bytes = 0;         // running size, enforces the cap
int s_live_devices = 0;  // ref-count -> session begin/end
// Atomic so every Log* can early-out lock-free and stop FORMATTING once capped
// (Review #1 I5: the byte cap must bound CPU/lock cost, not just disk).
std::atomic<bool> s_capped{false};

std::string Timestamp()
{
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buf[32] = {};
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
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

const std::string& FilePath()
{
  // D_GBAUSER_IDX already ends in a path separator (".../GBA/").
  static const std::string path = File::GetUserPath(D_GBAUSER_IDX) + "gba_detect.log";
  return path;
}

void OnDeviceCreated()
{
  std::lock_guard lock(s_mutex);
  if (s_live_devices++ != 0)
    return;  // session already open; just count this device

  s_path = FilePath();
  s_file.close();
  s_file.clear();
  s_file.open(s_path, std::ios::out | std::ios::trunc);
  s_bytes = 0;
  s_capped.store(false, std::memory_order_relaxed);

  WriteLine(fmt::format("gba_detect.log  path={}", s_path), true);
  WriteLine(fmt::format("session start  {}", Timestamp()), true);
  WriteLine("cols: t=<gc_tick|boot> sock=<SI channel; link GBAs=2,3> <event> <key=val...>", true);
  WriteLine("note: '> ... sent=0' == command withheld by quiet window (NOT GBA silence); "
            "a per-socket 'role' line prints once under netplay",
            true);

  OSD::AddMessage(fmt::format("GBA detect log: {}", s_path), 12000, OSD::Color::CYAN);
}

void OnDeviceDestroyed()
{
  std::lock_guard lock(s_mutex);
  if (s_live_devices > 0 && --s_live_devices == 0)
  {
    WriteLine(fmt::format("session end  {}", Timestamp()), true);
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

void LogEvent(int channel, u64 tick, const char* event, const std::string& detail)
{
  if (s_capped.load(std::memory_order_relaxed))
    return;
  std::lock_guard lock(s_mutex);
  WriteLine(fmt::format("t={} sock={} {} {}", tick, channel, event, detail), true);
}

void LogJoybus(int channel, u64 tick, char dir, const u8* bytes, int len, bool sent)
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
    WriteLine(fmt::format("t={} sock={} joy < {:02x}{:02x}{:02x}{:02x}{:02x} len={}", tick, channel,
                          bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], len),
              false);
}

void LogSummary(int channel, u64 tick, bool rom, bool gba, bool loc, bool lk, u32 win, u32 rst,
                u32 prb, bool est, bool lck, u8 cmd)
{
  if (s_capped.load(std::memory_order_relaxed))
    return;
  std::lock_guard lock(s_mutex);
  WriteLine(fmt::format("t={} sock={} sum rom={} gba={} loc={} lk={} win={} rst={} prb={} est={} "
                        "lck={} cmd={:02x}",
                        tick, channel, rom ? 1 : 0, gba ? 1 : 0, loc ? 1 : 0, lk ? 1 : 0, win, rst,
                        prb, est ? 1 : 0, lck ? 1 : 0, cmd),
            true);
}
}  // namespace GBADetectLog
