// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <SFML/Network/Packet.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/SPSCQueue.h"
#include "Common/TraversalClient.h"
#include "Core/HLE/HLE_XD.h"
#include "Core/NetPlayProto.h"
#include "Core/SyncIdentifier.h"
#include "InputCommon/GCPadStatus.h"

class BootSessionData;

namespace IOS::HLE::FS
{
class FileSystem;
}

namespace UICommon
{
class GameFile;
}

namespace WiimoteEmu
{
struct SerializedWiimoteState;
}

namespace NetPlay
{
// XD Netplay: a player's part in the next battle, from the server's XdSeats broadcast. Unknown
// until the first broadcast arrives.
enum class XdRole : u8
{
  Unknown = 0,
  Host = 1,
  Opponent = 2,
  Waiting = 3,
  Watching = 4,
  // A Multi room: holds Seat 2, 3 or 4 (XdSeatInfo::seat). The host is Seat 1.
  Seated = 5,
};

// XD Netplay: a player's seat from the last XdSeats. seat: 1 for the host in a Multi room, 2-4 for
// a seated joiner there, 3 for the 1v1 opponent, 2 or 3 (SI port; GBA 1 or GBA 2) for a joiner
// seated in a 1v1 room whose host watches (role Seated), 0 otherwise. team_in: the seat has a team
// ready. watching: the host of a 1v1 room ticked Watch only.
struct XdSeatInfo
{
  XdRole role = XdRole::Unknown;
  u8 seat = 0;
  bool team_in = false;
  bool watching = false;
};

// XD Netplay, host side: where a joiner's TeamData goes. 1v1: the GBA 2 slot (device 2,
// stage_serial 0). Multi: a private stage file for that connection (stage_serial), later copied
// into the boot save of SI port device + 1.
struct XdTeamTarget
{
  int device = 2;
  u32 stage_serial = 0;
};
struct XdTeamResult
{
  bool applied = false;
  std::string line;  // for the room chat: "<name> submitted a team: <line>"
};

class NetPlayUI
{
public:
  virtual ~NetPlayUI() {}
  virtual void BootGame(const std::string& filename,
                        std::unique_ptr<BootSessionData> boot_session_data) = 0;
  virtual void StopGame() = 0;
  virtual bool IsHosting() const = 0;

  virtual void Update() = 0;
  virtual void AppendChat(const std::string& msg) = 0;
  // A room chat line that is never drawn over the game. XD Netplay's in-battle notices (out of
  // sync, a player left the game window, a GBA that did not start) use it, because the fork keeps
  // on-screen text off during the GBA link. Android's AppendChat is already chat-only, so that is
  // the default.
  virtual void AppendChatQuiet(const std::string& msg) { AppendChat(msg); }
  // XD Netplay, host side: a joiner submitted a Showdown team. The
  // implementation writes it where target says (the GBA 2 slot in 1v1, the
  // player's stage file in Multi; UICommon/XDNetplay/MultiStart.h
  // HandleTeamSubmission -- Core cannot call uicommon directly, hence the hop
  // through the UI layer) and returns whether it landed plus a one-line result
  // for the room chat. Called on the NETPLAY thread under the server's seat
  // mutex; must finish before the ack, so the file is on disk before any Start
  // can read it.
  virtual XdTeamResult OnTeamSubmission(const std::string& player, const std::string& text,
                                        const XdTeamTarget& target) = 0;
  // XD Netplay, host side: the GBA 2 slot holds the team of a player who no longer holds the seat.
  // Put the host's spare team back and forget that player's model pick
  // (UICommon/XDNetplay/TeamInjector.h, ResetGuestSlot). Called on the NETPLAY thread (TeamData)
  // or the host UI thread (Start) with the server's seat mutex held, so it must not touch widgets
  // or wait on a UI thread. Returns false and changes nothing while emulation is not
  // Uninitialized.
  virtual bool OnXdGuestSlotReset() = 0;
  // XD Netplay: this machine's session is over. Puts the host's own team back
  // if a guest's submission overwrote it, and erases every remaining file that
  // holds the opponent's party -- including, on a joiner, netplay's own
  // NetPlayTemp GBA saves (UICommon/XDNetplay/TeamInjector.h).
  //
  // Raised from BOTH ~NetPlayServer (host) and ~NetPlayClient (either end), so
  // hosts see it twice; the implementation is idempotent. It may complete
  // asynchronously: while a battle is live the mGBA core owns the save files,
  // so the work waits for emulation to reach Uninitialized.
  virtual void OnRoomClosed() = 0;

