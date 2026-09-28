// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/MultiStart.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/AchievementManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Core.h"
#include "Core/NetPlayServer.h"
#include "Core/System.h"

#include "UICommon/XDNetplay/BattleCustomizer.h"
#include "UICommon/XDNetplay/FormatRules.h"
#include "UICommon/XDNetplay/TeamInjector.h"

namespace XDNetplay
{
MultiStartOutcome PrepareMultiStart(NetPlay::NetPlayServer& server,
                                    const NetPlay::XdStartClaim& claim)
{
  MultiStartOutcome outcome;
  const auto refuse = [&outcome](std::string line) {
    outcome.refusal = std::move(line);
    return outcome;
  };

  // 1-5: the room and this machine.
  if (!Config::Get(Config::MAIN_XD_MULTI_ENABLED))
    return refuse("Can't start: Multi is not enabled.");
  // The previous game's GBA cores write their saves back as they shut down.
  if (!Core::IsUninitialized(Core::System::GetInstance()))
    return refuse("Can't start: the last battle is still closing.");
  // The port-1 control and the four GBAs need every machine to run every pad through its own
  // pushes (Host Input Authority and Golf carry them another way).
  if (server.IsHostInputAuthority())
    return refuse("Can't start: Multi needs Fixed Delay network mode.");
  // Hardcore mode filters this machine's Action Replay codes, not the synced copy the others run.
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
    return refuse("Can't start: RetroAchievements hardcore mode is on.");
  if (server.XdMultiNeedsNormalCore())
    return refuse("Can't start: Multi needs ForceCommonCoreOnMixedArch off.");

  // 6: the room's format is what every player was shown.
  const int room_format = server.GetXdRoomFormat();
  if (Config::Get(Config::MAIN_XD_FORMAT) != room_format)
    Config::SetBaseOrCurrent(Config::MAIN_XD_FORMAT, room_format);

  // 7-8: every seat taken, every seat's team in.
  for (size_t i = 0; i < claim.seats.size(); ++i)
  {
    if (claim.seats[i].pid == 0)
      return refuse(fmt::format("Can't start: Seat {} is open.", i + 2));
  }
  const bool host_fills_seat2 = claim.seats[0].pid == 1;
  std::vector<std::string> missing;
  std::vector<NetPlay::PlayerId> missing_pids;
  for (const auto& seat : claim.seats)
  {
    if (seat.team_in || std::ranges::find(missing_pids, seat.pid) != missing_pids.end())
      continue;
    missing_pids.push_back(seat.pid);
    missing.push_back(seat.name);
    outcome.notes.emplace_back(seat.pid, "Submit a team for Multi.");
  }
  if (!missing.empty())
    return refuse(fmt::format("Can't start: waiting for teams from {}.", fmt::join(missing, ", ")));

  // 9: one Emerald dump for all four GBAs.
#ifdef HAS_LIBMGBA
  std::string rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[1]);
  if (rom.empty() || !File::Exists(rom))
    rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[2]);
  if (rom.empty() || !File::Exists(rom))
    return refuse("Can't start: no Emerald ROM is set up.");
  for (int i = 0; i < 4; ++i)
    Config::SetBaseOrCurrent(Config::MAIN_GBA_ROM_PATHS[i], rom);
#else
  return refuse("Can't start: no Emerald ROM is set up.");
