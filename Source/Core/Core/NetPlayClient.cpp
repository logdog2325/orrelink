// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlayClient.h"

#ifdef __ANDROID__
#include <sys/resource.h>
#endif

#include <algorithm>
#include <chrono>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Assert.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/ENet.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/QoSSession.h"
#include "Common/SFMLHelper.h"
#include "Common/Timer.h"
#include "Common/Version.h"

#include "Core/ActionReplay.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/ConfigManager.h"
#include "Core/GeckoCode.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#include "Core/HW/GBADetectLog.h"
#endif
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SI/SI_DeviceAMBaseboard.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "Core/HW/Sram.h"
#include "Core/HW/WiiSave.h"
#include "Core/HW/WiiSaveStructs.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/Uids.h"
#include "Core/Movie.h"
#include "Core/NetPlayCommon.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/SyncIdentifier.h"
#include "Core/System.h"
#include "DiscIO/Blob.h"

#include "InputCommon/GCAdapter.h"
#include "UICommon/GameFile.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace NetPlay
{
using namespace WiimoteCommon;

static std::mutex crit_netplay_client;
static NetPlayClient* netplay_client = nullptr;
static bool s_si_poll_batching = false;

// XD netplay peer-vanish watchdog -- see WaitOnRemote() for the full story.
//
// Every number here is about the CONNECTION, never about emulated state. A
// timeout in this file can only ever end the session; it can never invent a pad
// frame, so none of it can desync anybody. They are also all deliberately
// enormous next to this fork's real traffic: our users play transatlantic at
// ~200 ms with buffer 10-14 (about 200 ms of cushion) and see 300 ms+ spikes,
// so the smallest bound below is still ~10x the worst spike we have on record.
//
// More important than any of the numbers: none of them ends a session on
// elapsed time ALONE. Every rule in WaitOnRemote() also needs corroboration
// that the session is genuinely over, because a peer that is merely paused --
// which Android does on every backgrounding -- can outlast any threshold
// anybody would be willing to pick, and must find the session still there when
// it comes back.
namespace
{
// One slice of a bounded wait. Also the granularity at which a wedged CPU
// thread notices a stop request, so keep it short; it costs 4 wakeups a second
// on a thread that would otherwise be asleep anyway.
constexpr std::chrono::milliseconds PAD_WAIT_SLICE = std::chrono::milliseconds(250);
// First "we are waiting on somebody" notice. Purely cosmetic -- no action is
// taken -- so it can afford to be near the edge of plausible jitter.
constexpr u64 PAD_STALL_NOTICE_MS = 3000;
// How often to repeat that notice so the window never looks simply dead.
constexpr u64 PAD_STALL_REPEAT_MS = 5000;
// ...and how often to repeat it once the stall has gone on for
// PAD_STALL_SLOW_NOTICE_AFTER_MS. A wait can now legitimately last as long as
// an opponent's phone call (see rule 3 in WaitOnRemote()), and a banner every
// five seconds for ten minutes stops being information.
constexpr u64 PAD_STALL_SLOW_NOTICE_AFTER_MS = 60000;
constexpr u64 PAD_STALL_REPEAT_SLOW_MS = 30000;
// Starved of remote frames AND not one netplay packet from the server in this
// long. The server pings every client at 1 Hz, so this is 20 consecutive missed
// pings: the peer we actually hold a socket to is gone, not slow. ENet's own
// verdict (PEER_TIMEOUT, 30 s) still arrives and is still authoritative -- this
// only stops us from being a frozen window for the last third of that wait.
constexpr u64 LINK_SILENT_MS = 20000;
// Backstop for "starved of a pad whose owner has already left the room".
// Deliberately parked past PEER_TIMEOUT so that in every topology we understand
// the server's own DisableGame broadcast wins the race and this never fires; it
// is here for the case where the server told us the player left but never told
// us to stop. Note what it is NOT: a plain "no pad for 45 s" timer. See rule 3
// in WaitOnRemote() for why an unconditional one is unsafe in this fork.
constexpr u64 PAD_OWNER_GONE_ABORT_MS = 45000;
// How long a locally requested stop may sit unanswered before we stop anyway.
// RequestStopGame() only mails a packet to the server; if the server IS the
// machine that vanished, that packet goes nowhere and upstream leaves the CPU
// thread waiting on remote pads for a game the user already quit.
constexpr u64 LOCAL_STOP_GRACE_MS = 2000;

u64 SteadyNowMs()
{
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}
}  // namespace

// called from ---GUI--- thread
NetPlayClient::~NetPlayClient()
{
  // not perfect
  if (m_is_running.IsSet())
    StopGame();

  if (m_is_connected)
  {
    m_should_compute_game_digest = false;
    m_dialog->AbortGameDigest();
    if (m_game_digest_thread.joinable())
      m_game_digest_thread.join();
    m_do_loop.Clear();
    m_thread.join();

    m_chunked_data_receive_queue.clear();
    m_dialog->HideChunkedProgressDialog();
  }

  if (m_server)
  {
    Disconnect();
  }

  if (Common::g_MainNetHost.get() == m_client)
  {
    Common::g_MainNetHost.release();
  }
  if (m_client)
  {
    enet_host_destroy(m_client);
    m_client = nullptr;
  }

  if (m_traversal_client)
  {
    Common::ReleaseTraversalClient();
  }

  // The session is over for THIS machine, whichever end of it we were.
  //
  // ~NetPlayServer makes the same call, but only a host has a server: without
  // this a joiner never cleaned up, and a joiner is the one left holding
  // NetPlayTemp2.sav -- a byte-for-byte copy of the HOST's party, EVs, IVs and
  // natures included. The call is idempotent, so a host simply gets it twice.
  //
  // Note this runs BEFORE the emulation shutdown that StopGame() above kicked
  // off has finished; OnRoomClosed defers the actual work until the GBA cores
  // have stopped and flushed. See XDNetplay::RestoreHostTeam.
  if (m_dialog)
    m_dialog->OnRoomClosed();
}

// called from ---GUI--- thread
NetPlayClient::NetPlayClient(const std::string& address, const u16 port, NetPlayUI* dialog,
                             std::string name, const NetTraversalConfig& traversal_config)
    : m_dialog(dialog), m_player_name(std::move(name))
{
  ClearBuffers();

  if (!traversal_config.use_traversal)
  {
    // Direct Connection
    m_client = enet_host_create(nullptr, 1, CHANNEL_COUNT, 0, 0);

    if (m_client == nullptr)
    {
      m_dialog->OnConnectionError(_trans("Could not create client."));
      return;
    }

    m_client->mtu = std::min(m_client->mtu, NetPlay::MAX_ENET_MTU);

    // Zero-init and CHECK the resolve. On failure enet leaves addr.host unwritten, so the old
    // code dialed stack garbage and spent the full 5-second connect timeout producing the same
    // "Could not communicate with host." a genuine network failure does -- a typo and a firewall
    // were indistinguishable. Fail fast and say what is actually wrong.
    ENetAddress addr{};
    if (enet_address_set_host(&addr, address.c_str()) != 0)
    {
      m_dialog->OnConnectionError(
          _trans("Not a usable address. Enter the host's IP like 192.168.1.5, or IP:port like "
                 "192.168.1.5:2626."));
      return;
    }
    addr.port = port;

    m_server = enet_host_connect(m_client, &addr, CHANNEL_COUNT, 0);

    if (m_server == nullptr)
    {
      m_dialog->OnConnectionError(_trans("Could not create peer."));
      return;
    }

    // Update time in milliseconds of no acknowledgment of
    // sent packets before a connection is deemed disconnected
    enet_peer_timeout(m_server, 0, PEER_TIMEOUT.count(), PEER_TIMEOUT.count());

    ENetEvent netEvent;
    int net = enet_host_service(m_client, &netEvent, 5000);
    if (net > 0 && netEvent.type == ENET_EVENT_TYPE_CONNECT)
    {
      if (Connect())
      {
        m_client->intercept = Common::ENet::InterceptCallback;
        m_thread = std::thread(&NetPlayClient::ThreadFunc, this);
      }
    }
    else
    {
      m_dialog->OnConnectionError(_trans("Could not communicate with host."));
    }
  }
  else
  {
    if (address.size() > Common::NETPLAY_CODE_SIZE)
    {
      m_dialog->OnConnectionError(
          _trans("The host code is too long.\nPlease recheck that you have the correct code."));
      return;
    }

    if (!Common::EnsureTraversalClient(traversal_config.traversal_host,
                                       traversal_config.traversal_port))
    {
      // Without this the join fails in perfect silence -- the spinner stops and nothing is said.
      m_dialog->OnConnectionError(
          _trans("Could not reach the traversal server. Check your connection and try again."));
      return;
    }
    m_client = Common::g_MainNetHost.get();

    m_traversal_client = Common::g_TraversalClient.get();

    // If we were disconnected in the background, reconnect.
    if (m_traversal_client->HasFailed())
      m_traversal_client->ReconnectToServer();
    m_traversal_client->m_Client = this;
    m_host_spec = address;
    m_connection_state = ConnectionState::WaitingForTraversalClientConnection;
    OnTraversalStateChanged();
    m_connecting = true;

    Common::Timer connect_timer;
    connect_timer.Start();

    while (m_connecting)
    {
      ENetEvent netEvent;
      if (m_traversal_client)
        m_traversal_client->HandleResends();

      while (enet_host_service(m_client, &netEvent, 4) > 0)
      {
        sf::Packet rpac;
        switch (netEvent.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
          m_server = netEvent.peer;

          // Update time in milliseconds of no acknowledgment of
          // sent packets before a connection is deemed disconnected
          enet_peer_timeout(m_server, 0, PEER_TIMEOUT.count(), PEER_TIMEOUT.count());

          if (Connect())
          {
            m_connection_state = ConnectionState::Connected;
            m_thread = std::thread(&NetPlayClient::ThreadFunc, this);
          }
          return;
        default:
          break;
        }
      }
      if (connect_timer.ElapsedMs() > 5000)
        break;
    }
    // Say which of the very different failures this actually was. The connection state is no use
    // here -- Disconnect() has already collapsed it to Failure -- but the traversal client still
    // knows why IT failed, and "the server never answered" vs "the server answered and the punch
    // died" point at completely different problems on the user's side.
    if (m_specific_connect_error)
      return;  // OnConnectFailed already told the user something better than the generic line.
    if (m_traversal_client && m_traversal_client->HasFailed())
    {
      switch (m_traversal_client->GetFailureReason())
      {
      case Common::TraversalClient::FailureReason::BadHost:
      case Common::TraversalClient::FailureReason::SocketSendError:
      case Common::TraversalClient::FailureReason::ResendTimeout:
      case Common::TraversalClient::FailureReason::ServerForgotAboutUs:
        m_dialog->OnConnectionError(
            _trans("Could not reach the traversal server at all. This network may be blocking the "
                   "connection -- see the README's section on networks that block peer-to-peer."));
        return;
      case Common::TraversalClient::FailureReason::VersionTooOld:
        m_dialog->OnConnectionError(_trans("The traversal server no longer supports this build."));
        return;
      default:
        break;
      }
    }
    m_dialog->OnConnectionError(
        _trans("Reached the traversal server, but no connection to the host could be made. This "
               "usually means a firewall or NAT blocked it on one side."));
  }
}

bool NetPlayClient::Connect()
{
  INFO_LOG_FMT(NETPLAY, "Connecting to server.");

  // send connect message
  sf::Packet packet;
  packet << Common::GetScmRevGitStr();
  // The Revision column in every room UI then reads e.g. "... Mac arm64" /
  // "... Win x86_64" with no UI change; the separate arch field below is what
  // the host's cross-architecture policy uses (NetPlayServer::SetupNetSettings).
  packet << Common::GetNetplayDolphinVer() + " " + LocalCpuArch();
  packet << m_player_name;
  packet << std::string(LocalCpuArch());
  Send(packet);
  enet_host_flush(m_client);
  sf::Packet rpac;
  // TODO: make this not hang
  ENetEvent netEvent;
  int net;
  while ((net = enet_host_service(m_client, &netEvent, 5000)) > 0 &&
         static_cast<int>(netEvent.type) == Common::ENet::SKIPPABLE_EVENT)
  {
    // ignore packets from traversal server
  }
  if (net > 0 && netEvent.type == ENET_EVENT_TYPE_RECEIVE)
  {
    rpac.append(netEvent.packet->data, netEvent.packet->dataLength);
    enet_packet_destroy(netEvent.packet);
  }
  else
  {
    return false;
  }

  ConnectionError error;
  rpac >> error;

  // got error message
  if (error != ConnectionError::NoError)
  {
    switch (error)
    {
    case ConnectionError::ServerFull:
      m_dialog->OnConnectionError(_trans("The server is full."));
      break;
    case ConnectionError::VersionMismatch:
      m_dialog->OnConnectionError(
          _trans("The server and client's NetPlay versions are incompatible."));
      break;
    case ConnectionError::GameRunning:
      m_dialog->OnConnectionError(_trans("The game is currently running."));
      break;
    case ConnectionError::NameTooLong:
      m_dialog->OnConnectionError(_trans("Nickname is too long."));
      break;
    default:
      m_dialog->OnConnectionError(_trans("The server sent an unknown error message."));
      break;
    }

    Disconnect();
    return false;
  }
  else
  {
    rpac >> m_pid;

    Player player;
    player.name = m_player_name;
    player.pid = m_pid;
    player.revision = Common::GetNetplayDolphinVer() + " " + LocalCpuArch();

    // add self to player list
    m_players[m_pid] = player;
    m_local_player = &m_players[m_pid];

    m_dialog->Update();

    m_is_connected = true;

    return true;
  }
}

static void ReceiveSyncIdentifier(sf::Packet& spac, SyncIdentifier& sync_identifier)
{
  // We use a temporary variable here due to a potential long vs long long mismatch
  u64 dol_elf_size;
  spac >> dol_elf_size;
  sync_identifier.dol_elf_size = dol_elf_size;

  spac >> sync_identifier.game_id;
  spac >> sync_identifier.revision;
  spac >> sync_identifier.disc_number;
  spac >> sync_identifier.is_datel;

  for (u8& x : sync_identifier.sync_hash)
    spac >> x;
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnData(sf::Packet& packet)
{
  MessageID mid;
  packet >> mid;

  INFO_LOG_FMT(NETPLAY, "Got server message: {:x}", static_cast<u8>(mid));

  switch (mid)
  {
  case MessageID::PlayerJoin:
    OnPlayerJoin(packet);
    break;

  case MessageID::PlayerLeave:
    OnPlayerLeave(packet);
    break;

  case MessageID::ChatMessage:
    OnChatMessage(packet);
    break;

  case MessageID::ChunkedDataStart:
    OnChunkedDataStart(packet);
    break;

  case MessageID::ChunkedDataEnd:
    OnChunkedDataEnd(packet);
    break;

  case MessageID::ChunkedDataPayload:
    OnChunkedDataPayload(packet);
    break;

  case MessageID::ChunkedDataAbort:
    OnChunkedDataAbort(packet);
    break;

  case MessageID::PadMapping:
    OnPadMapping(packet);
    break;

  case MessageID::GBAConfig:
    OnGBAConfig(packet);
    break;

  case MessageID::WiimoteMapping:
    OnWiimoteMapping(packet);
    break;

  case MessageID::PadData:
    OnPadData(packet);
    break;

  case MessageID::PadHostData:
    OnPadHostData(packet);
    break;

  case MessageID::WiimoteData:
    OnWiimoteData(packet);
    break;

  case MessageID::PadBuffer:
    OnPadBuffer(packet);
    break;

  case MessageID::HostInputAuthority:
    OnHostInputAuthority(packet);
    break;

  case MessageID::GolfSwitch:
    OnGolfSwitch(packet);
    break;

  case MessageID::GolfPrepare:
    OnGolfPrepare(packet);
    break;

  case MessageID::ChangeGame:
    OnChangeGame(packet);
    break;

  case MessageID::GameStatus:
    OnGameStatus(packet);
    break;

  case MessageID::StartGame:
    OnStartGame(packet);
    break;

  case MessageID::StopGame:
  case MessageID::DisableGame:
    OnStopGame(packet);
    break;

  case MessageID::PowerButton:
    OnPowerButton();
    break;

  case MessageID::Ping:
    OnPing(packet);
    break;

  case MessageID::PlayerPingData:
    OnPlayerPingData(packet);
    break;

  case MessageID::DesyncDetected:
    OnDesyncDetected(packet);
    break;

  case MessageID::SyncSaveData:
    OnSyncSaveData(packet);
    break;

  case MessageID::SyncCodes:
    OnSyncCodes(packet);
    break;

  case MessageID::ComputeGameDigest:
    OnComputeGameDigest(packet);
    break;

  case MessageID::GameDigestProgress:
    OnGameDigestProgress(packet);
    break;

  case MessageID::GameDigestResult:
    OnGameDigestResult(packet);
    break;

  case MessageID::GameDigestError:
    OnGameDigestError(packet);
    break;

  case MessageID::GameDigestAbort:
    OnGameDigestAbort();
    break;

  default:
    PanicAlertFmtT("Unknown message received with id : {0}", static_cast<u8>(mid));
    break;
  }
}

void NetPlayClient::OnPlayerJoin(sf::Packet& packet)
{
  Player player{};
  packet >> player.pid;
  packet >> player.name;
  packet >> player.revision;

  INFO_LOG_FMT(NETPLAY, "Player {} ({}) using {} joined", player.name, player.pid, player.revision);

  {
    std::lock_guard lkp(m_crit.players);
    m_players[player.pid] = player;
  }

  m_dialog->OnPlayerConnect(player.name);

  m_dialog->Update();
}

void NetPlayClient::OnPlayerLeave(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard lkp(m_crit.players);
    const auto it = m_players.find(pid);
    if (it == m_players.end())
      return;

    const auto& player = it->second;
    INFO_LOG_FMT(NETPLAY, "Player {} ({}) left", player.name, pid);
    m_dialog->OnPlayerDisconnect(player.name);
    m_players.erase(it);
  }

  m_dialog->Update();
}

void NetPlayClient::OnChatMessage(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;
  std::string msg;
  packet >> msg;

  // don't need lock to read in this thread
  const Player& player = m_players[pid];

  INFO_LOG_FMT(NETPLAY, "Player {} ({}) wrote: {}", player.name, player.pid, msg);

  // add to gui
  m_dialog->AppendChat(fmt::format("{}[{}]: {}", player.name, pid, msg));
}

void NetPlayClient::OnChunkedDataStart(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;
  std::string title;
  packet >> title;
  const u64 data_size = Common::PacketReadU64(packet);

  INFO_LOG_FMT(NETPLAY, "Starting data chunk {}.", cid);

  m_chunked_data_receive_queue.emplace(cid, sf::Packet{});

  std::vector<int> players;
  players.push_back(m_local_player->pid);
  m_dialog->ShowChunkedProgressDialog(title, data_size, players);
}

void NetPlayClient::OnChunkedDataEnd(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;

  const auto data_packet_iter = m_chunked_data_receive_queue.find(cid);
  if (data_packet_iter == m_chunked_data_receive_queue.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", cid);
    return;
  }

  INFO_LOG_FMT(NETPLAY, "Ending data chunk {}.", cid);

  auto& data_packet = data_packet_iter->second;
  OnData(data_packet);
  m_chunked_data_receive_queue.erase(data_packet_iter);
  m_dialog->HideChunkedProgressDialog();

  sf::Packet complete_packet;
  complete_packet << MessageID::ChunkedDataComplete;
  complete_packet << cid;
  Send(complete_packet, CHUNKED_DATA_CHANNEL);
}

void NetPlayClient::OnChunkedDataPayload(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;

  const auto data_packet_iter = m_chunked_data_receive_queue.find(cid);
  if (data_packet_iter == m_chunked_data_receive_queue.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", cid);
    return;
  }

  auto& data_packet = data_packet_iter->second;
  while (!packet.endOfPacket())
  {
    u8 byte;
    packet >> byte;
    data_packet << byte;
  }

  INFO_LOG_FMT(NETPLAY, "Received {} bytes of data chunk {}.", data_packet.getDataSize(), cid);

  m_dialog->SetChunkedProgress(m_local_player->pid, data_packet.getDataSize());

  sf::Packet progress_packet;
  progress_packet << MessageID::ChunkedDataProgress;
  progress_packet << cid;
  progress_packet << u64{data_packet.getDataSize()};
  Send(progress_packet, CHUNKED_DATA_CHANNEL);
}

void NetPlayClient::OnChunkedDataAbort(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;

  const auto iter = m_chunked_data_receive_queue.find(cid);
  if (iter == m_chunked_data_receive_queue.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", cid);
    return;
  }

  INFO_LOG_FMT(NETPLAY, "Aborting data chunk {}.", cid);

  m_chunked_data_receive_queue.erase(iter);
  m_dialog->HideChunkedProgressDialog();
}

void NetPlayClient::OnPadMapping(sf::Packet& packet)
{
  for (PlayerId& mapping : m_net_settings.pad_map)
    packet >> mapping;

  m_dialog->Update();
}

void NetPlayClient::OnWiimoteMapping(sf::Packet& packet)
{
  for (PlayerId& mapping : m_net_settings.wiimote_map)
    packet >> mapping;

  m_dialog->Update();
}

void NetPlayClient::OnGBAConfig(sf::Packet& packet)
{
  for (size_t i = 0; i < m_net_settings.gba_config.size(); ++i)
  {
    auto& config = m_net_settings.gba_config[i];
    const auto old_config = config;

    packet >> config.enabled >> config.has_rom >> config.title;
    for (auto& data : config.hash)
      packet >> data;

    if (std::tie(config.has_rom, config.title, config.hash) !=
        std::tie(old_config.has_rom, old_config.title, old_config.hash))
    {
      m_dialog->OnMsgChangeGBARom(static_cast<int>(i), config);
      m_net_settings.gba_rom_paths[i] =
          config.has_rom ?
              m_dialog->FindGBARomPath(config.hash, config.title, static_cast<int>(i)) :
              "";
    }
  }

  SendGameStatus();

  m_dialog->Update();
}

void NetPlayClient::OnPadData(sf::Packet& packet)
{
  while (!packet.endOfPacket())
  {
    PadIndex map;
    packet >> map;

    GCPadStatus pad;
    packet >> pad.button;
    if (static_cast<size_t>(map) < m_net_settings.gba_config.size() &&
        !m_net_settings.gba_config.at(map).enabled)
    {
      packet >> pad.analogA >> pad.analogB >> pad.stickX >> pad.stickY >> pad.substickX >>
          pad.substickY >> pad.triggerLeft >> pad.triggerRight >> pad.isConnected;
    }

    if (static_cast<size_t>(map) < m_pad_buffer.size())
    {
      m_pad_buffer.at(map).Push(pad);
      m_gc_pad_event.Set();
    }
  }
}

void NetPlayClient::OnPadHostData(sf::Packet& packet)
{
  while (!packet.endOfPacket())
  {
    PadIndex map;
    packet >> map;

    GCPadStatus pad;
    packet >> pad.button;
    if (static_cast<size_t>(map) < m_net_settings.gba_config.size() &&
        !m_net_settings.gba_config.at(map).enabled)
    {
      packet >> pad.analogA >> pad.analogB >> pad.stickX >> pad.stickY >> pad.substickX >>
          pad.substickY >> pad.triggerLeft >> pad.triggerRight >> pad.isConnected;
    }

    if (static_cast<size_t>(map) < m_last_pad_status.size())
      m_last_pad_status[map] = pad;

    if (static_cast<size_t>(map) < m_first_pad_status_received.size())
    {
      if (!m_first_pad_status_received[map])
      {
        m_first_pad_status_received[map] = true;
        m_first_pad_status_received_event.Set();
      }
    }
  }
}

void NetPlayClient::OnWiimoteData(sf::Packet& packet)
{
  while (!packet.endOfPacket())
  {
    PadIndex map;
    packet >> map;

    WiimoteEmu::SerializedWiimoteState pad;
    packet >> pad.length;
    ASSERT(pad.length <= pad.data.size());
    if (pad.length <= pad.data.size())
    {
      for (size_t i = 0; i < pad.length; ++i)
        packet >> pad.data[i];
    }
    else
    {
      pad.length = 0;
    }

    if (static_cast<size_t>(map) < m_wiimote_buffer.size())
    {
      m_wiimote_buffer.at(map).Push(pad);
      m_wii_pad_event.Set();
    }
  }
}

void NetPlayClient::OnPadBuffer(sf::Packet& packet)
{
  u32 size = 0;
  packet >> size;

  m_target_buffer_size = size;
  m_dialog->OnPadBufferChanged(size);
}

void NetPlayClient::OnHostInputAuthority(sf::Packet& packet)
{
  packet >> m_host_input_authority;
  m_dialog->OnHostInputAuthorityChanged(m_host_input_authority);
}

void NetPlayClient::OnGolfSwitch(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  const PlayerId previous_golfer = m_current_golfer;
  m_current_golfer = pid;
  m_dialog->OnGolferChanged(m_local_player->pid == pid, pid != 0 ? m_players[pid].name : "");

  if (m_local_player->pid == previous_golfer)
  {
    sf::Packet spac;
    spac << MessageID::GolfRelease;
    Send(spac);
  }
  else if (m_local_player->pid == pid)
  {
    sf::Packet spac;
    spac << MessageID::GolfAcquire;
    Send(spac);

    // Pads are already calibrated so we can just ignore this
    m_first_pad_status_received.fill(true);

    m_wait_on_input = false;
    m_wait_on_input_event.Set();
  }
}

void NetPlayClient::OnGolfPrepare(sf::Packet& packet)
{
  m_wait_on_input_received = true;
  m_wait_on_input = true;
}

void NetPlayClient::OnChangeGame(sf::Packet& packet)
{
  std::string netplay_name;
  {
    std::lock_guard lkg(m_crit.game);
    ReceiveSyncIdentifier(packet, m_selected_game);
    packet >> netplay_name;
  }

  INFO_LOG_FMT(NETPLAY, "Game changed to {}", netplay_name);

  // update gui
  m_dialog->OnMsgChangeGame(m_selected_game, netplay_name);

  SendGameStatus();

  sf::Packet client_capabilities_packet;
  client_capabilities_packet << MessageID::ClientCapabilities;
  client_capabilities_packet << ExpansionInterface::CEXIIPL::HasIPLDump();
  client_capabilities_packet << Config::Get(Config::SESSION_USE_FMA);
  Send(client_capabilities_packet);
}

void NetPlayClient::OnGameStatus(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard lkp(m_crit.players);
    packet >> m_players[pid].game_status;
  }

  m_dialog->Update();
}