  virtual void OnMsgChangeGame(const SyncIdentifier& sync_identifier,
                               const std::string& netplay_name) = 0;
  virtual void OnMsgChangeGBARom(int pad, const NetPlay::GBAConfig& config) = 0;
  virtual void OnMsgStartGame() = 0;
  virtual void OnMsgStopGame() = 0;
  virtual void OnMsgPowerButton() = 0;
  virtual void OnPlayerConnect(const std::string& player) = 0;
  virtual void OnPlayerDisconnect(const std::string& player) = 0;
  virtual void OnPadBufferChanged(u32 buffer) = 0;
  virtual void OnHostInputAuthorityChanged(bool enabled) = 0;
  virtual void OnDesync(u32 frame, const std::string& player) = 0;
  virtual void OnConnectionLost() = 0;
  virtual void OnConnectionError(const std::string& message) = 0;
  virtual void OnTraversalError(Common::TraversalClient::FailureReason error) = 0;
  virtual void OnTraversalStateChanged(Common::TraversalClient::State state) = 0;
  virtual void OnGameStartAborted() = 0;
  virtual void OnGolferChanged(bool is_golfer, const std::string& golfer_name) = 0;
  virtual void OnTtlDetermined(u8 ttl) = 0;

  virtual bool IsRecording() = 0;
  virtual std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const SyncIdentifier& sync_identifier,
               SyncIdentifierComparison* found = nullptr) = 0;
  virtual std::string FindGBARomPath(const std::array<u8, 20>& hash, std::string_view title,
                                     int device_number) = 0;
  virtual void ShowGameDigestDialog(const std::string& title) = 0;
  virtual void SetGameDigestProgress(int pid, int progress) = 0;
  virtual void SetGameDigestResult(int pid, const std::string& result) = 0;
  virtual void AbortGameDigest() = 0;

  virtual void OnIndexAdded(bool success, std::string error) = 0;
  virtual void OnIndexRefreshFailed(std::string error) = 0;

  virtual void ShowChunkedProgressDialog(const std::string& title, u64 data_size,
                                         std::span<const int> players) = 0;
  virtual void HideChunkedProgressDialog() = 0;
  virtual void SetChunkedProgress(int pid, u64 progress) = 0;

  virtual void SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder) = 0;
};

class Player
{
public:
  PlayerId pid{};
  std::string name;
  std::string revision;
  u32 ping = 0;
  SyncIdentifierComparison game_status = SyncIdentifierComparison::Unknown;
  // XD Netplay: this player ticked Watch only (from XdSeats).
  bool xd_watching = false;

  bool IsHost() const { return pid == 1; }
};

class NetPlayClient : public Common::TraversalClientClient
{
public:
  void ThreadFunc();
  void SendAsync(sf::Packet&& packet, u8 channel_id = DEFAULT_CHANNEL);

  NetPlayClient(const std::string& address, const u16 port, NetPlayUI* dialog, std::string name,
                const NetTraversalConfig& traversal_config);
  ~NetPlayClient() override;

  std::vector<const Player*> GetPlayers();
  const NetSettings& GetNetSettings() const;

