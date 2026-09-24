// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>
#include <variant>
#include <vector>

#include "Common/CommonTypes.h"

namespace Common
{
class IniFile;
}

namespace Core
{
class CPUThreadGuard;
}

namespace ActionReplay
{
struct AREntry
{
  AREntry() = default;
  AREntry(u32 _addr, u32 _value) : cmd_addr(_addr), value(_value) {}
  u32 cmd_addr = 0;
  u32 value = 0;
};
constexpr bool operator==(const AREntry& left, const AREntry& right)
{
  return left.cmd_addr == right.cmd_addr && left.value == right.value;
}

struct ARCode
{
  std::string name;
  std::vector<AREntry> ops;
  bool enabled = false;
  bool default_enabled = false;
  bool user_defined = false;
};

void RunAllActive(const Core::CPUThreadGuard& cpu_guard);

// XD Netplay live battle style: one extra code that netplay installs at the same emulated moment
// on every machine (NetPlayClient applies it at a numbered pad pop). RunAllActive runs it after the
// active codes every frame, so for an address both touch it has the last word, and it runs whether
// or not cheats are enabled, because it only ever comes from netplay and the room decides it. It is
// held apart from the active codes so nothing that rebuilds those (the cheat manager, a code sync)
// can drop it. The host refuses live changes in hardcore mode (a synced setting), so nothing
// here consults this machine's own achievements state. Cleared by PatchEngine::Shutdown.
void SetLiveCode(std::vector<AREntry> ops);
void ClearLiveCode();

void ApplyCodes(std::span<const ARCode> codes, const std::string& game_id, u16 revision);
void SetSyncedCodesAsActive();
void UpdateSyncedCodes(std::span<const ARCode> codes);
std::vector<ARCode> ApplyAndReturnCodes(std::span<const ARCode> codes);
// OrreLink: the codes that will run this session (synced set on a guest, INI set
// on the host/solo), for the per-session gba_detect log.
std::vector<ARCode> GetActiveCodesSnapshot();
void AddCode(ARCode new_code);
size_t CountEnabledCodes();
void LoadAndApplyCodes(const Common::IniFile& global_ini, const Common::IniFile& local_ini,
                       const std::string& game_id, u16 revision);

std::vector<ARCode> LoadCodes(const Common::IniFile& global_ini, const Common::IniFile& local_ini);
void SaveCodes(Common::IniFile* local_ini, std::span<const ARCode> codes);

using EncryptedLine = std::string;
std::variant<std::monostate, AREntry, EncryptedLine> DeserializeLine(const std::string& line);
std::string SerializeLine(const AREntry& op);

void EnableSelfLogging(bool enable);
std::vector<std::string> GetSelfLog();
void ClearSelfLog();
bool IsSelfLogging();
}  // namespace ActionReplay