void NetPlayClient::OnStartGame(sf::Packet& packet)
{
  {
    std::lock_guard lkg(m_crit.game);

    INFO_LOG_FMT(NETPLAY, "Start of game {}", m_selected_game.game_id);

    packet >> m_current_game;
    packet >> m_net_settings.cpu_thread;
    packet >> m_net_settings.cpu_core;
    // Never let PowerPC.cpp's silent fallback pick this machine's native JIT when
    // the host named a core this build cannot construct. Unreachable under the
    // host's mixed-arch policy; if it ever fires, use the one core every platform
    // has and say so, so a JIT-vs-JIT mismatch can never hide again.
    if (std::ranges::find(PowerPC::AvailableCPUCores(), m_net_settings.cpu_core) ==
        PowerPC::AvailableCPUCores().end())
    {
      m_dialog->AppendChat(fmt::format(
          "Host asked for CPU core {} which this build cannot run; using the Cached Interpreter.",
          static_cast<int>(m_net_settings.cpu_core)));
      m_net_settings.cpu_core = PowerPC::CPUCore::CachedInterpreter;
    }
    packet >> m_net_settings.enable_cheats;
    packet >> m_net_settings.enable_hardcore;
    packet >> m_net_settings.selected_language;
    packet >> m_net_settings.override_region_settings;
    packet >> m_net_settings.dsp_enable_jit;
    packet >> m_net_settings.dsp_hle;
    packet >> m_net_settings.ram_override_enable;
    packet >> m_net_settings.mem1_size;
    packet >> m_net_settings.mem2_size;
    packet >> m_net_settings.fallback_region;
    packet >> m_net_settings.allow_sd_writes;
    packet >> m_net_settings.oc_enable;
    packet >> m_net_settings.oc_factor;
    packet >> m_net_settings.vi_oc_enable;
    packet >> m_net_settings.vi_oc_factor;

    for (auto slot : ExpansionInterface::SLOTS)
      packet >> m_net_settings.exi_device[slot];

    packet >> m_net_settings.memcard_size_override;

    for (u32& value : m_net_settings.sysconf_settings)
      packet >> value;

    packet >> m_net_settings.efb_access_enable;
    packet >> m_net_settings.bbox_enable;
    packet >> m_net_settings.force_progressive;
    packet >> m_net_settings.efb_to_texture_enable;
    packet >> m_net_settings.xfb_to_texture_enable;
    packet >> m_net_settings.disable_copy_to_vram;
    packet >> m_net_settings.immediate_xfb_enable;
    packet >> m_net_settings.efb_emulate_format_changes;
    packet >> m_net_settings.safe_texture_cache_color_samples;
    packet >> m_net_settings.perf_queries_enable;
    packet >> m_net_settings.float_exceptions;
    packet >> m_net_settings.divide_by_zero_exceptions;
    packet >> m_net_settings.fprf;
    packet >> m_net_settings.accurate_nans;
    packet >> m_net_settings.accurate_fmadds;
    packet >> m_net_settings.disable_icache;
    packet >> m_net_settings.sync_on_skip_idle;
    packet >> m_net_settings.sync_gpu;
    packet >> m_net_settings.sync_gpu_max_distance;
    packet >> m_net_settings.sync_gpu_min_distance;
    packet >> m_net_settings.sync_gpu_overclock;
    packet >> m_net_settings.jit_follow_branch;
    packet >> m_net_settings.fast_disc_speed;
    packet >> m_net_settings.mmu;
    packet >> m_net_settings.fastmem;
    packet >> m_net_settings.skip_ipl;
    packet >> m_net_settings.load_ipl_dump;
    packet >> m_net_settings.vertex_rounding;
    packet >> m_net_settings.internal_resolution;
    packet >> m_net_settings.efb_scaled_copy;
    packet >> m_net_settings.fast_depth_calc;
    packet >> m_net_settings.enable_pixel_lighting;
    packet >> m_net_settings.widescreen_hack;
    packet >> m_net_settings.force_texture_filtering;
    packet >> m_net_settings.max_anisotropy;
    packet >> m_net_settings.force_true_color;
    packet >> m_net_settings.disable_copy_filter;
    packet >> m_net_settings.disable_fog;
    packet >> m_net_settings.arbitrary_mipmap_detection;
    packet >> m_net_settings.arbitrary_mipmap_detection_threshold;
    packet >> m_net_settings.enable_gpu_texture_decoding;
    packet >> m_net_settings.defer_efb_copies;
    packet >> m_net_settings.efb_access_tile_size;
    packet >> m_net_settings.efb_access_defer_invalidation;
    packet >> m_net_settings.savedata_load;
    packet >> m_net_settings.savedata_write;
    packet >> m_net_settings.savedata_sync_all_wii;
    if (!m_net_settings.savedata_load)
    {
      m_net_settings.savedata_write = false;
      m_net_settings.savedata_sync_all_wii = false;
    }
    packet >> m_net_settings.strict_settings_sync;

    m_initial_rtc = Common::PacketReadU64(packet);

    packet >> m_net_settings.save_data_region;
    packet >> m_net_settings.sync_codes;

    packet >> m_net_settings.golf_mode;
    packet >> m_net_settings.use_fma;
    packet >> m_net_settings.hide_remote_gbas;

    for (size_t i = 0; i < sizeof(m_net_settings.sram); ++i)
      packet >> m_net_settings.sram[i];

    m_net_settings.is_hosting = m_local_player->IsHost();
  }

  m_dialog->OnMsgStartGame();
}