  // Called from the GUI thread.
  bool IsConnected() const { return m_is_connected; }
  bool StartGame(const std::string& path);
  void InvokeStop();
  bool StopGame();
  void Stop();
  bool ChangeGame(const std::string& game);
  void SendChatMessage(const std::string& msg);
  // XD Netplay: submit this player's own team -- and, optionally, the in-game
  // trainer name to play under -- to the host, which writes both into the save
  // it syncs at start. No-op unless the host is running this fork's XD flow.
  // The payload is built by XDNetplay::BuildTeamSubmissionPayload and Core
  // treats it as opaque text. See UICommon/XDNetplay/TeamInjector.h.
  void SendTeamSubmission(const std::string& payload);
  // XD Netplay: this player only watches (true) or wants to play (false). Takes effect at the
  // next Start; the server answers with XdSeats.
  void SendXdWatch(bool watching);
  // XD Netplay: pid's part in the next battle, from the last XdSeats. Display only; the pad map
  // is decided by the host at Start. Takes m_crit.players.
  XdRole GetXdRole(PlayerId pid);
  XdSeatInfo GetXdSeatInfo(PlayerId pid);
  // The last XdSeats was a Multi room's. Takes m_crit.players.
  bool IsXdMultiSeats();
  // The last XdSeats was a 1v1 room's whose host watches (joiners play GBA 1 and GBA 2). Takes
  // m_crit.players.
  bool IsXdHostWatching();
  // XD Netplay: the room's battle format (a FormatRules id) from the server's last XdFormat, or
  // nullopt before the first one. Display only. Takes m_crit.players.
  std::optional<int> GetXdRoomFormat();
  void RequestStopGame();
  void SendPowerButtonEvent();
  void RequestGolfControl(PlayerId pid);
  void RequestGolfControl();
  std::string GetCurrentGolfer();

  // Send and receive pads values
  struct WiimoteDataBatchEntry
  {
    int wiimote;
    WiimoteEmu::SerializedWiimoteState* state;
  };
  bool WiimoteUpdate(const std::span<WiimoteDataBatchEntry>& entries);
  bool GetNetPads(int pad_nb, bool from_vi, GCPadStatus* pad_status);

  u64 GetInitialRTCValue() const;

  void OnTraversalStateChanged() override;
  void OnConnectReady(ENetAddress addr) override;
  void OnConnectFailed(Common::TraversalConnectFailedReason reason) override;
  void OnTtlDetermined(u8 ttl) override {}

  bool IsFirstInGamePad(int ingame_pad) const;
  int NumLocalPads() const;
  int NumLocalWiimotes() const;

  int InGamePadToLocalPad(int ingame_pad) const;
  int LocalPadToInGamePad(int local_pad) const;
  int InGameWiimoteToLocalWiimote(int ingame_wiimote) const;
  int LocalWiimoteToInGameWiimote(int local_wiimote) const;

  bool PlayerHasControllerMapped(PlayerId pid) const;
  bool LocalPlayerHasControllerMapped() const;
  // True when the calling thread belongs to the core that was booted for the current game. A core
  // that is still shutting down from the previous game must not touch this game's input.
  bool IsCurrentGameCore() const;
  // Called by the input hook each time it turns a previous game's core away.
  void CountStaleCorePoll() { m_stale_core_polls.fetch_add(1, std::memory_order_relaxed); }

  // XD Netplay, host only: change the running game's battle style (music and location). ops are
  // Action Replay write lines as (address word, value) pairs, the full live set; each request
  // replaces the previous one. It is applied at the same emulated moment on every machine: the
  // host's CPU thread numbers it with the index of the next entry it pushes for the first pad it
  // owns, sends it ahead of that entry on the same ordered channel, and every machine installs it
  // right before popping that entry. Returns false with a one-line reason when it cannot be
  // scheduled. Safe from any thread; takes only m_live_mutex.
  bool RequestLiveStyle(std::vector<std::pair<u32, u32>> ops, std::string* reason);
  bool IsLocalPlayer(PlayerId pid) const;
  const PlayerId& GetLocalPlayerId() const;

  static void SendTimeBase();
  bool DoAllPlayersHaveGame();

