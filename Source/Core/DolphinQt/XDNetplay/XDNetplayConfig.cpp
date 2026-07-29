// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/XDNetplay/XDNetplayConfig.h"

#include <string>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/InputConfig.h"

#include "Core/Config/MainSettings.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GBAPadEmu.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/HW/GBACore.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/NetPlayProto.h"
#include "Core/NetPlayServer.h"

namespace XDNetplay
{
namespace
{
constexpr u64 GBA_BIOS_SIZE = 16384;

#ifdef HAS_LIBMGBA
// Copy a bundled Sys template into place. Existing destinations are never
// overwritten (a player's teams live there); missing sources are tolerated.
bool SeedFile(const std::string& source, const std::string& destination)
{
  if (destination.empty())
    return false;
  if (File::Exists(destination))
    return true;
  if (!File::Exists(source))
    return false;
  File::CreateFullPath(destination);
  return File::CopyRegularFile(source, destination);
}
#endif
}  // namespace

bool IsXdGameId(const std::string& game_id)
{
  return game_id.rfind(XD_GAME_ID, 0) == 0;
}

bool EnsureGbaConfig()
{
  // Rom2/Rom3 point at the Emerald dump the launcher imported. Re-assert both
  // only while the stored path still exists on disk, so a deleted dump shows
  // up as a red checklist row instead of being silently re-broken at boot.
  const std::string rom_path = Config::Get(Config::MAIN_GBA_ROM_PATHS[1]);
  if (!rom_path.empty() && File::Exists(rom_path))
  {
    Config::SetBaseOrCurrent(Config::MAIN_GBA_ROM_PATHS[1], rom_path);
    Config::SetBaseOrCurrent(Config::MAIN_GBA_ROM_PATHS[2], rom_path);
  }

  // Port 1 pad, ports 2/3 integrated GBAs -- the fixed XD link layout.
  Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(0), SerialInterface::SIDEVICE_GC_CONTROLLER);
  Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(1),
                           SerialInterface::SIDEVICE_GC_GBA_EMULATED);
  Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(2),
                           SerialInterface::SIDEVICE_GC_GBA_EMULATED);

  // The $XD OU Fixes code must be active locally even for solo boots.
  Config::SetBaseOrCurrent(Config::MAIN_ENABLE_CHEATS, true);

  // Netplay tuned for the XD link: small fixed-delay buffer, no UPnP noise.
  Config::SetBaseOrCurrent(Config::NETPLAY_BUFFER_SIZE, 5);
  Config::SetBaseOrCurrent(Config::NETPLAY_NETWORK_MODE, "fixeddelay");
  Config::SetBaseOrCurrent(Config::NETPLAY_USE_UPNP, false);

  Config::Save();
  return true;
}

void ApplyStartForcing(NetPlay::NetPlayServer* server)
{
  if (!server)
    return;

  // Netplay rebuilds every SI channel and GBA ROM path at boot purely from the
  // session's pad_map + gba_config, ignoring local SIDevice settings. Mirror
  // the Android build's fixed two-player layout:
  //   port 1 (idx0) = host GC controller
  //   port 2 (idx1) = host GBA
  //   port 3 (idx2) = guest GBA
  //   port 4 (idx3) = unused
  // The host is always pid 1 (its loopback client connects first) and XD is a
  // strict two-player game, so the guest is pid 2.
  constexpr NetPlay::PlayerId HOST_PID = 1;
  constexpr NetPlay::PlayerId GUEST_PID = 2;

  NetPlay::PadMappingArray pad_map{};
  pad_map[0] = HOST_PID;
  pad_map[1] = HOST_PID;
  pad_map[2] = GUEST_PID;
  pad_map[3] = 0;

  NetPlay::GBAConfigArray gba_config{};
  gba_config[1].enabled = true;
  gba_config[2].enabled = true;

  // Sync the host's team saves to the guest, give each player only their own
  // GBA window, and keep the $XD OU Fixes code active and synced -- if only
  // one side ran it the two would desync on turn 1.
  Config::SetBaseOrCurrent(Config::NETPLAY_SAVEDATA_LOAD, true);
  Config::SetBaseOrCurrent(Config::NETPLAY_HIDE_REMOTE_GBAS, true);
  Config::SetBaseOrCurrent(Config::MAIN_ENABLE_CHEATS, true);
  Config::SetBaseOrCurrent(Config::NETPLAY_SYNC_CODES, true);

  // SetGBAConfig(update_rom=true) reads MAIN_GBA_ROM_PATHS[1]/[2] to fill each
  // enabled slot's hash/title, which the guest hash-matches against its local
  // copy. Both must precede RequestStartGame; the caller issues that.
  server->SetPadMapping(pad_map);
  server->SetGBAConfig(gba_config, /*update_rom=*/true);
}