void NetPlayClient::OnStopGame(sf::Packet& packet)
{
  INFO_LOG_FMT(NETPLAY, "Game stopped");

  StopGame();
  m_dialog->OnMsgStopGame();
}

void NetPlayClient::OnPowerButton()
{
  InvokeStop();
  m_dialog->OnMsgPowerButton();
}

void NetPlayClient::OnPing(sf::Packet& packet)
{
  u32 ping_key = 0;
  packet >> ping_key;

  sf::Packet response_packet;
  response_packet << MessageID::Pong;
  response_packet << ping_key;

  Send(response_packet);
}

void NetPlayClient::OnPlayerPingData(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard lkp(m_crit.players);
    Player& player = m_players[pid];
    packet >> player.ping;
  }

  DisplayPlayersPing();
  m_dialog->Update();
}

void NetPlayClient::OnDesyncDetected(sf::Packet& packet)
{
  int pid_to_blame;
  u32 frame;
  packet >> pid_to_blame;
  packet >> frame;

  std::string player = "??";
  std::lock_guard lkp(m_crit.players);
  {
    const auto it = m_players.find(pid_to_blame);
    if (it != m_players.end())
      player = it->second.name;
  }

  INFO_LOG_FMT(NETPLAY, "Player {} ({}) desynced!", player, pid_to_blame);

  m_dialog->OnDesync(frame, player);
}

void NetPlayClient::OnSyncSaveData(sf::Packet& packet)
{
  SyncSaveDataID sub_id;
  packet >> sub_id;

  if (m_local_player->IsHost())
    return;

  INFO_LOG_FMT(NETPLAY, "Processing OnSyncSaveData sub id: {}", static_cast<u8>(sub_id));

  switch (sub_id)
  {
  case SyncSaveDataID::Notify:
    OnSyncSaveDataNotify(packet);
    break;

  case SyncSaveDataID::RawData:
    OnSyncSaveDataRaw(packet);
    break;

  case SyncSaveDataID::GCIData:
    OnSyncSaveDataGCI(packet);
    break;

  case SyncSaveDataID::WiiData:
    OnSyncSaveDataWii(packet);
    break;

  case SyncSaveDataID::GBAData:
    OnSyncSaveDataGBA(packet);
    break;

  default:
    PanicAlertFmtT("Unknown SYNC_SAVE_DATA message received with id: {0}", static_cast<u8>(sub_id));
    break;
  }
}

void NetPlayClient::OnSyncSaveDataNotify(sf::Packet& packet)
{
  packet >> m_sync_save_data_count;
  m_sync_save_data_success_count = 0;

  INFO_LOG_FMT(NETPLAY, "Initializing wait for {} savegame chunks.", m_sync_save_data_count);

  if (m_sync_save_data_count == 0)
    SyncSaveDataResponse(true);
  else
    m_dialog->AppendChat(Common::GetStringT("Synchronizing save data..."));
}

void NetPlayClient::OnSyncSaveDataRaw(sf::Packet& packet)
{
  bool is_slot_a;
  std::string region;
  int size_override;
  packet >> is_slot_a >> region >> size_override;

  INFO_LOG_FMT(NETPLAY, "Received raw memcard data for slot {}: region {}, size override {}.",
               is_slot_a ? 'A' : 'B', region, size_override);

  // This check is mainly intended to filter out characters which have special meanings in paths
  if (region != JAP_DIR && region != USA_DIR && region != EUR_DIR)
  {
    WARN_LOG_FMT(NETPLAY, "Received invalid raw memory card region.");
    SyncSaveDataResponse(false);
    return;
  }

  std::string size_suffix;
  if (size_override >= 0 && size_override <= 4)
  {
    size_suffix = fmt::format(
        ".{}", Memcard::MbitToFreeBlocks(Memcard::MBIT_SIZE_MEMORY_CARD_59 << size_override));
  }

  const std::string path = File::GetUserPath(D_GCUSER_IDX) + GC_MEMCARD_NETPLAY +
                           (is_slot_a ? "A." : "B.") + region + size_suffix + ".raw";
  if (File::Exists(path) && !File::Delete(path))
  {
    PanicAlertFmtT("Failed to delete NetPlay memory card. Verify your write permissions.");
    SyncSaveDataResponse(false);
    return;
  }

  const bool success = DecompressPacketIntoFile(packet, path);
  SyncSaveDataResponse(success);
}

void NetPlayClient::OnSyncSaveDataGCI(sf::Packet& packet)
{
  bool is_slot_a;
  u8 file_count;
  packet >> is_slot_a >> file_count;

  const std::string path = File::GetUserPath(D_GCUSER_IDX) + GC_MEMCARD_NETPLAY DIR_SEP +
                           fmt::format("Card {}", is_slot_a ? 'A' : 'B');

  INFO_LOG_FMT(NETPLAY, "Received GCI memcard data for slot {}: {}, {} files.",
               is_slot_a ? 'A' : 'B', path, file_count);

  if ((File::Exists(path) && !File::DeleteDirRecursively(path + DIR_SEP)) ||
      !File::CreateFullPath(path + DIR_SEP))
  {
    PanicAlertFmtT("Failed to reset NetPlay GCI folder. Verify your write permissions.");
    SyncSaveDataResponse(false);
    return;
  }

  for (u8 i = 0; i < file_count; i++)
  {
    std::string file_name;
    packet >> file_name;

    INFO_LOG_FMT(NETPLAY, "Received GCI: {}", file_name);

    if (!Common::IsFileNameSafe(file_name) ||
        !DecompressPacketIntoFile(packet, path + DIR_SEP + file_name))
    {
      WARN_LOG_FMT(NETPLAY, "Received invalid GCI.");
      SyncSaveDataResponse(false);
      return;
    }
  }

  SyncSaveDataResponse(true);
}

void NetPlayClient::OnSyncSaveDataWii(sf::Packet& packet)
{
  const std::string path = File::GetUserPath(D_USER_IDX) + "Wii" GC_MEMCARD_NETPLAY DIR_SEP;
  std::string redirect_path = File::GetUserPath(D_USER_IDX) + "Redirect" GC_MEMCARD_NETPLAY DIR_SEP;

  if (File::Exists(path) && !File::DeleteDirRecursively(path))
  {
    PanicAlertFmtT("Failed to reset NetPlay NAND folder. Verify your write permissions.");
    SyncSaveDataResponse(false);
    return;
  }
  if (File::Exists(redirect_path) && !File::DeleteDirRecursively(redirect_path))
  {
    PanicAlertFmtT("Failed to reset NetPlay redirect folder. Verify your write permissions.");
    SyncSaveDataResponse(false);
    return;
  }

  auto temp_fs = std::make_unique<IOS::HLE::FS::HostFileSystem>(path);
  std::vector<u64> titles;

  constexpr IOS::HLE::FS::Modes fs_modes{
      IOS::HLE::FS::Mode::ReadWrite,
      IOS::HLE::FS::Mode::ReadWrite,
      IOS::HLE::FS::Mode::ReadWrite,
  };

  // Read the Mii data
  bool mii_data;
  packet >> mii_data;
  if (mii_data)
  {
    INFO_LOG_FMT(NETPLAY, "Received Mii data.");

    auto buffer = DecompressPacketIntoBuffer(packet);

    temp_fs->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL, "/shared2/menu/FaceLib/", 0,
                            fs_modes);
    auto file = temp_fs->CreateAndOpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL,
                                           Common::GetMiiDatabasePath(), fs_modes);

    if (!buffer || !file || !file->Write(buffer->data(), buffer->size()))
    {
      PanicAlertFmtT("Failed to write Mii data.");
      SyncSaveDataResponse(false);
      return;
    }
  }

  // Read the saves
  u32 save_count;
  packet >> save_count;
  INFO_LOG_FMT(NETPLAY, "Received data for {} Wii saves.", save_count);
  for (u32 n = 0; n < save_count; n++)
  {
    u64 title_id = Common::PacketReadU64(packet);
    titles.push_back(title_id);
    temp_fs->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL,
                            Common::GetTitleDataPath(title_id) + '/', 0, fs_modes);
    auto save = WiiSave::MakeNandStorage(temp_fs.get(), title_id);

    bool exists;
    packet >> exists;
    if (!exists)
    {
      INFO_LOG_FMT(NETPLAY, "No data for Wii save of title {:016x}.", title_id);
      continue;
    }

    INFO_LOG_FMT(NETPLAY, "Received Wii save of title {:016x}.", title_id);

    // Header
    WiiSave::Header header;
    packet >> header.tid;
    packet >> header.banner_size;
    packet >> header.permissions;
    packet >> header.unk1;
    for (u8& byte : header.md5)
      packet >> byte;
    packet >> header.unk2;
    for (size_t i = 0; i < std::min<size_t>(header.banner_size, sizeof(header.banner)); i++)
      packet >> header.banner[i];

    // BkHeader
    WiiSave::BkHeader bk_header;
    packet >> bk_header.size;
    packet >> bk_header.magic;
    packet >> bk_header.ngid;
    packet >> bk_header.number_of_files;
    packet >> bk_header.size_of_files;
    packet >> bk_header.unk1;
    packet >> bk_header.unk2;
    packet >> bk_header.total_size;
    for (u8& byte : bk_header.unk3)
      packet >> byte;
    packet >> bk_header.tid;
    for (u8& byte : bk_header.mac_address)
      packet >> byte;

    // Files
    std::vector<WiiSave::Storage::SaveFile> files;
    for (u32 i = 0; i < bk_header.number_of_files; i++)
    {
      WiiSave::Storage::SaveFile file;
      packet >> file.mode >> file.attributes;
      packet >> file.type;
      packet >> file.path;

      INFO_LOG_FMT(NETPLAY, "Received Wii save data of type {} at {}", static_cast<u8>(file.type),
                   file.path);

      if (file.type == WiiSave::Storage::SaveFile::Type::File)
      {
        auto buffer = DecompressPacketIntoBuffer(packet);
        if (!buffer)
        {
          SyncSaveDataResponse(false);
          return;
        }

        file.data = std::move(*buffer);
      }

      files.push_back(std::move(file));
    }

    if (!save->WriteHeader(header) || !save->WriteBkHeader(bk_header) || !save->WriteFiles(files))
    {
      PanicAlertFmtT("Failed to write Wii save.");
      SyncSaveDataResponse(false);
      return;
    }
  }

  bool has_redirected_save;
  packet >> has_redirected_save;
  if (has_redirected_save)
  {
    INFO_LOG_FMT(NETPLAY, "Received redirected save.");
    if (!DecompressPacketIntoFolder(packet, redirect_path))
    {
      PanicAlertFmtT("Failed to write redirected save.");
      SyncSaveDataResponse(false);
      return;
    }
  }

  SetWiiSyncData(std::move(temp_fs), std::move(titles), std::move(redirect_path));
  SyncSaveDataResponse(true);
}