  // XD Netplay state check (v1.7.5), two parts with their own tags. Inputs: every
  // STATE_SAMPLE_POPS pops of the first mapped pad, a hash of the pad entries this machine
  // consumed since the last report (every pad, in pop order, as they travel on the wire), tagged
  // with that pop count; pops only happen in the SI poll, in channel order, so the windows are
  // identical everywhere. XD state: HLE_XD::ComputeXdDigest read at every STATE_SAMPLE_WRITES-th
  // GC->GBA WRITE of a GBA port, tagged with that WRITE count. A pad pop is the same emulated
  // tick on every machine but not the same instruction when the CPUs split JIT blocks
  // differently, so XD's memory is read at the WRITE, an MMIO store in the game's own code (the
  // point the 'xd' log lines have always matched at across x86 and ARM). Warn only: nothing here
  // changes what any machine emulates.
  struct StateSample
  {
    u32 tag = 0;
    u32 inputs = 0;
  };
  // ---CPU--- thread, under crit_netplay_client.
  std::optional<StateSample> TakeStateSampleDue();
  void SendInputsSample(const StateSample& sample);
  void SendXdStateSample(int port, u32 write_count, const HLE_XD::XdDigest& xd);
  // A GBA core of this game did not start on this machine (SI port 0-3). The emulation thread,
  // under crit_netplay_client (NetPlay::ReportGbaStartFailure).
  void OnLocalGbaStartFailed(int port);
  // A Multi battle started on this machine, the port-1 owner (NetPlay::ReportXdMultiSides).
  void OnLocalMultiSides(u8 packed);

  const PadMappingArray& GetPadMapping() const;
  const GBAConfigArray& GetGBAConfig() const;
  const PadMappingArray& GetWiimoteMapping() const;

  void AdjustPadBufferSize(unsigned int size);

  void SetWiiSyncData(std::unique_ptr<IOS::HLE::FS::FileSystem> fs, std::vector<u64> titles,
                      std::string redirect_folder);

  static SyncIdentifier GetSDCardIdentifier();

protected:
  struct AsyncQueueEntry
  {
    sf::Packet packet;
    u8 channel_id = 0;
  };

  void ClearBuffers();

  struct
  {
    std::recursive_mutex game;
    // lock order
    std::recursive_mutex players;
    std::recursive_mutex async_queue_write;
  } m_crit;

  Common::SPSCQueue<AsyncQueueEntry> m_async_queue;

  std::array<Common::SPSCQueue<GCPadStatus>, 4> m_pad_buffer;
  std::array<Common::SPSCQueue<WiimoteEmu::SerializedWiimoteState>, 4> m_wiimote_buffer;

  std::array<GCPadStatus, 4> m_last_pad_status{};
  std::array<bool, 4> m_first_pad_status_received{};
  // Set by OnStartGame (netplay thread) once it has emptied the pad queues for the game it is
  // announcing. StartGame() on the GUI thread only asserts on it: it must never clear them a
  // second time, because the netplay thread is pushing into those queues by then. See the
  // comments in OnStartGame and StartGame.
  // Atomic: written on the netplay thread, read on the GUI thread that boots the game.
  std::atomic<bool> m_input_reset_for_game{false};
  // Core::GetBootSequence() of the core this game will run on, recorded in StartGame before
  // netplay input is enabled. StartGame always runs before that core's Core::Init, whether or not
  // the previous game's core is still alive, and Init is what moves the sequence, so it is always
  // the current sequence + 1. See IsCurrentGameCore().
  std::atomic<u64> m_game_boot_sequence{0};
  // Polls turned away from a previous game's core in this game, reported on the padpop lines.
  std::atomic<u32> m_stale_core_polls{0};