#endif

  // 10: the four teams under the room's rules, each on its own.
  const std::array<u32, 3> serials = {claim.seats[0].serial, claim.seats[1].serial,
                                      claim.seats[2].serial};
  const char* format_name = FormatRules::FormatDisplayName(room_format);
  const MultiTeamCheck check = CheckMultiTeams(room_format, serials, host_fills_seat2);
  if (!check.host.ok)
  {
    return refuse(
        fmt::format("Can't start: your team is not {} legal: {}.", format_name, check.host.reason));
  }
  for (size_t i = 0; i < claim.seats.size(); ++i)
  {
    if (check.seats[i].ok)
      continue;
    outcome.notes.emplace_back(claim.seats[i].pid,
                               fmt::format("Your team is not {} legal: {}. Submit a new team.",
                                           format_name, check.seats[i].reason));
    // No reason here: it would show the host a move or item from another player's hidden team.
    return refuse(
        fmt::format("Can't start: {}'s team is not {} legal.", claim.seats[i].name, format_name));
  }

  // 11-12: the boot copies every machine starts from, and their scrub once the game stops.
  const std::vector<u32> room_serials = server.XdRoomSerials();
  if (std::string error; !WriteMultiBootSaves(serials, host_fills_seat2, room_serials, &error))
  {
    ScrubMultiBootSaves();
    return refuse("Can't start: a team save could not be written.");
  }
  ArmTempPurgeOnStop();

  // 13: the session settings the link needs (as ApplyStartForcing does for 1v1).
  Config::SetBaseOrCurrent(Config::NETPLAY_SAVEDATA_LOAD, true);
  Config::SetBaseOrCurrent(Config::NETPLAY_HIDE_REMOTE_GBAS, true);
  Config::SetBaseOrCurrent(Config::NETPLAY_SYNC_CODES, true);

  // 14: the Multi block (rules, music, location; no model lines) and the live-style baseline.
  BattleCustomizer::PrepareForStart(BattleCustomizer::StartKind::Multi);
  BattleCustomizer::BeginLiveStyleForStart();

  // 15: Seat n plays SI port n. Channel 0 is the host's controller that becomes a GBA.
  constexpr NetPlay::PlayerId HOST_PID = 1;
  const NetPlay::PadMappingArray pad_map = {HOST_PID, claim.seats[0].pid, claim.seats[1].pid,
                                            claim.seats[2].pid};
  NetPlay::GBAConfigArray gba_config{};
  for (auto& config : gba_config)
    config.enabled = true;
  server.SetPadMapping(pad_map);
  server.SetGBAConfig(gba_config, /*update_rom=*/true);
  return outcome;
}

std::string MultiStartChatLine()
{
  if (Config::Get(Config::MAIN_XD_MULTI_PREVIEW_CANCEL_PIN))
    return "Multi: press A on your GBA when asked.";
  return "Multi: press A on your GBA when asked. B cancels for everyone.";
}

void AbortMultiStart()
{
  ScrubMultiBootSaves();
  DisarmTempPurge();
}

void OnStartAborted()
{
  if (IsTempPurgeArmed() && Core::IsUninitialized(Core::System::GetInstance()))
    AbortMultiStart();
}

NetPlay::XdTeamResult HandleTeamSubmission(const std::string& player, const std::string& text,
                                           const NetPlay::XdTeamTarget& target)
{
  (void)player;
  // The payload may carry an in-game trainer name, a model pick and a raise-to-100 request ahead
  // of the team, or be a party bundle (TeamInjector.h documents the grammar).
  const TeamSubmission submission = ParseTeamSubmissionPayload(text);
  std::string status;
  bool applied;
  if (target.stage_serial == 0)
  {
    // 1v1: stash the guest's cosmetic model pick (every submission overwrites it; absent or
    // invalid means no preference and the host's fallback wins) and rebuild the 1v1 block now, so
    // a model submitted any time before Start is already in the file SyncCodes reads. The server
    // rejects TeamData while a battle runs, so this cannot race a synced set.
    BattleCustomizer::SetGuestModel(submission.model);
    BattleCustomizer::RegenerateFromConfig(nullptr, BattleCustomizer::StartKind::OneVsOne);
    applied = submission.save_bundle ?
                  InjectGuestBundle(*submission.save_bundle, target.device, &status,
                                    submission.raise_to_level_100) :
                  InjectGuestTeam(submission.showdown_text, submission.trainer_name, target.device,
                                  &status, submission.raise_to_level_100);
  }
  else
  {
    // Multi: the player's own stage. Nothing is regenerated here: a Multi block is only ever
    // built by a Multi Start.
    applied = submission.save_bundle ?
                  InjectGuestBundleToStage(*submission.save_bundle, target.device,
                                           target.stage_serial, &status,
                                           submission.raise_to_level_100) :
                  InjectGuestTeamToStage(submission.showdown_text, submission.trainer_name,
                                         target.device, target.stage_serial, &status,
                                         submission.raise_to_level_100);
    if (applied)
      RecordMultiStage(target.stage_serial, submission.model);
  }

  NetPlay::XdTeamResult result;
  result.applied = applied;
  if (applied)
    result.line = status;
  else
    result.line = status.empty() ? std::string{"team not applied"} : "team not applied - " + status;
  return result;
}
}  // namespace XDNetplay