void NetPlayClient::OnSyncSaveDataGBA(sf::Packet& packet)
{
  u8 slot;
  packet >> slot;

  INFO_LOG_FMT(NETPLAY, "Received GBA save for slot {}.", slot);

  const std::string path =
      fmt::format("{}{}{}.sav", File::GetUserPath(D_GBAUSER_IDX), GBA_SAVE_NETPLAY, slot + 1);
  if (File::Exists(path) && !File::Delete(path))
  {
    PanicAlertFmtT("Failed to delete NetPlay GBA{0} save file. Verify your write permissions.",
                   slot + 1);
    SyncSaveDataResponse(false);
    return;
  }

  const bool success = DecompressPacketIntoFile(packet, path);
  SyncSaveDataResponse(success);
}

void NetPlayClient::OnSyncCodes(sf::Packet& packet)
{
  // Receive Data Packet
  SyncCodeID sub_id;
  packet >> sub_id;

  INFO_LOG_FMT(NETPLAY, "Processing OnSyncCodes sub id: {}", static_cast<u8>(sub_id));

  // Check Which Operation to Perform with This Packet
  switch (sub_id)
  {
  case SyncCodeID::Notify:
    OnSyncCodesNotify();
    break;

  case SyncCodeID::NotifyGecko:
    OnSyncCodesNotifyGecko(packet);
    break;

  case SyncCodeID::GeckoData:
    OnSyncCodesDataGecko(packet);
    break;

  case SyncCodeID::NotifyAR:
    OnSyncCodesNotifyAR(packet);
    break;

  case SyncCodeID::ARData:
    OnSyncCodesDataAR(packet);
    break;

  default:
    PanicAlertFmtT("Unknown SYNC_CODES message received with id: {0}", static_cast<u8>(sub_id));
    break;
  }
}

void NetPlayClient::OnSyncCodesNotify()
{
  // Set both codes as unsynced
  m_sync_gecko_codes_complete = false;
  m_sync_ar_codes_complete = false;
}

void NetPlayClient::OnSyncCodesNotifyGecko(sf::Packet& packet)
{
  // Return if this is the host
  if (m_local_player->IsHost())
    return;

  // Receive Number of Codelines to Receive
  packet >> m_sync_gecko_codes_count;

  m_sync_gecko_codes_success_count = 0;

  INFO_LOG_FMT(NETPLAY, "Receiving {} Gecko codelines", m_sync_gecko_codes_count);

  // Check if no codes to sync, if so return as finished
  if (m_sync_gecko_codes_count == 0)
  {
    m_sync_gecko_codes_complete = true;
    SyncCodeResponse(true);
  }
  else
  {
    m_dialog->AppendChat(Common::GetStringT("Synchronizing Gecko codes..."));
  }
}

void NetPlayClient::OnSyncCodesDataGecko(sf::Packet& packet)
{
  // Return if this is the host
  if (m_local_player->IsHost())
    return;

  std::vector<Gecko::GeckoCode> synced_codes;
  synced_codes.reserve(m_sync_gecko_codes_count);

  Gecko::GeckoCode gcode{};
  gcode.name = "Synced Codes";
  gcode.enabled = true;

  // Receive code contents from packet
  for (u32 i = 0; i < m_sync_gecko_codes_count; i++)
  {
    Gecko::GeckoCode::Code new_code;
    packet >> new_code.address;
    packet >> new_code.data;

    INFO_LOG_FMT(NETPLAY, "Received {:08x} {:08x}", new_code.address, new_code.data);

    gcode.codes.push_back(std::move(new_code));

    if (++m_sync_gecko_codes_success_count >= m_sync_gecko_codes_count)
    {
      m_sync_gecko_codes_complete = true;
      SyncCodeResponse(true);
    }
  }

  // Add gcode containing all codes to Gecko Code vector
  synced_codes.push_back(std::move(gcode));

  // Clear Vector if received 0 codes (match host's end when using no codes)
  if (m_sync_gecko_codes_count == 0)
    synced_codes.clear();

  // Copy this to the vector located in GeckoCode.cpp
  Gecko::UpdateSyncedCodes(synced_codes);
}

void NetPlayClient::OnSyncCodesNotifyAR(sf::Packet& packet)
{
  // Return if this is the host
  if (m_local_player->IsHost())
    return;

  // Receive Number of Codelines to Receive
  packet >> m_sync_ar_codes_count;

  m_sync_ar_codes_success_count = 0;

  INFO_LOG_FMT(NETPLAY, "Receiving {} AR codelines", m_sync_ar_codes_count);

  // Check if no codes to sync, if so return as finished
  if (m_sync_ar_codes_count == 0)
  {
    m_sync_ar_codes_complete = true;
    SyncCodeResponse(true);
  }
  else
  {
    m_dialog->AppendChat(Common::GetStringT("Synchronizing AR codes..."));
  }
}

void NetPlayClient::OnSyncCodesDataAR(sf::Packet& packet)
{
  // Return if this is the host
  if (m_local_player->IsHost())
    return;

  std::vector<ActionReplay::ARCode> synced_codes;
  synced_codes.reserve(m_sync_ar_codes_count);

  ActionReplay::ARCode arcode{};
  arcode.name = "Synced Codes";
  arcode.enabled = true;

  // Receive code contents from packet
  for (u32 i = 0; i < m_sync_ar_codes_count; i++)
  {
    ActionReplay::AREntry new_code;
    packet >> new_code.cmd_addr;
    packet >> new_code.value;

    INFO_LOG_FMT(NETPLAY, "Received {:08x} {:08x}", new_code.cmd_addr, new_code.value);
    arcode.ops.push_back(new_code);

    if (++m_sync_ar_codes_success_count >= m_sync_ar_codes_count)
    {
      m_sync_ar_codes_complete = true;
      SyncCodeResponse(true);
    }
  }

#ifdef HAS_LIBMGBA
  // OrreLink: what this guest received, so the host's 'ar synced-send' and this
  // 'ar synced-recv' can be compared crc-for-crc in the two gba_detect logs.
  {
    std::string blob;
    for (const ActionReplay::AREntry& op : arcode.ops)
      blob += fmt::format("{:08X} {:08X}\n", op.cmd_addr, op.value);
    GBADetectLog::NoteBoot(fmt::format("ar synced-recv lines={} crc32={:08x}", arcode.ops.size(),
                                       Common::ComputeCRC32(blob)));
  }
#endif
  // Add arcode containing all codes to AR Code vector
  synced_codes.push_back(std::move(arcode));

  // Clear Vector if received 0 codes (match host's end when using no codes)
  if (m_sync_ar_codes_count == 0)
    synced_codes.clear();

  // Copy this to the vector located in ActionReplay.cpp
  ActionReplay::UpdateSyncedCodes(synced_codes);
}

void NetPlayClient::OnComputeGameDigest(sf::Packet& packet)
{
  SyncIdentifier sync_identifier;
  ReceiveSyncIdentifier(packet, sync_identifier);

  ComputeGameDigest(sync_identifier);
}

void NetPlayClient::OnGameDigestProgress(sf::Packet& packet)
{
  PlayerId pid;
  int progress;
  packet >> pid;
  packet >> progress;

  m_dialog->SetGameDigestProgress(pid, progress);
}

void NetPlayClient::OnGameDigestResult(sf::Packet& packet)
{
  PlayerId pid;
  std::string result;
  packet >> pid;
  packet >> result;

  m_dialog->SetGameDigestResult(pid, result);
}

void NetPlayClient::OnGameDigestError(sf::Packet& packet)
{
  PlayerId pid;
  std::string error;
  packet >> pid;
  packet >> error;

  m_dialog->SetGameDigestResult(pid, error);
}

void NetPlayClient::OnGameDigestAbort()
{
  m_should_compute_game_digest = false;
  m_dialog->AbortGameDigest();
}

void NetPlayClient::Send(const sf::Packet& packet, const u8 channel_id)
{
  Common::ENet::SendPacket(m_server, packet, channel_id);
}

void NetPlayClient::DisplayPlayersPing()
{
  if (!Config::Get(Config::GFX_SHOW_NETPLAY_PING))
    return;

  OSD::AddTypedMessage(OSD::MessageType::NetPlayPing, fmt::format("Ping: {}", GetPlayersMaxPing()),
                       OSD::Duration::SHORT, OSD::Color::CYAN);
}

u32 NetPlayClient::GetPlayersMaxPing() const
{
  return std::ranges::max_element(m_players, {}, [](const auto& kv) { return kv.second.ping; })
      ->second.ping;
}

void NetPlayClient::Disconnect()
{
  ENetEvent netEvent;
  m_connecting = false;
  m_connection_state = ConnectionState::Failure;
  if (m_server)
    enet_peer_disconnect(m_server, 0);
  else
    return;

  while (enet_host_service(m_client, &netEvent, 3000) > 0)
  {
    switch (netEvent.type)
    {
    case ENET_EVENT_TYPE_RECEIVE:
      enet_packet_destroy(netEvent.packet);
      break;
    case ENET_EVENT_TYPE_DISCONNECT:
      m_server = nullptr;
      return;
    default:
      break;
    }
  }
  // didn't disconnect gracefully force disconnect
  enet_peer_reset(m_server);
  m_server = nullptr;
}

void NetPlayClient::SendAsync(sf::Packet&& packet, const u8 channel_id)
{
  {
    std::lock_guard lkq(m_crit.async_queue_write);
    m_async_queue.Push(AsyncQueueEntry{std::move(packet), channel_id});
  }
  Common::ENet::WakeupThread(m_client);
}

// called from ---NETPLAY--- thread
void NetPlayClient::ThreadFunc()
{
  INFO_LOG_FMT(NETPLAY, "NetPlayClient starting.");
#ifdef __ANDROID__
  // Keep this thread responsive under Android scheduling (URGENT_DISPLAY class).
  setpriority(PRIO_PROCESS, 0, -8);
#endif


  Common::QoSSession qos_session;
  if (Config::Get(Config::NETPLAY_ENABLE_QOS))
  {
    qos_session = Common::QoSSession(m_server);

    if (qos_session.Successful())
    {
      m_dialog->AppendChat(
          Common::GetStringT("Quality of Service (QoS) was successfully enabled."));
    }
    else
    {
      m_dialog->AppendChat(Common::GetStringT("Quality of Service (QoS) couldn't be enabled."));
    }
  }

  // XD netplay: set once we have run the teardown DeclareSessionLost() asked
  // for. Thread-local to this loop, so it needs no synchronisation.
  bool session_lost_handled = false;

  while (m_do_loop.IsSet())
  {
    // XD netplay: the CPU thread concluded the session is unrecoverable and
    // freed itself, but it cannot run StopGame() from inside GetNetPads (it
    // holds crit_netplay_client there, and StopGame reaches NetPlay_Disable
    // which wants the same non-recursive mutex). Finish the job here, on the
    // thread that is allowed to.
    const SessionEndKind session_end = m_session_end.load(std::memory_order_acquire);
    if (session_end == SessionEndKind::None)
    {
      // Re-arm. This thread outlives individual games -- a room can start a
      // second battle after the first one was ended by a drop -- and StartGame()
      // clears m_session_end, so the latch has to come back with it or the next
      // drop in the same room would go unhandled.
      session_lost_handled = false;
    }
    else if (!session_lost_handled)
    {
      session_lost_handled = true;
      // Only announce a drop when there actually was one. The other kind is the
      // local user's own Stop completing without the server's acknowledgement,
      // which needs the teardown below just the same but must not tell the room
      // that somebody disconnected.
      if (session_end == SessionEndKind::PeerLost)
      {
        m_dialog->AppendChat(
            Common::GetStringT("NetPlay session ended: a player disconnected unexpectedly."));
      }
      // Deliberately NOT m_dialog->OnConnectionLost(): that one means "our own
      // socket to the server died", which is only one of the ways we get here,
      // and on Android it drives a modal. When it IS true, ENet raises its own
      // disconnect event below and calls it for us. StopGame() is what puts
      // every platform into a defined, unfrozen state.
      StopGame();
    }

    ENetEvent netEvent;
    int net;
    if (m_traversal_client)
      m_traversal_client->HandleResends();
    net = enet_host_service(m_client, &netEvent, 250);
    while (!m_async_queue.Empty())
    {
      INFO_LOG_FMT(NETPLAY, "Processing async queue event.");
      {
        auto& e = m_async_queue.Front();
        Send(e.packet, e.channel_id);
      }
      INFO_LOG_FMT(NETPLAY, "Processing async queue event done.");
      m_async_queue.Pop();
    }
    if (net > 0)
    {
      sf::Packet rpac;
      switch (netEvent.type)
      {
      case ENET_EVENT_TYPE_CONNECT:
        INFO_LOG_FMT(NETPLAY, "enet_host_service: connect event");
        break;
      case ENET_EVENT_TYPE_RECEIVE:
        INFO_LOG_FMT(NETPLAY, "enet_host_service: receive event");

        // Proof of life for the pad-wait watchdog. Any netplay packet counts,
        // not just pad data -- the point is "the machine we hold a socket to is
        // still talking", and the server's 1 Hz Ping guarantees this keeps
        // ticking even when the GAME is fully starved. See WaitOnRemote().
        m_last_recv_ms.store(SteadyNowMs(), std::memory_order_relaxed);

        rpac.append(netEvent.packet->data, netEvent.packet->dataLength);
        OnData(rpac);

        enet_packet_destroy(netEvent.packet);
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        INFO_LOG_FMT(NETPLAY, "enet_host_service: disconnect event");

        m_dialog->OnConnectionLost();

        if (m_is_running.IsSet())
          StopGame();

        break;
      default:
        // not a valid switch case due to not technically being part of the enum
        if (static_cast<int>(netEvent.type) == Common::ENet::SKIPPABLE_EVENT)
          INFO_LOG_FMT(NETPLAY, "enet_host_service: skippable packet event");
        else
          ERROR_LOG_FMT(NETPLAY, "enet_host_service: unknown event type: {}", int(netEvent.type));
        break;
      }
    }
    else if (net == 0)
    {
      INFO_LOG_FMT(NETPLAY, "enet_host_service: no event occurred");
    }
    else
    {
      ERROR_LOG_FMT(NETPLAY, "enet_host_service error: {}", net);
    }
  }

  INFO_LOG_FMT(NETPLAY, "NetPlayClient shutting down.");

  Disconnect();
  return;
}

