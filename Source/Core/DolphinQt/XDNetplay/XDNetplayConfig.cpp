// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/XDNetplay/XDNetplayConfig.h"

#include <optional>
#include <string>
#include <string_view>
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
#include "InputCommon/ControllerInterface/CoreDevice.h"
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

#include "UICommon/XDNetplay/BattleCustomizer.h"
#include "UICommon/XDNetplay/DisposableSave.h"
#include "UICommon/XDNetplay/FormatRules.h"

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

std::string MakeOpenSessionName(const std::string& nickname, int format_key_value)
{
  // Mirrors XdMatchmaker.sessionName() on Android, byte for byte. The
  // "[Orre] " / "[OU] " room tags are purely human-readable labels (no
  // matchmaking filter keys off them); with the Format pick on Free the name
  // is byte-identical to the pre-Format builds.
  const std::string base = "XD [OC] " + (nickname.empty() ? std::string("Player") : nickname);
  return XDNetplay::FormatRules::FormatSessionTag(format_key_value) + base;
}

bool LooksLikeXdSession(const std::string& published_game_name)
{
  std::string lower = published_game_name;
  Common::ToLower(&lower);
  // "pokémon xd" is matched as raw UTF-8: Common::ToLower only folds ASCII, so
  // the accented byte pair survives untouched and the literal below (already
  // lowercase) lines up. Desktop hosts publish the ASCII "Pokemon XD ..."
  // title anyway; the accented needle is for title databases that localize it.
  for (const std::string_view needle :
       {"gxxe01", "gale of darkness", "pokemon xd", "pokémon xd"})
  {
    if (lower.find(needle) != std::string::npos)
      return true;
  }
  return false;
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

  // Cheats (the $XD OU Fixes code) are DERIVED state: BattleCustomizer::
  // PrepareForStart turns them on exactly when the session needs the AR engine
  // (a style/rules block, or Format = OU -- the pick that replaced the old
  // standalone toggle) and off otherwise. Don't force them here.

  // Session-boundary scrub for the cosmetic battle-style feature: remove any
  // "$OrreLink Battle Style" block a crashed session left orphaned in the
  // local GXXE01.ini, so stale cosmetics cannot leak into a solo boot or the
  // next session. Deliberately NOT BeginSession: this helper also runs for
  // SOLO boots, and claiming the netplay lifecycle there made the solo
  // cleanup hook stand down -- EndSession (which restores the reconciled
  // cheats flag) then waited for a room-closed event a solo boot never gets.
  // The netplay room itself calls BeginSession when it opens
  // (NetPlayDialog::show / Android nativeHost).
  BattleCustomizer::ScrubLeftovers();

  // Same boundary, same reason, for the disposable netplay saves: if a crashed
  // hosted session left a <save>.netplayorig stash behind, the socket save
  // still holds the session's disposable -- put the real import back so a solo
  // boot plays the user's own save again (DisposableSave.h).
  DisposableSave::HealLeftoverSession();

  // Netplay tuned for the XD link: fixed delay, no UPnP noise.
  //
  // The buffer is only re-seeded while automatic sizing is on, where 5 is just
  // a starting point the host's sizer raises off the measured ping within a few
  // seconds of a guest joining. A host who turned Auto off picked their number
  // deliberately -- re-running this setup must not quietly stomp it back to 5.
  if (Config::Get(Config::NETPLAY_AUTO_BUFFER))
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
  // Sync codes so the HOST's choice governs both sides (if the host's Format
  // is OU, the $XD OU Fixes code carries to the guest; if not, both run
  // clean). Cheats themselves are reconciled by PrepareForStart below.
  Config::SetBaseOrCurrent(Config::NETPLAY_SAVEDATA_LOAD, true);
  Config::SetBaseOrCurrent(Config::NETPLAY_HIDE_REMOTE_GBAS, true);
  Config::SetBaseOrCurrent(Config::NETPLAY_SYNC_CODES, true);

  // Rebuild the "$OrreLink Battle Style" cosmetic AR block from the host's
  // launcher selections plus the guest's submitted model, and force cheats on
  // when (and only when) the block is non-empty so it actually loads and
  // ships. Must run before the caller's RequestStartGame: that is what
  // snapshots MAIN_ENABLE_CHEATS (SetupNetSettings) and re-reads the local
  // GXXE01.ini off disk (SyncCodes).
  BattleCustomizer::PrepareForStart();

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

namespace
{
// The keyboard device of this platform: DInput "Keyboard Mouse" on Windows, Quartz on
// macOS, XInput2 on Linux (the same three InputConfig treats as keyboard sources).
// Keyboards sort first in ControllerInterface (DEFAULT_DEVICE_SORT_PRIORITY), so
// LoadDefaults normally lands on one already; picking it explicitly guards the cases
// where it does not (a backend that failed to enumerate the keyboard at startup), since
// the GBA defaults are key names ("X", "Z", "RETURN") that no gamepad has. The actual
// field failure (a Windows joiner with no GBA input) came from the older version of this
// file: it only defaulted slot 0, never called UpdateReferences, and treated a mapping
// to a device that no longer exists as "mapped".
std::optional<ciface::Core::DeviceQualifier> KeyboardDevice()
{
  for (const std::string& device : g_controller_interface.GetAllDeviceStrings())
  {
    ciface::Core::DeviceQualifier q;
    q.FromString(device);
    if (q.source == "Quartz" || q.source == "XInput2" ||
        (q.source == "DInput" && q.name == "Keyboard Mouse"))
    {
      return q;
    }
  }
  return std::nullopt;
}

// Mapped = the slot's default device currently exists AND at least one button has an
// expression. A mapping to an unplugged gamepad is not "mapped".
bool GbaSlotMapped(int slot)
{
  const ControllerEmu::EmulatedController* controller = Pad::GetGBAConfig()->GetController(slot);
  if (!controller || !g_controller_interface.FindDevice(controller->GetDefaultDevice()))
    return false;
  const ControllerEmu::ControlGroup* buttons = Pad::GetGBAGroup(slot, GBAPadGroup::Buttons);
  if (!buttons)
    return false;
  for (const auto& control : buttons->controls)
  {
    if (control->control_ref && !control->control_ref->GetExpression().empty())
      return true;
  }
  return false;
}
}  // namespace

// Which local GBA slot a player's own GBA reads from is decided by NetPlay's pad map
// (NetPlayClient InGameToLocal counts the SI channels assigned to you, in order): the
// HOST owns channel 0 (GameCube controller) and channel 1 (GBA port 2), so the host's
// GBA is local slot 1 = "GBA 2"; a JOINER owns only channel 2 (GBA port 3), so theirs
// is local slot 0 = "GBA 1". Both must be mapped for the checklist to be honest.
bool GbaInputMapped()
{
  if (!Pad::IsGBAInitialized())
    return false;
  return GbaSlotMapped(0) && GbaSlotMapped(1);
}

bool ApplyDefaultGbaInput()
{
  if (!Pad::IsGBAInitialized())
    return false;

  InputConfig* const config = Pad::GetGBAConfig();
  const std::optional<ciface::Core::DeviceQualifier> keyboard = KeyboardDevice();
  for (int slot = 0; slot < 4; ++slot)
  {
    ControllerEmu::EmulatedController* const controller = config->GetController(slot);
    if (!controller)
      return false;
    controller->LoadDefaults(g_controller_interface);
    if (keyboard)
      controller->SetDefaultDevice(*keyboard);
    controller->UpdateReferences(g_controller_interface);
  }
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