  // XD Netplay live battle style (RequestLiveStyle). m_live_mutex is a leaf lock: nothing is ever
  // locked while holding it. The counters are CPU-thread only, reset with the pad queues in
  // OnStartGame; because every machine pushes and pops each pad's entries in the same order, pop
  // number K of a pad is the same emulated poll everywhere.
  std::mutex m_live_mutex;
  std::optional<std::vector<std::pair<u32, u32>>> m_live_request;          // host, not yet numbered
  std::map<u32, std::vector<std::pair<u32, u32>>> m_live_pending;          // pop index -> ops
  std::array<u32, 4> m_live_push_count{};
  std::array<u32, 4> m_live_pop_count{};
  // The marker pad's pop count, readable from the netplay thread for the LATE log line only.
  std::atomic<u32> m_live_marker_pops{0};
  // The first in-game pad the host owns, in a Pokemon XD room; the pad whose entries number live
  // changes. -1 turns live changes off for the game. Fixed at OnStartGame, the same everywhere.
  // Atomic: written on the netplay thread, read on the CPU thread and in RequestLiveStyle.
  std::atomic<int> m_live_marker_pad{-1};
  // One "padpop" diagnostic line per pad per game: how deep that pad's queue was at its first pop.
  std::array<bool, 4> m_first_pop_logged{};
  // How often the room's game polls each pad, for converting the spectator reserve between ms
  // and entries: 120 in Pokemon XD, 60 (once a frame) otherwise. Set at OnStartGame from the
  // room's game, which is identical everywhere; atomic because the CPU thread reads it.
  std::atomic<u32> m_spec_polls_per_sec{60};
  // XD Netplay spectator playout reserve (GetNetPads). CPU-thread only, never touched from the
  // netplay thread: it is rebuilt at the first pop of each game, which the CPU thread spots by
  // m_game_boot_sequence no longer matching m_spec_game_seq. m_spec_active is decided there once
  // per game; m_spec_reserve is how many entries a filling pad's queue must hold before it pops.
  // m_spec_after_pop is a pad's queue depth just after our last pop and m_spec_last_growth when we
  // last saw it grow, which together time the gap in delivery behind an underflow.
  u64 m_spec_game_seq = UINT64_MAX;
  bool m_spec_active = false;
  u32 m_spec_rate = 60;
  u32 m_spec_reserve = 0;
  std::array<bool, 4> m_spec_filling{};
  std::array<size_t, 4> m_spec_after_pop{};
  std::array<std::chrono::steady_clock::time_point, 4> m_spec_last_growth{};

  std::chrono::time_point<std::chrono::steady_clock> m_buffer_under_target_last;

  NetPlayUI* m_dialog = nullptr;

  ENetHost* m_client = nullptr;
  ENetPeer* m_server = nullptr;
  std::thread m_thread;

  SyncIdentifier m_selected_game;
  Common::Flag m_is_running{false};
  Common::Flag m_do_loop{true};

  // In non-host input authority mode, this is how many packets each client should
  // try to keep in-flight to the other clients. In host input authority mode, this is how
  // many incoming input packets need to be queued up before the client starts
  // speeding up the game to drain the buffer.
  unsigned int m_target_buffer_size = 20;
  bool m_host_input_authority = false;

  // XD latency instrumentation (measurement only -- no protocol, no wire format,
  // no emulation state; times how long the CPU thread stalls waiting on a remote
  // pad frame in GetNetPads, which is the netplay input latency itself).
  double m_lat_wait_ewma_us = 0.0;
  u64 m_lat_wait_max_us = 0;
  u32 m_lat_starve_pops = 0;
  u32 m_lat_pops = 0;
  u32 m_lat_bucket[7] = {};
  u64 m_lat_last_emit_us = 0;
  PlayerId m_current_golfer = 1;

  // This bool will stall the client at the start of GetNetPads, used for switching input control
  // without deadlocking. Use the correspondingly named Event to wake it up.
  bool m_wait_on_input;
  bool m_wait_on_input_received;

  Player* m_local_player = nullptr;

  u32 m_current_game = 0;

  bool m_is_recording = false;

private:
  enum class ConnectionState
  {
    WaitingForTraversalClientConnection,
    WaitingForTraversalClientConnectReady,
    Connecting,
    WaitingForHelloResponse,
    Connected,
    Failure
  };

  void SendStartGamePacket();
  void SendStopGamePacket();

  void SyncSaveDataResponse(bool success);
  void SyncCodeResponse(bool success);

  bool PollLocalPad(int local_pad, sf::Packet& packet);
  void SendPadHostPoll(PadIndex pad_num);

  // XD netplay peer-vanish watchdog. Per-wait bookkeeping for WaitOnRemote();
  // one of these lives on the stack of whichever CPU-thread loop is blocked.
  struct RemoteWaitState
  {
    std::chrono::steady_clock::time_point started{};
    u64 next_notice_ms = 0;
    bool started_valid = false;
    bool silence_notice = false;
  };