// called from ---GUI--- thread
std::vector<const Player*> NetPlayClient::GetPlayers()
{
  std::lock_guard lkp(m_crit.players);
  std::vector<const Player*> players;

  for (const auto& pair : m_players)
    players.push_back(&pair.second);

  return players;
}

const NetSettings& NetPlayClient::GetNetSettings() const
{
  return m_net_settings;
}

// called from ---GUI--- thread
void NetPlayClient::SendChatMessage(const std::string& msg)
{
  sf::Packet packet;
  packet << MessageID::ChatMessage;
  packet << msg;

  SendAsync(std::move(packet));
}

void NetPlayClient::SendTeamSubmission(const std::string& payload)
{
  // Showdown text, not prebuilt party bytes: the host has to construct the
  // Pokemon against ITS save's trainer name/ID, and it is the single place
  // that parses untrusted team text. Pokepaste links are resolved by the
  // submitting client before this call, so the host never fetches a URL.
  //
  // The payload MAY carry an in-game trainer name ahead of the team ("Name: x"
  // then a blank line); it is built and parsed by
  // UICommon/XDNetplay/TeamInjector.h, which documents the grammar. Core does
  // not look inside it -- it stays one opaque string on one message ID, so an
  // older peer at either end still exchanges teams.
  sf::Packet packet;
  packet << MessageID::TeamData;
  packet << payload;

  SendAsync(std::move(packet));
}

// called from ---CPU--- thread
void NetPlayClient::AddPadStateToPacket(const int in_game_pad, const GCPadStatus& pad,
                                        sf::Packet& packet)
{
  packet << static_cast<PadIndex>(in_game_pad);
  packet << pad.button;
  if (!m_net_settings.gba_config[in_game_pad].enabled)
  {
    packet << pad.analogA << pad.analogB << pad.stickX << pad.stickY << pad.substickX
           << pad.substickY << pad.triggerLeft << pad.triggerRight << pad.isConnected;
  }
}

// called from ---CPU--- thread
void NetPlayClient::AddWiimoteStateToPacket(int in_game_pad,
                                            const WiimoteEmu::SerializedWiimoteState& state,
                                            sf::Packet& packet)
{
  packet << static_cast<PadIndex>(in_game_pad);
  packet << state.length;
  for (size_t i = 0; i < state.length; ++i)
    packet << state.data[i];
}

// called from ---GUI--- thread
void NetPlayClient::SendStartGamePacket()
{
  sf::Packet packet;
  packet << MessageID::StartGame;
  packet << m_current_game;

  SendAsync(std::move(packet));
}

// called from ---GUI--- thread
void NetPlayClient::SendStopGamePacket()
{
  sf::Packet packet;
  packet << MessageID::StopGame;

  SendAsync(std::move(packet));
}

// called from ---GUI--- thread
bool NetPlayClient::StartGame(const std::string& path)
{
  std::lock_guard lkg(m_crit.game);
  SendStartGamePacket();

  if (m_is_running.IsSet())
  {
    PanicAlertFmtT("Game is already running!");
    return false;
  }

  m_timebase_frame = 0;
  m_current_golfer = 1;
  m_wait_on_input = false;

  // XD netplay: arm the peer-vanish watchdog for this game. m_last_recv_ms must
  // start non-zero or the very first pad wait would read "silent since forever"
  // and the link-silence rule would trip on a session that has been fine.
  m_last_recv_ms.store(SteadyNowMs(), std::memory_order_relaxed);
  m_stop_requested_ms.store(0, std::memory_order_relaxed);
  m_session_end.store(SessionEndKind::None, std::memory_order_release);

  m_is_running.Set();
  NetPlay_Enable(this);

  ClearBuffers();

  m_first_pad_status_received.fill(false);

  if (m_dialog->IsRecording())
  {
    auto& movie = Core::System::GetInstance().GetMovie();
    if (movie.IsReadOnly())
      movie.SetReadOnly(false);

    Movie::ControllerTypeArray controllers{};
    Movie::WiimoteEnabledArray wiimotes{};
    for (unsigned int i = 0; i < 4; ++i)
    {
      if (m_net_settings.pad_map[i] > 0 && m_net_settings.gba_config[i].enabled)
        controllers[i] = Movie::ControllerType::GBA;
      else if (m_net_settings.pad_map[i] > 0)
        controllers[i] = Movie::ControllerType::GC;
      else
        controllers[i] = Movie::ControllerType::None;
      wiimotes[i] = m_net_settings.wiimote_map[i] > 0;
    }
    movie.BeginRecordingInput(controllers, wiimotes);
  }

  // boot game
  auto boot_session_data = std::make_unique<BootSessionData>();

  INFO_LOG_FMT(NETPLAY,
               "Setting Wii sync data: has FS {}, sync_titles = {:016x}, redirect folder = {}",
               !!m_wii_sync_fs, fmt::join(m_wii_sync_titles, ", "), m_wii_sync_redirect_folder);

  boot_session_data->SetWiiSyncData(std::move(m_wii_sync_fs), std::move(m_wii_sync_titles),
                                    std::move(m_wii_sync_redirect_folder), [] {
                                      // on emulation end clean up the Wii save sync directory --
                                      // see OnSyncSaveDataWii()
                                      const std::string wii_path = File::GetUserPath(D_USER_IDX) +
                                                                   "Wii" GC_MEMCARD_NETPLAY DIR_SEP;
                                      if (File::Exists(wii_path))
                                        File::DeleteDirRecursively(wii_path);
                                      const std::string redirect_path =
                                          File::GetUserPath(D_USER_IDX) +
                                          "Redirect" GC_MEMCARD_NETPLAY DIR_SEP;
                                      if (File::Exists(redirect_path))
                                        File::DeleteDirRecursively(redirect_path);
                                    });

  m_net_settings.local_player_id = m_local_player->pid;
  boot_session_data->SetNetplaySettings(std::make_unique<NetPlay::NetSettings>(m_net_settings));

  m_dialog->BootGame(path, std::move(boot_session_data));

  return true;
}

void NetPlayClient::SyncSaveDataResponse(const bool success)
{
  m_dialog->AppendChat(success ? Common::GetStringT("Data received!") :
                                 Common::GetStringT("Error processing data."));

  if (success)
  {
    if (++m_sync_save_data_success_count >= m_sync_save_data_count)
    {
      sf::Packet response_packet;
      response_packet << MessageID::SyncSaveData;
      response_packet << SyncSaveDataID::Success;

      Send(response_packet);
    }
  }
  else
  {
    sf::Packet response_packet;
    response_packet << MessageID::SyncSaveData;
    response_packet << SyncSaveDataID::Failure;

    Send(response_packet);
  }
}

void NetPlayClient::SyncCodeResponse(const bool success)
{
  // If something failed, immediately report back that code sync failed
  if (!success)
  {
    m_dialog->AppendChat(Common::GetStringT("Error processing codes."));

    sf::Packet response_packet;
    response_packet << MessageID::SyncCodes;
    response_packet << SyncCodeID::Failure;

    Send(response_packet);
    return;
  }

  // If both gecko and AR codes have completely finished transferring, report back as successful
  if (m_sync_gecko_codes_complete && m_sync_ar_codes_complete)
  {
    m_dialog->AppendChat(Common::GetStringT("Codes received!"));

    sf::Packet response_packet;
    response_packet << MessageID::SyncCodes;
    response_packet << SyncCodeID::Success;

    Send(response_packet);
  }
}

// called from ---GUI--- thread
bool NetPlayClient::ChangeGame(const std::string&)
{
  return true;
}