bool CheckOfficialBios(std::string* path_out)
{
  std::string path = Config::Get(Config::MAIN_GBA_BIOS_PATH);
  if (path.empty())
    path = File::GetUserPath(D_GBAUSER_IDX) + GBA_BIOS;
  if (path_out)
    *path_out = path;
  return IsOfficialBiosFile(path);
}

bool IsOfficialBiosFile(const std::string& path)
{
  File::IOFile file(path, "rb");
  if (!file || file.GetSize() != GBA_BIOS_SIZE)
    return false;

  std::vector<u8> data(GBA_BIOS_SIZE);
  if (!file.ReadBytes(data.data(), data.size()))
    return false;

  const std::string digest =
      Common::SHA1::DigestToString(Common::SHA1::CalculateDigest(data.data(), data.size()));
  return Common::CaseInsensitiveEquals(digest, OFFICIAL_GBA_BIOS_SHA1);
}

bool ImportOfficialBios(const std::string& path)
{
  if (!IsOfficialBiosFile(path))
    return false;

  const std::string destination = File::GetUserPath(D_GBAUSER_IDX) + GBA_BIOS;
  if (path != destination)
  {
    File::CreateFullPath(destination);
    if (!File::CopyRegularFile(path, destination))
      return false;
  }
  Config::SetBaseOrCurrent(Config::MAIN_GBA_BIOS_PATH, destination);
  Config::Save();
  return true;
}

bool AutoImportOfficialBios(const std::string& directory)
{
  if (CheckOfficialBios(nullptr))
    return true;
  if (!File::IsDirectory(directory))
    return false;

  const File::FSTEntry entry = File::ScanDirectoryTree(directory, false);
  for (const File::FSTEntry& child : entry.children)
  {
    if (child.isDirectory || child.size != GBA_BIOS_SIZE)
      continue;

    std::string name = child.virtualName;
    Common::ToLower(&name);
    if (!name.ends_with(".bin") || !IsOfficialBiosFile(child.physicalName))
      continue;

    const std::string destination = File::GetUserPath(D_GBAUSER_IDX) + GBA_BIOS;
    if (child.physicalName != destination)
    {
      File::CreateFullPath(destination);
      if (!File::CopyRegularFile(child.physicalName, destination))
        continue;
    }
    Config::SetBaseOrCurrent(Config::MAIN_GBA_BIOS_PATH, destination);
    Config::Save();
    return true;
  }
  return false;
}

bool GbaInputMapped()
{
  if (!Pad::IsGBAInitialized())
    return false;

  const ControllerEmu::ControlGroup* buttons = Pad::GetGBAGroup(0, GBAPadGroup::Buttons);
  if (!buttons)
    return false;

  for (const auto& control : buttons->controls)
  {
    if (control->control_ref && !control->control_ref->GetExpression().empty())
      return true;
  }
  return false;
}

bool ApplyDefaultGbaInput()
{
  if (!Pad::IsGBAInitialized())
    return false;

  InputConfig* const config = Pad::GetGBAConfig();
  ControllerEmu::EmulatedController* const controller = config->GetController(0);
  if (!controller)
    return false;

  controller->LoadDefaults(g_controller_interface);
  config->SaveConfig();
  return GbaInputMapped();
}

bool SeedTeamSaves()
{
#ifdef HAS_LIBMGBA
  const std::string rom2 = Config::Get(Config::MAIN_GBA_ROM_PATHS[1]);
  const std::string rom3 = Config::Get(Config::MAIN_GBA_ROM_PATHS[2]);
  if (rom2.empty() || rom3.empty())
    return false;

  const std::string sys_dir = File::GetSysDirectory() + "XDNetplay/";
  const bool save2 = SeedFile(sys_dir + "EMERALD-2.sav", HW::GBA::Core::GetSavePath(rom2, 1));
  const bool save3 = SeedFile(sys_dir + "EMERALD-3.sav", HW::GBA::Core::GetSavePath(rom3, 2));
  return save2 && save3;
#else
  return false;
#endif
}

bool SeedVsModeGci()
{
  const std::string destination =
      File::GetUserPath(D_GCUSER_IDX) + "USA/Card A/01-GXXE-PokemonXD.gci";
  if (File::Exists(destination))
    return true;

  const std::string source = File::GetSysDirectory() + "XDNetplay/01-GXXE-PokemonXD.gci";
  if (!File::Exists(source))
    return false;

  File::CreateFullPath(destination);
  return File::CopyRegularFile(source, destination);
}
}  // namespace XDNetplay
