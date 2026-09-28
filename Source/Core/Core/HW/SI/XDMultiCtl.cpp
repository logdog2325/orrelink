// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/SI/XDMultiCtl.h"

#include <mutex>

namespace XDMultiCtl
{
namespace
{
// A leaf lock: nothing is taken while holding it. Both users are the host's CPU thread; the lock
// only keeps a closing game's core (which can still poll for a moment) from racing a new one.
std::mutex s_mutex;
u8 s_cmd = CMD_NONE;
u64 s_boot_seq = 0;
bool s_in_flight = false;
}  // namespace

const char* CommandName(u8 cmd)
{
  switch (cmd)
  {
  case CMD_NONE:
    return "none";
  case CMD_TO_UNPLUG:
    return "unplug";
  case CMD_TO_GBA:
    return "gba";
  case CMD_TO_PAD:
    return "pad";
  case CMD_REPLUG:
    return "replug";
  default:
    return "?";
  }
}

bool Post(u8 cmd, u64 boot_seq)
{
  std::lock_guard lk(s_mutex);
  if (cmd == CMD_NONE || s_cmd != CMD_NONE || s_in_flight)
    return false;
  s_cmd = cmd;
  s_boot_seq = boot_seq;
  return true;
}

u8 Take(u64 boot_seq)
{
  std::lock_guard lk(s_mutex);
  if (s_cmd == CMD_NONE)
    return CMD_NONE;
  const u8 cmd = s_cmd;
  s_cmd = CMD_NONE;
  if (s_boot_seq != boot_seq)
    return CMD_NONE;  // posted by another game's device
  s_in_flight = true;
  return cmd;
}

bool Busy()
{
  std::lock_guard lk(s_mutex);
  return s_cmd != CMD_NONE || s_in_flight;
}

void MarkApplied()
{
  std::lock_guard lk(s_mutex);
  s_in_flight = false;
}

void Reset()
{
  std::lock_guard lk(s_mutex);
  s_cmd = CMD_NONE;
  s_boot_seq = 0;
  s_in_flight = false;
}
}  // namespace XDMultiCtl