// called from ---NETPLAY--- thread
void NetPlayClient::ClearBuffers()
{
  // clear pad buffers, Clear method isn't thread safe
  for (unsigned int i = 0; i < 4; ++i)
  {
    while (m_pad_buffer[i].Size())
      m_pad_buffer[i].Pop();

    while (m_wiimote_buffer[i].Size())
      m_wiimote_buffer[i].Pop();
  }
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnTraversalStateChanged()
{
  const Common::TraversalClient::State state = m_traversal_client->GetState();

  if (m_connection_state == ConnectionState::WaitingForTraversalClientConnection &&
      state == Common::TraversalClient::State::Connected)
  {
    m_connection_state = ConnectionState::WaitingForTraversalClientConnectReady;
    m_traversal_client->ConnectToClient(m_host_spec);
  }
  else if (m_connection_state != ConnectionState::Failure &&
           state == Common::TraversalClient::State::Failure)
  {
    Disconnect();
    m_dialog->OnTraversalError(m_traversal_client->GetFailureReason());
  }
  m_dialog->OnTraversalStateChanged(state);
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnConnectReady(ENetAddress addr)
{
  if (m_connection_state == ConnectionState::WaitingForTraversalClientConnectReady)
  {
    m_connection_state = ConnectionState::Connecting;
    enet_host_connect(m_client, &addr, CHANNEL_COUNT, 0);
  }
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnConnectFailed(Common::TraversalConnectFailedReason reason)
{
  m_connecting = false;
  m_connection_state = ConnectionState::Failure;
  // Through the dialog, not PanicAlert: on Android a PanicAlert with no emulation activity
  // degrades to a 3.5-second toast, after which the ctor's generic "Could not communicate with
  // host." dialog buried the real answer -- a WRONG CODE was indistinguishable from a NAT
  // failure. The flag suppresses that generic follow-up on every platform.
  m_specific_connect_error = true;
  switch (reason)
  {
  case Common::TraversalConnectFailedReason::ClientDidntRespond:
    m_dialog->OnConnectionError(
        _trans("The traversal server reached the host, but the host never answered. The host's "
               "network may be blocking the connection."));
    break;
  case Common::TraversalConnectFailedReason::ClientFailure:
    m_dialog->OnConnectionError(_trans("The traversal server rejected the connection attempt."));
    break;
  case Common::TraversalConnectFailedReason::NoSuchClient:
    m_dialog->OnConnectionError(
        _trans("No host with that code exists right now. Check the code with the host -- and have "
               "them check their room still shows it, since codes die when the room closes."));
    break;
  default:
    m_dialog->OnConnectionError(_trans("The traversal server reported an unknown error."));
    break;
  }
}

// called from ---CPU--- thread
//
// Name of the player who owns an in-game pad, for the "who are we waiting on?"
// message. Deliberately does NOT go through NetPlay::GetPadDetails(): that free
// function locks crit_netplay_client, and every caller of this one is already
// holding it (NetPlay_GetInput takes it for the whole of GetNetPads), so it
// would self-deadlock a plain std::mutex. Reading m_players under m_crit.players
// is fine -- that lock is only ever held for a handful of instructions.
std::string NetPlayClient::DescribePadOwner(const int pad_nb)
{
  if (pad_nb < 0 || static_cast<size_t>(pad_nb) >= m_net_settings.pad_map.size())
    return {};

  const PlayerId owner = m_net_settings.pad_map[pad_nb];
  if (owner == 0)
    return {};

  std::lock_guard lkp(m_crit.players);
  const auto it = m_players.find(owner);
  return it == m_players.end() ? std::string{} : it->second.name;
}

// called from ---CPU--- thread
//
// True only when we can PROVE the pad's owner is no longer in the room: we know
// which player owns the slot, and the server has since told us they left --
// MessageID::PlayerLeave, which is the only thing that erases anybody from
// m_players (see OnPlayerLeave). Everything we cannot answer confidently,
// including "that index is not a pad index" and "the slot is unmapped",
// answers false.
//
// The asymmetry is deliberate, because this gates the backstop in
// WaitOnRemote(): a wrong "true" ends a session that is alive, while a wrong
// "false" only means we keep waiting -- which is exactly the situation ENet's
// PEER_TIMEOUT already exists to resolve. Same locking note as
// DescribePadOwner(): m_crit.players directly, never NetPlay::GetPadDetails().
bool NetPlayClient::PadOwnerHasLeftRoom(const int pad_nb)
{
  if (pad_nb < 0 || static_cast<size_t>(pad_nb) >= m_net_settings.pad_map.size())
    return false;

  const PlayerId owner = m_net_settings.pad_map[pad_nb];
  if (owner == 0)
    return false;

  std::lock_guard lkp(m_crit.players);
  return m_players.find(owner) == m_players.end();
}

// called from ---CPU--- thread (safe from any thread)
void NetPlayClient::DeclareSessionLost(const SessionEndKind kind, const std::string& reason)
{
  // First one in wins; later slices of the same stall must not re-announce.
  // Kind and "who won" live in the same atomic on purpose: with a separate flag
  // plus a separate payload, a losing caller could overwrite the winner's kind,
  // or the netplay thread could see the flag before the kind it belongs to.
  SessionEndKind expected = SessionEndKind::None;
  if (!m_session_end.compare_exchange_strong(expected, kind, std::memory_order_acq_rel,
                                             std::memory_order_relaxed))
  {
    return;
  }

  if (kind == SessionEndKind::LocalStopCompleted)
  {
    // NOT an error and NOT worth alarming anybody about. The user asked for this
    // stop; all that happened is that we finished it ourselves instead of
    // waiting on an acknowledgement that was slow or was never coming. Both UIs
    // call RequestStopGame() from the Core state-changed hook, so this path is
    // reached by ORDINARY shutdowns on a laggy link -- a red "the server never
    // acknowledged the stop" banner there would be a scary lie about a session
    // that ended correctly. A log line is the right amount of noise.
    INFO_LOG_FMT(NETPLAY, "Netplay session ended locally: {}", reason);
  }
  else
  {
    ERROR_LOG_FMT(NETPLAY, "Ending netplay session: {}", reason);

    // The OSD is the part the player is actually looking at when this fires --
    // they are staring at a stopped render window, not at the netplay dialog --
    // so put the explanation there. OSD::AddMessage takes its own lock and is
    // called from the CPU thread all over Dolphin.
    OSD::AddMessage(reason, OSD::Duration::VERY_LONG, OSD::Color::RED);
  }

  // Free the CPU thread immediately: every wait in this class keys off
  // m_is_running, and InvokeStop() touches nothing but flags and events.
  InvokeStop();

  // The REST of the teardown has to happen on the NETPLAY thread. StopGame()
  // reaches NetPlay_Disable(), which locks crit_netplay_client -- and when we
  // are called from the CPU thread that mutex is already held by us one frame
  // up in NetPlay_GetInput(). It is a plain std::mutex, so re-entering it here
  // would be a self-deadlock: the same freeze we are removing, moved one
  // function over. Poke the netplay thread instead; its loop notices the flag
  // on the next pass (immediately thanks to the wakeup, and within 250 ms even
  // if the wakeup datagram is dropped, since that is its enet_host_service
  // timeout).
  if (m_client)
    Common::ENet::WakeupThread(m_client);
}

// called from ---CPU--- thread
//
// One bounded slice of "block until a remote player's frame shows up".
//
// Upstream spends these waits in a bare Common::Event::Wait(), which is only
// correct for as long as SOMETHING is guaranteed to eventually call
// InvokeStop(). Nothing is, once a peer's process is killed outright. A
// swiped-away Android task never runs its teardown, so no ENet disconnect is
// ever put on the wire; the surviving clients' only remaining escape is
// whoever is hosting noticing the silence after PEER_TIMEOUT (30 s) and
// broadcasting DisableGame. And if the machine that vanished WAS the host,
// even that never comes: the local user's own Stop cannot rescue them either,
// because RequestStopGame() only mails a packet at the dead server. The CPU
// thread then sits in Wait() forever while holding crit_netplay_client, the
// render window stops updating, and Core::Stop() can never join the emu
// thread -- the reported "have to force-quit" freeze.
//
// So: wait in slices and look around in between. Nothing here touches pad
// contents or any emulated state. A timeout can only ever END the session; it
// never fabricates an input, so it cannot desync anyone. Returns false when the
// caller must abandon its wait -- by then m_is_running is clear, so the caller
// bails through exactly the same path a normal stop uses.
//
// Note what this deliberately does NOT do: end a session merely because time has
// passed. Every rule below needs evidence that the session is actually over --
// the user asked to stop (1), our own socket has gone silent (2), or the server
// has told us the player we are waiting on left the room (3). Waiting is not a
// failure state in this fork: an Android peer that got a phone call is paused,
// not gone, and it must still be here when they come back. See rule 3.
bool NetPlayClient::WaitOnRemote(Common::Event& wait_event, const int pad_nb,
                                 RemoteWaitState& state)
{
  if (!state.started_valid)
  {
    // Stamped once per wait loop, never refreshed. Refreshing it whenever the
    // event fires would be wrong: the pad events are shared across all pads, so
    // in a 3+ player room a healthy player's frames would keep resetting the
    // clock on a dead player's stall and the watchdog would never fire.
    state.started = std::chrono::steady_clock::now();
    state.started_valid = true;
    state.next_notice_ms = PAD_STALL_NOTICE_MS;
  }

  if (wait_event.WaitFor(PAD_WAIT_SLICE))
    return true;

  const auto now = std::chrono::steady_clock::now();
  const u64 now_ms =
      static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                           .count());
  const u64 stalled_ms = static_cast<u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now - state.started).count());

  // 1. The user already asked to quit and no stop has landed yet. Very often the
  // server IS the machine that vanished, so waiting for its blessing is waiting
  // forever -- but the ordinary case is just a slow link, so this is a
  // completely expected outcome and is reported as one. The stamp is cleared by
  // InvokeStop(), i.e. the moment any real stop lands, so reaching here means no
  // stop has landed by any route.
  if (const u64 stop_requested = m_stop_requested_ms.load(std::memory_order_relaxed);
      stop_requested != 0 && now_ms - stop_requested >= LOCAL_STOP_GRACE_MS)
  {
    DeclareSessionLost(SessionEndKind::LocalStopCompleted,
                       Common::GetStringT("NetPlay: finishing the requested stop locally."));
    return false;
  }

  // 2. Dead silence on our own socket. The server pings every client at 1 Hz
  // while a room is open, so LINK_SILENT_MS is 20 consecutive missed pings on
  // top of an already-starved game -- that is a gone host, not a slow one.
  // ENet's own PEER_TIMEOUT verdict is still coming and is still authoritative;
  // this just stops us from being a dead window for the rest of that wait.
  if (const u64 last_recv = m_last_recv_ms.load(std::memory_order_relaxed);
      last_recv != 0 && now_ms - last_recv >= LINK_SILENT_MS)
  {
    DeclareSessionLost(
        SessionEndKind::PeerLost,
        Common::GetStringT("NetPlay: lost contact with the host. Ending this session."));
    return false;
  }

  // 3. The pad we are starved of belongs to a player the server has already told
  // us left the room. Nobody is ever going to send that slot's frames again, so
  // waiting is pointless however healthy our own link looks. Parked past
  // PEER_TIMEOUT so that normally the server's DisableGame broadcast (it sends
  // one whenever a player with a mapped pad drops) gets here first and this
  // never fires; it covers the case where PlayerLeave arrived but a stop never
  // did.
  //
  // The membership test is load-bearing, not a refinement. This rule used to be
  // an unconditional "no pad for 45 s -> end the session", and that was wrong in
  // the way that matters most to this fork: Android pauses emulation on ANY
  // backgrounding -- screen lock, incoming call, pulling down the notification
  // shade -- and a paused peer stops producing pad frames while its socket stays
  // wide open. The server's 1 Hz ping therefore keeps rule 2 quiet and ENet
  // never times out, so the unconditional backstop was the only thing that
  // fired, and it fired on a completely healthy session: it hung up on people
  // for glancing at a notification, the exact behaviour
  // NetplayTaskWatcherService documents that we must never ship. A paused
  // opponent has to be able to come back and find the battle still there, and no
  // wall-clock number can tell "paused" from "gone" -- a phone call can outlast
  // any threshold anybody would accept.
  //
  // Room membership is the one signal that cannot be confused with a pause: a
  // paused player is still in m_players and only the server removing them takes
  // them out. So there is deliberately no unconditional timer left here. The
  // remaining gap -- a peer that is truly gone but that the server still lists,
  // or a stall on a wait with no pad owner to check (golf handoff, Wiimote) --
  // is closed by ENet's own PEER_TIMEOUT at both ends: the server drops the dead
  // peer at 30 s and broadcasts DisableGame + PlayerLeave, and if the dead
  // machine WAS the server, rule 2 has already fired at 20 s. And the user's own
  // Stop now always works within LOCAL_STOP_GRACE_MS via rule 1, which is the
  // right escape hatch for "my opponent has been paused for ten minutes":
  // a human decision, not a timer's.
  if (stalled_ms >= PAD_OWNER_GONE_ABORT_MS && PadOwnerHasLeftRoom(pad_nb))
  {
    DeclareSessionLost(SessionEndKind::PeerLost,
                       Common::GetStringT("NetPlay: the other player left the room. "
                                          "Ending this session."));
    return false;
  }

  // Still plausibly alive: somebody's console is just behind. Say so on screen,
  // and keep saying it, so the window never simply looks dead. This is the part
  // that turns a mystery freeze into "oh, they dropped".
  if (stalled_ms >= state.next_notice_ms)
  {
    state.next_notice_ms = stalled_ms + (stalled_ms >= PAD_STALL_SLOW_NOTICE_AFTER_MS ?
                                             PAD_STALL_REPEAT_SLOW_MS :
                                             PAD_STALL_REPEAT_MS);

    const std::string who = DescribePadOwner(pad_nb);
    const u64 stalled_s = stalled_ms / 1000;
    std::string msg;
    if (who.empty())
      msg = Common::FmtFormatT("Waiting for the other player... ({0}s)", stalled_s);
    else
      msg = Common::FmtFormatT("Waiting for {0}... ({1}s)", who, stalled_s);

    OSD::AddTypedMessage(OSD::MessageType::NetPlayLatency, msg, OSD::Duration::NORMAL,
                         OSD::Color::YELLOW);
    WARN_LOG_FMT(NETPLAY, "Stalled {} ms waiting on in-game pad {}", stalled_ms, pad_nb);
  }

  return true;
}

// called from ---CPU--- thread
bool NetPlayClient::GetNetPads(const int pad_nb, const bool batching, GCPadStatus* pad_status)
{
  // The interface for this is extremely silly.
  //
  // Imagine a physical device that links three GameCubes together
  // and emulates NetPlay that way. Which GameCube controls which
  // in-game controllers can be configured on the device (m_pad_map)
  // but which sockets on each individual GameCube should be used
  // to control which players? The solution that Dolphin uses is
  // that we hardcode the knowledge that they go in order, so if
  // you have a 3P game with three GameCubes, then every single
  // controller should be plugged into slot 1.
  //
  // If you have a 4P game, then one of the GameCubes will have
  // a controller plugged into slot 1, and another in slot 2.
  //
  // The slot number is the "local" pad number, and what player
  // it actually means is the "in-game" pad number.

  // When the 1st in-game pad is polled and batching is set, the
  // others will be polled as well. To reduce latency, we poll all
  // local controllers at once and then send the status to the other
  // clients.
  //
  // Batching is enabled when polled from VI. If batching is not
  // enabled, the poll is probably from MMIO, which can poll any
  // specific pad arbitrarily. In this case, we poll just that pad
  // and send it.

  // When here when told to so we don't deadlock in certain situations
  RemoteWaitState golf_wait;
  while (m_wait_on_input)
  {
    if (!m_is_running.IsSet())
    {
      return false;
    }

    if (m_wait_on_input_received)
    {
      // Tell the server we've acknowledged the message
      sf::Packet spac;
      spac << MessageID::GolfPrepare;
      Send(spac);

      m_wait_on_input_received = false;
    }

    // Same watchdog as the pad wait below: the handoff we are waiting for comes
    // from the host, so it never lands if the host is the machine that died.
    // pad_nb is meaningless here (this wait is about input control, not one
    // slot), so -1.
    if (!WaitOnRemote(m_wait_on_input_event, -1, golf_wait))
      return false;
  }

  if (IsFirstInGamePad(pad_nb) && batching)
  {
    sf::Packet packet;
    packet << MessageID::PadData;

    bool send_packet = false;
    const int num_local_pads = NumLocalPads();
    for (int local_pad = 0; local_pad < num_local_pads; local_pad++)
    {
      send_packet = PollLocalPad(local_pad, packet) || send_packet;
    }

    if (send_packet)
      SendAsync(std::move(packet));

    if (m_host_input_authority)
      SendPadHostPoll(-1);
  }

  if (!batching)
  {
    const int local_pad = InGamePadToLocalPad(pad_nb);
    if (local_pad < 4)
    {
      sf::Packet packet;
      packet << MessageID::PadData;
      if (PollLocalPad(local_pad, packet))
        SendAsync(std::move(packet));
    }

    if (m_host_input_authority)
      SendPadHostPoll(pad_nb);
  }

  if (m_host_input_authority)
  {
    if (m_local_player->pid != m_current_golfer)
    {
      // CoreTiming acts funny and causes what looks like frame skip if
      // we toggle the emulation speed too quickly, so to prevent this
      // we wait until the buffer has been over for at least 1 second.

      const bool buffer_over_target = m_pad_buffer[pad_nb].Size() > m_target_buffer_size + 1;
      if (!buffer_over_target)
        m_buffer_under_target_last = std::chrono::steady_clock::now();

      std::chrono::duration<double> time_diff =
          std::chrono::steady_clock::now() - m_buffer_under_target_last;
      if (time_diff.count() >= 1.0 || !buffer_over_target)
      {
        // run fast if the buffer is overfilled, otherwise run normal speed
        Config::SetCurrent(Config::MAIN_EMULATION_SPEED, buffer_over_target ? 0.0f : 1.0f);
      }
    }
    else
    {
      // Set normal speed when we're the host, otherwise it can get stuck at unlimited
      Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 1.0f);
    }
  }

  // Now, we either use the data pushed earlier, or wait for the
  // other clients to send it to us
  const auto lat_t0 = std::chrono::steady_clock::now();
  RemoteWaitState pad_wait;
  while (m_pad_buffer[pad_nb].Size() == 0)
  {
    if (!m_is_running.IsSet())
    {
      return false;
    }

    // THE hang. This used to be an unbounded m_gc_pad_event.Wait(), which is
    // where a desktop client parks forever when the Android player's process is
    // killed mid-battle. See WaitOnRemote().
    if (!WaitOnRemote(m_gc_pad_event, pad_nb, pad_wait))
      return false;
  }

  // Latency telemetry: how long this pad fetch had to wait for a remote frame,
  // and how full the buffer was. All CPU-thread-local; nothing here affects the
  // input stream or emulation. Read the REMOTE pad's numbers: waits piling into
  // the high buckets with a near-empty buffer means network jitter (raise the
  // buffer); ~zero waits with the buffer at target means the delay is the buffer
  // itself (lower it); ~zero waits at target but turns still feel slow means it
  // is XD's own pacing, which no netcode can shorten.
  {
    const u64 wait_us = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - lat_t0)
            .count());
    const u32 depth = static_cast<u32>(m_pad_buffer[pad_nb].Size());
    const int b = wait_us < 1000 ? 0 : wait_us < 5000 ? 1 : wait_us < 16667 ? 2 :
                  wait_us < 33334 ? 3 : wait_us < 100000 ? 4 : wait_us < 250000 ? 5 : 6;
    m_lat_bucket[b]++;
    m_lat_pops++;
    if (wait_us > 1000)
      m_lat_starve_pops++;
    m_lat_wait_ewma_us = m_lat_wait_ewma_us * 0.98 + static_cast<double>(wait_us) * 0.02;
    m_lat_wait_max_us = std::max(m_lat_wait_max_us, wait_us);

    const u64 now_us = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (m_lat_last_emit_us == 0)
      m_lat_last_emit_us = now_us;
    else if (now_us - m_lat_last_emit_us > 1'000'000)
    {
      m_lat_last_emit_us = now_us;
      const std::string line = fmt::format(
          "NetLat ping={}ms buf={}/{} wait~{:.0f}ms(max{}) starve={}/{}", GetPlayersMaxPing(),
          depth, m_target_buffer_size, m_lat_wait_ewma_us / 1000.0, m_lat_wait_max_us / 1000,
          m_lat_starve_pops, m_lat_pops);
      // No OSD line anymore: the once-per-second flash on the play screen was
      // distracting in real sessions. The same readout goes into the session's
      // gba_detect log instead (same t= clock as everything else, and the
      // Android Share button already delivers it), plus the NETPLAY log.
#ifdef HAS_LIBMGBA
      GBADetectLog::LogEvent(0,
                             Core::System::GetInstance().GetCoreTiming().GetTicks(), "netlat",
                             line, false);
#endif
      NOTICE_LOG_FMT(NETPLAY, "{} buckets[<1|<5|<16|<33|<100|<250|hi]={},{},{},{},{},{},{}", line,
                     m_lat_bucket[0], m_lat_bucket[1], m_lat_bucket[2], m_lat_bucket[3],
                     m_lat_bucket[4], m_lat_bucket[5], m_lat_bucket[6]);
      m_lat_wait_max_us = 0;
      m_lat_starve_pops = 0;
      m_lat_pops = 0;
      for (u32& bkt : m_lat_bucket)
        bkt = 0;
    }
  }

  m_pad_buffer[pad_nb].Pop(*pad_status);

  auto& movie = Core::System::GetInstance().GetMovie();
  if (movie.IsRecordingInput())
  {
    movie.RecordInput(pad_status, pad_nb);
    movie.InputUpdate();
  }
  else
  {
    movie.CheckPadStatus(pad_status, pad_nb);
  }

  return true;
}

