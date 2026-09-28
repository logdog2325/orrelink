// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"

namespace NetPlay
{
class NetPlayServer;
struct XdStartClaim;
}  // namespace NetPlay

// XD multi battles (four seats): the host's Start and the host's TeamData handler, shared by both
// room UIs (DolphinQt NetPlayDialog, Android Netplay.cpp / NetPlayUICallbacks).
namespace XDNetplay
{
// What PrepareMultiStart decided. refusal: "" when the start may go ahead, else the one line the
// host sees. notes: private chat lines for players (sent with NetPlayServer::SendXdNote).
struct MultiStartOutcome
{
  std::string refusal;
  std::vector<std::pair<NetPlay::PlayerId, std::string>> notes;
};

// Host UI thread (Android: under s_host_write_mutex), inside the XdStartFreeze whose claim is
// `claim` (claim.multi), outside every other netplay lock. Checks everything a Multi start needs,
// writes the four boot copies, arms their scrub, forces the session settings, generates the
// Multi battle block and sets the pad map {host, Seat 2, Seat 3, Seat 4} with four GBAs. On a
// refusal after the boot copies were written it has scrubbed them again. The caller then says
// MultiStartChatLine() in the room chat and calls RequestStartGame; if that fails, AbortMultiStart.
MultiStartOutcome PrepareMultiStart(NetPlay::NetPlayServer& server,
                                    const NetPlay::XdStartClaim& claim);
// The Start line for the room chat (it names the team preview's B, which cancels for everyone
// unless the preview-cancel pin is on).
std::string MultiStartChatLine();
// RequestStartGame refused a prepared Multi start: scrub the boot copies now.
void AbortMultiStart();
// The server aborted a start that was syncing (a player left): if it was a Multi start and no game
// booted, its boot copies go now (the purge armed at Start only fires when a game stops). Any
// thread; takes no netplay lock.
void OnStartAborted();

// Host side, NETPLAY thread, under the server's seat mutex: a joiner's TeamData. 1v1
// (target.stage_serial == 0): into the GBA slot target.device, stashing the model and
// regenerating the 1v1 block, as before. Multi: into that player's stage, recording its model; no
// block is regenerated (a Multi block is only built at a Multi Start).
NetPlay::XdTeamResult HandleTeamSubmission(const std::string& player, const std::string& text,
                                           const NetPlay::XdTeamTarget& target);
}  // namespace XDNetplay