  // Why a session stopped by the watchdog stopped. It decides how loudly we say
  // so, which matters because one of these is an ordinary, user-requested event
  // and the other is a genuine failure.
  enum class SessionEndKind : u32
  {
    // Session is healthy. Also the "nobody has claimed the teardown yet" state.
    None = 0,
    // A peer or the host vanished and nothing is going to bring the session
    // back. The user did not ask for this and needs to be told why their battle
    // just ended: red OSD, error log, a line in the room chat.
    PeerLost,
    // The local user asked to stop and we finished the job ourselves rather than
    // keep waiting on an acknowledgement. Entirely ordinary -- the session
    // stopped exactly as requested -- so it gets a log line and nothing else.
    LocalStopCompleted,
  };

  bool WaitOnRemote(Common::Event& wait_event, int pad_nb, RemoteWaitState& state);
  void DeclareSessionLost(SessionEndKind kind, const std::string& reason);
  std::string DescribePadOwner(int pad_nb);
  bool PadOwnerHasLeftRoom(int pad_nb);
  u32 LocalPingToHost();

  bool AddLocalWiimoteToBuffer(int local_wiimote, const WiimoteEmu::SerializedWiimoteState& state,
                               sf::Packet& packet);

  void AddPadStateToPacket(int in_game_pad, const GCPadStatus& np, sf::Packet& packet);
  void AddWiimoteStateToPacket(int in_game_pad, const WiimoteEmu::SerializedWiimoteState& np,
                               sf::Packet& packet);
  void Send(const sf::Packet& packet, u8 channel_id = DEFAULT_CHANNEL);
  void Disconnect();
  bool Connect();
  void SendGameStatus();
  void ComputeGameDigest(const SyncIdentifier& sync_identifier);
  void DisplayPlayersPing();
  u32 GetPlayersMaxPing() const;

  void OnData(sf::Packet& packet);
  void OnPlayerJoin(sf::Packet& packet);
  void OnPlayerLeave(sf::Packet& packet);
  void OnChatMessage(sf::Packet& packet);
  void OnChunkedDataStart(sf::Packet& packet);
  void OnChunkedDataEnd(sf::Packet& packet);
  void OnChunkedDataPayload(sf::Packet& packet);
  void OnChunkedDataAbort(sf::Packet& packet);
  void OnPadMapping(sf::Packet& packet);
  void OnWiimoteMapping(sf::Packet& packet);
  void OnGBAConfig(sf::Packet& packet);
  void OnPadData(sf::Packet& packet);
  void OnPadHostData(sf::Packet& packet);
  void OnWiimoteData(sf::Packet& packet);
  void OnPadBuffer(sf::Packet& packet);
  void OnHostInputAuthority(sf::Packet& packet);
  void OnGolfSwitch(sf::Packet& packet);
  void OnGolfPrepare(sf::Packet& packet);
  void OnChangeGame(sf::Packet& packet);
  void OnGameStatus(sf::Packet& packet);
  void OnStartGame(sf::Packet& packet);
  void OnStopGame(sf::Packet& packet);
  void OnPowerButton();
  void OnLiveStyle(sf::Packet& packet);
  void OnPing(sf::Packet& packet);
  void OnPlayerPingData(sf::Packet& packet);
  void OnDesyncDetected(sf::Packet& packet);
  void OnSyncSaveData(sf::Packet& packet);
  void OnSyncSaveDataNotify(sf::Packet& packet);
  void OnSyncSaveDataRaw(sf::Packet& packet);
  void OnSyncSaveDataGCI(sf::Packet& packet);
  void OnSyncSaveDataWii(sf::Packet& packet);
  void OnSyncSaveDataGBA(sf::Packet& packet);
  void OnSyncCodes(sf::Packet& packet);
  void OnSyncCodesNotify();
  void OnSyncCodesNotifyGecko(sf::Packet& packet);
  void OnSyncCodesDataGecko(sf::Packet& packet);
  void OnSyncCodesNotifyAR(sf::Packet& packet);
  void OnSyncCodesDataAR(sf::Packet& packet);
  void OnComputeGameDigest(sf::Packet& packet);
  void OnGameDigestProgress(sf::Packet& packet);
  void OnGameDigestResult(sf::Packet& packet);
  void OnGameDigestError(sf::Packet& packet);
  void OnGameDigestAbort();
  void OnXdSeats(sf::Packet& packet);
  void OnXdFormat(sf::Packet& packet);
  void OnXdNotice(sf::Packet& packet);
  void OnStateMismatch(sf::Packet& packet);
  // ---NETPLAY--- thread, every loop: the "left the game window" notice.
  void UpdateFocusNotice();
  void SendXdNotice(XdNoticeKind kind, u8 arg);