u64 NetPlayClient::GetInitialRTCValue() const
{
  return m_initial_rtc;
}

// called from ---CPU--- thread
bool NetPlayClient::WiimoteUpdate(const std::span<WiimoteDataBatchEntry>& entries)
{
  for (const WiimoteDataBatchEntry& entry : entries)
  {
    const int local_wiimote = InGameWiimoteToLocalWiimote(entry.wiimote);
    DEBUG_LOG_FMT(NETPLAY,
                  "Entering WiimoteUpdate() with wiimote {}, local_wiimote {}, state [{:02x}]",
                  entry.wiimote, local_wiimote,
                  fmt::join(std::span(entry.state->data.data(), entry.state->length), ", "));
    if (local_wiimote < 4)
    {
      sf::Packet packet;
      packet << MessageID::WiimoteData;
      if (AddLocalWiimoteToBuffer(local_wiimote, *entry.state, packet))
        SendAsync(std::move(packet));
    }

    // Now, we either use the data pushed earlier, or wait for the
    // other clients to send it to us
    RemoteWaitState wiimote_wait;
    while (m_wiimote_buffer[entry.wiimote].Size() == 0)
    {
      if (!m_is_running.IsSet())
      {
        return false;
      }

      // Same watchdog as GetNetPads: this is the identical hang with a Wiimote
      // instead of a pad. XD never gets here, but leaving one unbounded Wait()
      // in the class would just relocate the freeze for anyone who does. -1
      // rather than the slot, because DescribePadOwner reads pad_map and this
      // index belongs to wiimote_map -- it would name the wrong player.
      if (!WaitOnRemote(m_wii_pad_event, -1, wiimote_wait))
        return false;
    }

    m_wiimote_buffer[entry.wiimote].Pop(*entry.state);

    DEBUG_LOG_FMT(NETPLAY, "Exiting WiimoteUpdate() with wiimote {}, state [{:02x}]", entry.wiimote,
                  fmt::join(std::span(entry.state->data.data(), entry.state->length), ", "));
  }

  return true;
}

bool NetPlayClient::PollLocalPad(const int local_pad, sf::Packet& packet)
{
  const int ingame_pad = LocalPadToInGamePad(local_pad);
  bool data_added = false;
  GCPadStatus pad_status;

  if (m_net_settings.gba_config[ingame_pad].enabled)
  {
    pad_status = Pad::GetGBAStatus(local_pad);
  }
  else if (Config::Get(Config::GetInfoForSIDevice(local_pad)) ==
           SerialInterface::SIDEVICE_WIIU_ADAPTER)
  {
    pad_status = GCAdapter::Input(local_pad);
  }
  else
  {
    pad_status = Pad::GetStatus(local_pad);
  }

  if (m_host_input_authority)
  {
    if (m_local_player->pid != m_current_golfer)
    {
      // add to packet
      AddPadStateToPacket(ingame_pad, pad_status, packet);
      data_added = true;
    }
    else
    {
      // set locally
      m_last_pad_status[ingame_pad] = pad_status;
      m_first_pad_status_received[ingame_pad] = true;
    }
  }
  else
  {
    // adjust the buffer either up or down
    // inserting multiple padstates or dropping states
    while (m_pad_buffer[ingame_pad].Size() <= m_target_buffer_size)
    {
      // add to buffer
      m_pad_buffer[ingame_pad].Push(pad_status);

      // add to packet
      AddPadStateToPacket(ingame_pad, pad_status, packet);
      data_added = true;
    }
  }

  return data_added;
}

bool NetPlayClient::AddLocalWiimoteToBuffer(const int local_wiimote,
                                            const WiimoteEmu::SerializedWiimoteState& state,
                                            sf::Packet& packet)
{
  const int ingame_pad = LocalWiimoteToInGameWiimote(local_wiimote);
  bool data_added = false;

  // adjust the buffer either up or down
  // inserting multiple padstates or dropping states
  while (m_wiimote_buffer[ingame_pad].Size() <= m_target_buffer_size)
  {
    // add to buffer
    m_wiimote_buffer[ingame_pad].Push(state);

    // add to packet
    AddWiimoteStateToPacket(ingame_pad, state, packet);
    data_added = true;
  }

  return data_added;
}

void NetPlayClient::SendPadHostPoll(const PadIndex pad_num)
{
  // Here we handle polling for the Host Input Authority and Golf modes. Pad data is "polled" from
  // the most recent data received for the given pad. Passing pad_num < 0 will poll all assigned
  // pads (used for batched polls), while 0..3 will poll the respective pad (used for MMIO polls).
  // See GetNetPads for more details.
  //
  // If the local buffer is non-empty, we skip actually buffering and sending new pad data, this way
  // don't end up with permanent local latency. It does create a period of time where no inputs are
  // accepted, but under typical circumstances this is not noticeable.
  //
  // Additionally, we wait until some actual pad data has been received before buffering and sending
  // it, otherwise controllers get calibrated wrongly with the default values of GCPadStatus.

  if (m_local_player->pid != m_current_golfer)
    return;

  sf::Packet packet;
  packet << MessageID::PadHostData;

  if (pad_num < 0)
  {
    for (size_t i = 0; i < m_net_settings.pad_map.size(); i++)
    {
      if (m_net_settings.pad_map[i] <= 0)
        continue;

      // Bounded for the same reason as GetNetPads': the first pad status comes
      // from another player, so a player who dies before sending it would wedge
      // the golfer here forever.
      RemoteWaitState first_status_wait;
      while (!m_first_pad_status_received[i])
      {
        if (!m_is_running.IsSet())
          return;

        if (!WaitOnRemote(m_first_pad_status_received_event, static_cast<int>(i),
                          first_status_wait))
          return;
      }
    }

    for (size_t i = 0; i < m_net_settings.pad_map.size(); i++)
    {
      if (m_net_settings.pad_map[i] == 0 || m_pad_buffer[i].Size() > 0)
        continue;

      const GCPadStatus& pad_status = m_last_pad_status[i];
      m_pad_buffer[i].Push(pad_status);
      AddPadStateToPacket(static_cast<int>(i), pad_status, packet);
    }
  }
  else if (m_net_settings.pad_map[pad_num] != 0)
  {
    RemoteWaitState first_status_wait;
    while (!m_first_pad_status_received[pad_num])
    {
      if (!m_is_running.IsSet())
        return;

      if (!WaitOnRemote(m_first_pad_status_received_event, pad_num, first_status_wait))
        return;
    }

    if (m_pad_buffer[pad_num].Size() == 0)
    {
      const GCPadStatus& pad_status = m_last_pad_status[pad_num];
      m_pad_buffer[pad_num].Push(pad_status);
      AddPadStateToPacket(pad_num, pad_status, packet);
    }
  }

  SendAsync(std::move(packet));
}

void NetPlayClient::InvokeStop()
{
  m_is_running.Clear();

  // XD netplay: a stop has now really landed, so the "our Stop has not been
  // answered yet" deadline is satisfied and must be disarmed. Every route by
  // which a game actually stops funnels through here -- StopGame() (which is
  // what the server's StopGame/DisableGame broadcast runs), Stop(),
  // OnPowerButton(), DeclareSessionLost() -- which is why the clear belongs
  // here and not in any one of them.
  //
  // Previously only StartGame() cleared it, and that produced a false alarm on
  // perfectly ordinary shutdowns: both UIs call RequestStopGame() from the Core
  // state-changed hook (on Stopping, while the CPU thread can still be parked in
  // a pad wait), and it does not even put a packet on the wire unless we have a
  // pad mapped. The stamp therefore stayed armed through a normal stop and, on a
  // slow link, rule 1 in WaitOnRemote() fired and shouted about a server that
  // had done nothing wrong.
  m_stop_requested_ms.store(0, std::memory_order_relaxed);

  // stop waiting for input
  m_gc_pad_event.Set();
  m_wii_pad_event.Set();
  m_first_pad_status_received_event.Set();
  m_wait_on_input_event.Set();
}

// called from ---GUI--- thread and ---NETPLAY--- thread (client side)
bool NetPlayClient::StopGame()
{
  InvokeStop();

  NetPlay_Disable();

  // stop game
  m_dialog->StopGame();

  return true;
}

// called from ---GUI--- thread
void NetPlayClient::Stop()
{
  if (!m_is_running.IsSet())
    return;

  InvokeStop();

  // Tell the server to stop if we have a pad mapped in game.
  if (LocalPlayerHasControllerMapped())
    SendStopGamePacket();
  else
    StopGame();
}

void NetPlayClient::RequestStopGame()
{
  // XD netplay: arm the local-stop deadline BEFORE sending, so the CPU thread's
  // pad wait can give up on the server's answer. This call is only ever made
  // when the local core is already on its way down (both UIs hook it to the
  // Core state-changed callback), so stopping ourselves after the grace period
  // is not a policy decision -- it is finishing what the user asked for. Without
  // it, hitting Stop while starved of a dead player's frames does nothing at
  // all: the packet below goes to a server that is very often the machine that
  // just vanished, and we would wait on the reply forever.
  //
  // Gated on m_is_running so a stale queued EmulationStateChanged(Uninitialized)
  // from the PREVIOUS game cannot land just after StartGame() cleared this and
  // arm a stop deadline against a session that is only now booting.
  if (m_is_running.IsSet())
    m_stop_requested_ms.store(SteadyNowMs(), std::memory_order_relaxed);

  // Tell the server to stop if we have a pad mapped in game.
  if (LocalPlayerHasControllerMapped())
    SendStopGamePacket();
}

void NetPlayClient::SendPowerButtonEvent()
{
  sf::Packet packet;
  packet << MessageID::PowerButton;
  SendAsync(std::move(packet));
}

void NetPlayClient::RequestGolfControl(const PlayerId pid)
{
  if (!m_host_input_authority || !m_net_settings.golf_mode)
    return;

  sf::Packet packet;
  packet << MessageID::GolfRequest;
  packet << pid;
  SendAsync(std::move(packet));
}

void NetPlayClient::RequestGolfControl()
{
  RequestGolfControl(m_local_player->pid);
}

// called from ---GUI--- thread
std::string NetPlayClient::GetCurrentGolfer()
{
  std::lock_guard lkp(m_crit.players);
  if (const auto it = m_players.find(m_current_golfer); it != m_players.end())
    return it->second.name;
  return "";
}