  bool m_is_connected = false;
  ConnectionState m_connection_state = ConnectionState::Failure;

  PlayerId m_pid = 0;
  NetSettings m_net_settings{};
  std::map<PlayerId, Player> m_players;
  // XD Netplay seats from the last XdSeats, guarded by m_crit.players: the holders of SI ports 2,
  // 3 and 4 (a 1v1 room uses only port 3), whether the room is a Multi room, and which seats
  // have a team in.
  std::array<PlayerId, 3> m_xd_seats{};
  bool m_xd_multi = false;
  bool m_xd_host_watching = false;
  u8 m_xd_team_bits = 0;
  bool m_xd_seats_known = false;
  // XD Netplay: the room's format from the last XdFormat, guarded by m_crit.players.
  std::optional<int> m_xd_room_format;
  std::string m_host_spec;
  std::string m_player_name;
  bool m_connecting = false;
  // OnConnectFailed showed a reason-specific error; the ctor's generic fallback must stay quiet.
  bool m_specific_connect_error = false;
  Common::TraversalClient* m_traversal_client = nullptr;
  std::thread m_game_digest_thread;
  bool m_should_compute_game_digest = false;
  Common::Event m_gc_pad_event;
  Common::Event m_wii_pad_event;
  Common::Event m_first_pad_status_received_event;
  Common::Event m_wait_on_input_event;

  // XD netplay peer-vanish watchdog.
  //
  // m_last_recv_ms is stamped by the NETPLAY thread every time a netplay packet
  // lands from the server. The server pings every client once a second while a
  // room is open, so on a live session this is never more than ~1 s stale no
  // matter how badly the *game* is starved. That is what lets the CPU thread's
  // pad-wait loop tell "the other player's console is briefly behind" (server
  // still pinging -- keep waiting) apart from "the machine we talk to is gone"
  // (dead silence -- stop pretending this session exists). Steady clock, ms.
  std::atomic<u64> m_last_recv_ms{0};
  // Claimed exactly once, by whichever thread first concludes this session is
  // over, via compare-exchange from None. It carries both facts -- that somebody
  // won, and what they concluded -- in a single atomic so the winner's reason
  // can never be overwritten by a loser and can never be read before it is
  // published. The NETPLAY thread does the actual teardown; see
  // DeclareSessionLost() for why the CPU thread must not run StopGame() itself.
  std::atomic<SessionEndKind> m_session_end{SessionEndKind::None};
  // Stamped when RequestStopGame() mails a stop to the server, and cleared by
  // InvokeStop(), i.e. the instant any real stop lands by any route. If no stop
  // lands within LOCAL_STOP_GRACE we stop ourselves rather than leave the CPU
  // thread wedged behind a Stop the user already asked for -- the server is
  // quite often the machine that just vanished. Clearing it in InvokeStop() is
  // what keeps an ordinary shutdown from looking like a failure; see there.
  // 0 means "no local stop pending".
  std::atomic<u64> m_stop_requested_ms{0};

  u8 m_sync_save_data_count = 0;
  u8 m_sync_save_data_success_count = 0;
  u16 m_sync_gecko_codes_count = 0;
  u16 m_sync_gecko_codes_success_count = 0;
  bool m_sync_gecko_codes_complete = false;
  u16 m_sync_ar_codes_count = 0;
  u16 m_sync_ar_codes_success_count = 0;
  bool m_sync_ar_codes_complete = false;
  std::unordered_map<u32, sf::Packet> m_chunked_data_receive_queue;

  u64 m_initial_rtc = 0;
  u32 m_timebase_frame = 0;

  // XD Netplay state check (TakeStateSampleDue). m_sd_marker_pad is the pad whose pop count tags
  // the reports, fixed at OnStartGame: the first mapped pad in a Pokemon XD room without host input
  // authority, -1 otherwise (no reports). m_sd_inputs and m_sd_due are CPU-thread state, reset in
  // OnStartGame for the same reason the live-style counters are.
  std::atomic<int> m_sd_marker_pad{-1};
  u32 m_sd_inputs = 0;
  std::optional<StateSample> m_sd_due;
  u32 m_sd_lines = 0;
  // This machine's wall clock at its recent reports, keyed (kind << 32 | tag), for the "since
  // HH:MM:SS" in a mismatch line. Leaf lock: nothing is taken while holding it.
  std::mutex m_sd_time_mutex;
  std::array<std::pair<u64, std::string>, 64> m_sd_times{};
  size_t m_sd_time_next = 0;
  // ---NETPLAY--- thread: this game's players' mismatch line was shown. m_sd_warned_since: the
  // time in the line this machine showed (players', or its own spectator view's), repeated once
  // in the room chat when the game stops (the line is easy to miss during a battle).
  bool m_sd_warned = false;
  std::string m_sd_warned_since;
  bool m_sd_warned_view_only = false;

  // Set by OnPadBuffer when the buffer is raised; the CPU thread then re-anchors the throttle at
  // its next pop, so a machine that had fallen behind its pacing deadline keeps the new slack
  // instead of sprinting through it. Wall-clock pacing only.
  std::atomic<bool> m_throttle_reanchor{false};
  // A player reports its pad waits once a second (MessageID::PadHealth) for the automatic buffer.
  // The counters cover the pads this machine does not own; CPU thread only.
  std::atomic<bool> m_send_pad_health{false};
  u64 m_health_wait_max_us = 0;
  u32 m_health_starve_pops = 0;
  u32 m_health_pops = 0;
  u32 m_health_min_depth = 0xFFFFFFFF;

  // XD multi battles (m_net_settings.xd_multi_p1), CPU thread only, reset in OnStartGame.
  // m_xdm_reset_pending: the port-1 GBA window's Reset, read by a poll that pushed nothing yet.
  // Edge-safe lower: the depth each pad's pushes keep (a raise applies at once, a lower steps by
  // one only on a poll whose sample repeats the last one pushed, sticks and triggers within 2, so
  // no press or release is ever dropped) and that last sample (without the control bits). Chase re-anchor: which of the last
  // 120 pops of other machines' pads waited, and when the throttle was last re-anchored for it.
  bool m_xdm_reset_pending = false;
  std::array<u32, 4> m_push_target{};
  std::array<std::optional<GCPadStatus>, 4> m_last_pushed{};
  std::array<bool, 120> m_chase_waited{};
  u32 m_chase_next = 0;
  u32 m_chase_count = 0;
  std::chrono::steady_clock::time_point m_chase_last_reanchor{};

  // "Left the game window" notice (UpdateFocusNotice). m_focus_watch: this machine plays a pad in
  // the running XD game. m_focus_gate_off_since_ms: when Dolphin's input gate closed (desktop: the
  // game window lost focus with Background Input off), 0 while open; written on the CPU thread.
  // m_focus_paused_since_ms: when the netplay thread first saw this machine's core paused (Pause
  // on Focus Loss, the pause button, Android leaving the game screen), 0 while running. The rest
  // is netplay-thread state.
  std::atomic<bool> m_focus_watch{false};
  std::atomic<u64> m_focus_gate_off_since_ms{0};
  u64 m_focus_paused_since_ms = 0;
  bool m_focus_left_sent = false;
  u64 m_focus_last_left_ms = 0;

  std::unique_ptr<IOS::HLE::FS::FileSystem> m_wii_sync_fs;
  std::vector<u64> m_wii_sync_titles;
  std::string m_wii_sync_redirect_folder;
};

void NetPlay_Enable(NetPlayClient* const np);
void NetPlay_Disable();
bool NetPlay_GetWiimoteData(const std::span<NetPlayClient::WiimoteDataBatchEntry>& entries);
unsigned int NetPlay_GetLocalWiimoteForSlot(unsigned int slot);
}  // namespace NetPlay