// called from ---GUI--- thread
bool NetPlayClient::LocalPlayerHasControllerMapped() const
{
  return PlayerHasControllerMapped(m_local_player->pid);
}

bool NetPlayClient::IsFirstInGamePad(int ingame_pad) const
{
  return std::none_of(m_net_settings.pad_map.begin(), m_net_settings.pad_map.begin() + ingame_pad,
                      [](auto mapping) { return mapping > 0; });
}

int NetPlayClient::NumLocalPads() const
{
  return std::ranges::count(m_net_settings.pad_map, m_local_player->pid);
}

int NetPlayClient::NumLocalWiimotes() const
{
  return std::ranges::count(m_net_settings.wiimote_map, m_local_player->pid);
}

static int InGameToLocal(int ingame_pad, const PadMappingArray& pad_map, PlayerId local_player_pid)
{
  // not our pad
  if (pad_map[ingame_pad] != local_player_pid)
    return 4;

  int local_pad = 0;
  int pad = 0;

  for (; pad < ingame_pad; ++pad)
  {
    if (pad_map[pad] == local_player_pid)
      local_pad++;
  }

  return local_pad;
}

static int LocalToInGame(int local_pad, const PadMappingArray& pad_map, PlayerId local_player_pid)
{
  // Figure out which in-game pad maps to which local pad.
  // The logic we have here is that the local slots always
  // go in order.
  int local_pad_count = -1;
  int ingame_pad = 0;
  for (; ingame_pad < 4; ingame_pad++)
  {
    if (pad_map[ingame_pad] == local_player_pid)
      local_pad_count++;

    if (local_pad_count == local_pad)
      break;
  }

  return ingame_pad;
}

int NetPlayClient::InGamePadToLocalPad(int ingame_pad) const
{
  return InGameToLocal(ingame_pad, m_net_settings.pad_map, m_local_player->pid);
}

int NetPlayClient::LocalPadToInGamePad(int local_pad) const
{
  return LocalToInGame(local_pad, m_net_settings.pad_map, m_local_player->pid);
}

int NetPlayClient::InGameWiimoteToLocalWiimote(int ingame_wiimote) const
{
  return InGameToLocal(ingame_wiimote, m_net_settings.wiimote_map, m_local_player->pid);
}

int NetPlayClient::LocalWiimoteToInGameWiimote(int local_wiimote) const
{
  return LocalToInGame(local_wiimote, m_net_settings.wiimote_map, m_local_player->pid);
}

bool NetPlayClient::PlayerHasControllerMapped(const PlayerId pid) const
{
  const auto mapping_matches_player_id = [pid](const PlayerId& mapping) { return mapping == pid; };

  return std::ranges::any_of(m_net_settings.pad_map, mapping_matches_player_id) ||
         std::ranges::any_of(m_net_settings.wiimote_map, mapping_matches_player_id);
}

bool NetPlayClient::IsLocalPlayer(const PlayerId pid) const
{
  return pid == m_local_player->pid;
}

const PlayerId& NetPlayClient::GetLocalPlayerId() const
{
  return m_local_player->pid;
}

void NetPlayClient::SendGameStatus()
{
  sf::Packet packet;
  packet << MessageID::GameStatus;

  SyncIdentifierComparison result;
  m_dialog->FindGameFile(m_selected_game, &result);
  for (size_t i = 0; i < 4; ++i)
  {
    if (m_net_settings.gba_config[i].enabled && m_net_settings.gba_config[i].has_rom &&
        m_net_settings.gba_rom_paths[i].empty())
    {
      result = SyncIdentifierComparison::DifferentGame;
    }
  }

  packet << result;
  Send(packet);
}

void NetPlayClient::SendTimeBase()
{
  std::lock_guard lk(crit_netplay_client);

  if (netplay_client->m_timebase_frame % 60 == 0)
  {
    const u64 timebase = Core::System::GetInstance().GetSystemTimers().GetFakeTimeBase();

    sf::Packet packet;
    packet << MessageID::TimeBase;
    packet << timebase;
    packet << netplay_client->m_timebase_frame;

    netplay_client->SendAsync(std::move(packet));
  }

  netplay_client->m_timebase_frame++;
}

bool NetPlayClient::DoAllPlayersHaveGame()
{
  std::lock_guard lkp(m_crit.players);

  return std::ranges::all_of(m_players, [](const auto& entry) {
    return entry.second.game_status == SyncIdentifierComparison::SameGame;
  });
}

static std::string SHA1Sum(const std::string& file_path,
                           const std::function<bool(int)>& report_progress)
{
  std::vector<u8> data(8 * 1024 * 1024);
  u64 read_offset = 0;

  std::unique_ptr<DiscIO::BlobReader> file(DiscIO::CreateBlobReader(file_path));
  u64 game_size = file->GetDataSize();

  auto ctx = Common::SHA1::CreateContext();

  while (read_offset < game_size)
  {
    size_t read_size = std::min(static_cast<u64>(data.size()), game_size - read_offset);
    if (!file->Read(read_offset, read_size, data.data()))
      return "";

    ctx->Update(data.data(), read_size);
    read_offset += read_size;

    int progress =
        static_cast<int>(static_cast<float>(read_offset) / static_cast<float>(game_size) * 100);
    if (!report_progress(progress))
      return "";
  }

  // Convert to hex
  return fmt::format("{:02x}", fmt::join(ctx->Finish(), ""));
}

void NetPlayClient::ComputeGameDigest(const SyncIdentifier& sync_identifier)
{
  if (m_should_compute_game_digest)
    return;

  m_dialog->ShowGameDigestDialog(sync_identifier.game_id);
  m_should_compute_game_digest = true;

  std::string file;
  if (sync_identifier == GetSDCardIdentifier())
    file = File::GetUserPath(F_WIISDCARDIMAGE_IDX);
  else if (auto game = m_dialog->FindGameFile(sync_identifier))
    file = game->GetFilePath();

  if (file.empty() || !File::Exists(file))
  {
    sf::Packet packet;
    packet << MessageID::GameDigestError;
    packet << "file not found";
    Send(packet);
    return;
  }

  if (m_game_digest_thread.joinable())
    m_game_digest_thread.join();
  m_game_digest_thread = std::thread([this, file] {
    std::string sum = SHA1Sum(file, [&](int progress) {
      sf::Packet packet;
      packet << MessageID::GameDigestProgress;
      packet << progress;
      SendAsync(std::move(packet));

      return m_should_compute_game_digest;
    });

    sf::Packet packet;
    packet << MessageID::GameDigestResult;
    packet << sum;
    SendAsync(std::move(packet));
  });
}

const PadMappingArray& NetPlayClient::GetPadMapping() const
{
  return m_net_settings.pad_map;
}

const GBAConfigArray& NetPlayClient::GetGBAConfig() const
{
  return m_net_settings.gba_config;
}

const PadMappingArray& NetPlayClient::GetWiimoteMapping() const
{
  return m_net_settings.wiimote_map;
}

void NetPlayClient::AdjustPadBufferSize(const unsigned int size)
{
  m_target_buffer_size = size;
  m_dialog->OnPadBufferChanged(size);
}

void NetPlayClient::SetWiiSyncData(std::unique_ptr<IOS::HLE::FS::FileSystem> fs,
                                   std::vector<u64> titles, std::string redirect_folder)
{
  m_wii_sync_fs = std::move(fs);
  m_wii_sync_titles = std::move(titles);
  m_wii_sync_redirect_folder = std::move(redirect_folder);
}

SyncIdentifier NetPlayClient::GetSDCardIdentifier()
{
  return SyncIdentifier{{}, "sd", {}, {}, {}, {}};
}

std::string GetPlayerMappingString(PlayerId pid, const PadMappingArray& pad_map,
                                   const GBAConfigArray& gba_config,
                                   const PadMappingArray& wiimote_map)
{
  std::vector<size_t> gc_slots, gba_slots, wiimote_slots;
  for (size_t i = 0; i < pad_map.size(); ++i)
  {
    if (pad_map[i] == pid && !gba_config[i].enabled)
      gc_slots.push_back(i + 1);
    if (pad_map[i] == pid && gba_config[i].enabled)
      gba_slots.push_back(i + 1);
    if (wiimote_map[i] == pid)
      wiimote_slots.push_back(i + 1);
  }
  std::vector<std::string> groups;
  std::array<std::pair<std::string, std::vector<size_t>*>, 3> slot_groups = {
      {{"GC", &gc_slots}, {"GBA", &gba_slots}, {"Wii", &wiimote_slots}}};

  for (const auto& [group_name, slots] : slot_groups)
  {
    if (!slots->empty())
      groups.emplace_back(fmt::format("{}{}", group_name, fmt::join(*slots, ",")));
  }
  std::string res = fmt::format("{}", fmt::join(groups, "|"));
  return res.empty() ? "None" : res;
}

bool IsNetPlayRunning()
{
  return netplay_client != nullptr;
}

void SetSIPollBatching(bool state)
{
  s_si_poll_batching = state;
}

void SendPowerButtonEvent()
{
  ASSERT(IsNetPlayRunning());
  netplay_client->SendPowerButtonEvent();
}

std::string GetGBASavePath(int pad_num)
{
  std::lock_guard lk(crit_netplay_client);

  if (!netplay_client || netplay_client->GetNetSettings().is_hosting)
  {
#ifdef HAS_LIBMGBA
    std::string rom_path = Config::Get(Config::MAIN_GBA_ROM_PATHS[pad_num]);
    return HW::GBA::Core::GetSavePath(rom_path, pad_num);
#else
    return {};
#endif
  }

  if (!netplay_client->GetNetSettings().savedata_load)
    return {};

  return fmt::format("{}{}{}.sav", File::GetUserPath(D_GBAUSER_IDX), GBA_SAVE_NETPLAY, pad_num + 1);
}

PadDetails GetPadDetails(int pad_num)
{
  std::lock_guard lk(crit_netplay_client);

  PadDetails res{};
  res.local_pad = 4;
  if (!netplay_client)
    return res;

  auto pad_map = netplay_client->GetPadMapping();
  if (pad_map[pad_num] <= 0)
    return res;

  for (auto player : netplay_client->GetPlayers())
  {
    if (player->pid == pad_map[pad_num])
      res.player_name = player->name;
  }

  int local_pad = 0;
  int non_local_pad = 0;
  for (int i = 0; i < pad_num; ++i)
  {
    if (netplay_client->IsLocalPlayer(pad_map[i]))
      ++local_pad;
    else
      ++non_local_pad;
  }
  res.is_local = netplay_client->IsLocalPlayer(pad_map[pad_num]);
  res.local_pad = res.is_local ? local_pad : netplay_client->NumLocalPads() + non_local_pad;
  res.hide_gba = !res.is_local && netplay_client->GetNetSettings().hide_remote_gbas &&
                 netplay_client->LocalPlayerHasControllerMapped();
  return res;
}

int NumLocalWiimotes()
{
  std::lock_guard lk(crit_netplay_client);
  if (netplay_client)
    return netplay_client->NumLocalWiimotes();
  return 0;
}

void NetPlay_Enable(NetPlayClient* const np)
{
  std::lock_guard lk(crit_netplay_client);
  netplay_client = np;
}

void NetPlay_Disable()
{
  std::lock_guard lk(crit_netplay_client);
  netplay_client = nullptr;
}
}  // namespace NetPlay

// stuff hacked into dolphin

// called from ---CPU--- thread
// Actual Core function which is called on every frame
bool SerialInterface::CSIDevice_GCController::NetPlay_GetInput(int pad_num, GCPadStatus* status)
{
  std::lock_guard lk(NetPlay::crit_netplay_client);

  if (NetPlay::netplay_client)
    return NetPlay::netplay_client->GetNetPads(pad_num, NetPlay::s_si_poll_batching, status);

  return false;
}

bool NetPlay::NetPlay_GetWiimoteData(const std::span<NetPlayClient::WiimoteDataBatchEntry>& entries)
{
  std::lock_guard lk(crit_netplay_client);

  if (netplay_client)
    return netplay_client->WiimoteUpdate(entries);

  return false;
}

unsigned int NetPlay::NetPlay_GetLocalWiimoteForSlot(unsigned int slot)
{
  if (slot >= std::tuple_size_v<PadMappingArray>)
    return slot;

  std::lock_guard lk(crit_netplay_client);

  if (!netplay_client)
    return slot;

  const auto& mapping = netplay_client->GetWiimoteMapping();
  const auto& local_player_id = netplay_client->GetLocalPlayerId();

  std::array<unsigned int, std::tuple_size_v<std::decay_t<decltype(mapping)>>> slot_map;
  size_t player_count = 0;
  for (size_t i = 0; i < mapping.size(); ++i)
  {
    if (mapping[i] == local_player_id)
    {
      slot_map[i] = static_cast<unsigned int>(player_count);
      ++player_count;
    }
  }
  for (size_t i = 0; i < mapping.size(); ++i)
  {
    if (mapping[i] != local_player_id)
    {
      slot_map[i] = static_cast<unsigned int>(player_count);
      ++player_count;
    }
  }

  INFO_LOG_FMT(NETPLAY, "Wiimote slot map: [{}]", fmt::join(slot_map, ", "));

  return slot_map[slot];
}

// called from ---CPU--- thread
// so all players' games get the same time
//
// also called from ---GUI--- thread when starting input recording
u64 ExpansionInterface::CEXIIPL::NetPlay_GetEmulatedTime()
{
  std::lock_guard lk(NetPlay::crit_netplay_client);

  if (NetPlay::netplay_client)
    return NetPlay::netplay_client->GetInitialRTCValue();

  return 0;
}

// called from ---CPU--- thread
// return the local pad num that should rumble given a ingame pad num
int SerialInterface::CSIDevice_GCController::NetPlay_InGamePadToLocalPad(int pad_num)
{
  std::lock_guard lk(NetPlay::crit_netplay_client);

  if (NetPlay::netplay_client)
    return NetPlay::netplay_client->InGamePadToLocalPad(pad_num);

  return pad_num;
}
