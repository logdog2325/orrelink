// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <SFML/Network/Packet.hpp>

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Common/Event.h"
#include "Common/QoSSession.h"
#include "Common/SPSCQueue.h"
#include "Common/Timer.h"
#include "Common/TraversalClient.h"
#include "Core/NetPlayProto.h"
#include "Core/SyncIdentifier.h"
#include "UICommon/NetPlayIndex.h"

namespace NetPlay
{
class NetPlayUI;
struct SaveSyncInfo;

// XD Netplay: what ClaimXdStart found. opponent is the pid that plays GBA 2 (SI port 3), 0 when
// nobody is seated. slot_busy: the GBA 2 save still holds a player's team who no longer holds the
// seat, and it could not be reset because emulation is not fully down yet. starting: a start is
// already syncing or a battle is running; nothing was claimed or changed.
struct XdStartClaim
{
  PlayerId opponent = 0;
  bool slot_busy = false;
  bool starting = false;
};

class NetPlayServer : public Common::TraversalClientClient
{
public:
  void ThreadFunc();
  void SendAsync(sf::Packet&& packet, PlayerId pid, u8 channel_id = DEFAULT_CHANNEL);
  void SendAsyncToClients(sf::Packet&& packet, PlayerId skip_pid = 0,
                          u8 channel_id = DEFAULT_CHANNEL);
  void SendChunked(sf::Packet&& packet, PlayerId pid, const std::string& title = "");
  void SendChunkedToClients(sf::Packet&& packet, PlayerId skip_pid = 0,
                            const std::string& title = "");

  NetPlayServer(u16 port, bool forward_port, NetPlayUI* dialog,
                const NetTraversalConfig& traversal_config);
  ~NetPlayServer() override;

  bool ChangeGame(const SyncIdentifier& sync_identifier, const std::string& netplay_name);
  bool ComputeGameDigest(const SyncIdentifier& sync_identifier);
  bool AbortGameDigest();
  void SendChatMessage(const std::string& msg);

  bool DoAllPlayersHaveIPLDump() const;
  bool DoAllPlayersHaveHardwareFMA() const;
  // OrreLink cross-architecture policy (see SetupNetSettings). Non-const: they
  // take m_crit.players, which the const helper above does not.
  bool RoomMixesCpuArchitectures();
  std::string DescribeRoomArchitectures();
  bool StartGame();
  bool RequestStartGame();
  void AbortGameStart();

  PadMappingArray GetPadMapping() const;
  void SetPadMapping(const PadMappingArray& mappings);

  GBAConfigArray GetGBAConfig() const;
  void SetGBAConfig(const GBAConfigArray& configs, bool update_rom);

  PadMappingArray GetWiimoteMapping() const;
  void SetWiimoteMapping(const PadMappingArray& mappings);

  // Low-level, already-synced setter: stores the value and broadcasts
  // MessageID::PadBuffer to every client. Used for session setup and by the
  // automatic sizer. A host UI editing the number by hand must NOT call this
  // directly -- use SetPadBufferSizeManual so auto stands down.
  void AdjustPadBufferSize(unsigned int size);

  // The host typed/stepped a buffer value. Manual always wins: this switches
  // the automatic sizer off (and persists that) before applying the value, so
  // auto can never overwrite the host's number a second later.
  void SetPadBufferSizeManual(unsigned int size);

  // Opt-out switch for the automatic sizer (Config::NETPLAY_AUTO_BUFFER).
  void SetAutoPadBufferEnabled(bool enabled);
  bool IsAutoPadBufferEnabled() const { return m_auto_buffer_enabled.load(); }

  // XD Netplay: true from the moment a Start is requested (the saves and the
  // codes have been read and are being sent to the other player) until the
  // game has ended. Anything that rewrites the host's GBA save or the synced
  // Battle Style block from the room UI must refuse while this holds, or the
  // two sides boot with different data. Safe to call from any thread.
  // StartGame() raises m_is_running before it clears m_start_pending, so there
  // is no instant where a start is in flight and both read false.
  bool IsStartingOrRunning() const { return m_start_pending.load() || m_is_running.load(); }
  // Separately, for the room's live battle style: a start in flight still refuses (the saves and
  // codes are being sent), a running game takes the change live. StartGame raises running before
  // it clears pending, so a caller testing pending first never sees neither during the switch.
  bool IsStartPending() const { return m_start_pending.load(); }
  bool IsGameRunning() const { return m_is_running.load(); }

  // XD Netplay seats, host UI thread only (Qt GUI thread, or the Android main thread under
  // s_host_write_mutex). ClaimXdStart picks the opponent by GetXdOpponentLocked, resets the GBA 2
  // slot if it holds someone else's team, and refuses TeamData and new joins until
  // ReleaseXdStart. Call RequestStartGame between the two, never under any other netplay lock.
  XdStartClaim ClaimXdStart();
  void ReleaseXdStart();

  // RAII over ClaimXdStart / ReleaseXdStart, held until RequestStartGame has returned.
  class XdStartFreeze
  {
  public:
    explicit XdStartFreeze(NetPlayServer& server) : m_server(server), m_claim(server.ClaimXdStart())
    {
    }
    ~XdStartFreeze() { m_server.ReleaseXdStart(); }
    XdStartFreeze(const XdStartFreeze&) = delete;
    XdStartFreeze& operator=(const XdStartFreeze&) = delete;

    const XdStartClaim& Claim() const { return m_claim; }

  private:
    NetPlayServer& m_server;
    XdStartClaim m_claim;
  };

  void SetHostInputAuthority(bool enable);

  void KickPlayer(PlayerId player);

  u16 GetPort() const;

  std::unordered_set<std::string> GetInterfaceSet() const;
  std::string GetInterfaceHost(const std::string& inter) const;

  bool is_connected = false;

private:
  class Client
  {
  public:
    PlayerId pid{};
    std::string name;
    std::string revision;
    std::string arch;  // "x86_64" | "arm64" | "other" (empty if a peer ever omits it)
    SyncIdentifierComparison game_status = SyncIdentifierComparison::Unknown;
    bool has_ipl_dump = false;
    bool has_hardware_fma = false;

    ENetPeer* socket = nullptr;
    u32 ping = 0;
    u32 current_game = 0;

    Common::QoSSession qos_session;

    // XD Netplay seats. Written only on the NETPLAY thread (OnConnect before the emplace, the
    // XdWatch case under m_crit.players). xd_serial names this connection and is never reused;
    // xd_queue is its place in line for the opponent seat (lower is earlier).
    u32 xd_serial = 0;
    u32 xd_queue = 0;
    bool xd_watching = false;

    bool operator==(const Client& other) const { return this == &other; }
    bool IsHost() const { return pid == 1; }
  };

  enum class TargetMode
  {
    Only,
    AllExcept
  };

  struct AsyncQueueEntry
  {
    sf::Packet packet;
    PlayerId target_pid{};
    TargetMode target_mode{};
    u8 channel_id = 0;
  };

  struct ChunkedDataQueueEntry
  {
    sf::Packet packet;
    PlayerId target_pid{};
    TargetMode target_mode{};
    std::string title;
  };

  bool SetupNetSettings();
  std::optional<SaveSyncInfo> CollectSaveSyncInfo();
  bool SyncSaveData(const SaveSyncInfo& sync_info);
  bool SyncCodes();
  void CheckSyncAndStartGame();

  u64 GetInitialNetPlayRTC() const;

  template <typename... Data>
  void SendResponseToPlayer(const Client& player, const MessageID message_id,
                            Data&&... data_to_send);
  template <typename... Data>
  void SendResponseToAllPlayers(const MessageID message_id, Data&&... data_to_send);
  void SendToClients(const sf::Packet& packet, PlayerId skip_pid = 0,
                     u8 channel_id = DEFAULT_CHANNEL);
  void Send(ENetPeer* socket, const sf::Packet& packet, u8 channel_id = DEFAULT_CHANNEL);
  ConnectionError OnConnect(ENetPeer* socket, sf::Packet& received_packet);
  unsigned int OnDisconnect(const Client& player);
  unsigned int OnData(sf::Packet& packet, Client& player);

  void OnTraversalStateChanged() override;
  void OnConnectReady(ENetAddress) override {}
  void OnConnectFailed(Common::TraversalConnectFailedReason) override {}
  void OnTtlDetermined(u8 ttl) override;
  void UpdatePadMapping();
  void UpdateGBAConfig();
  void UpdateWiimoteMapping();
  std::vector<std::pair<std::string, std::string>> GetInterfaceListInternal() const;
  void ChunkedDataThreadFunc();
  void ChunkedDataSend(sf::Packet&& packet, PlayerId pid, const TargetMode target_mode);
  void ChunkedDataAbort();

  void SetupIndex();
  bool PlayerHasControllerMapped(PlayerId pid) const;
  // Host plus every joiner not watching: what the lobby index publishes.
  int PlayingCount();

  // XD Netplay seats. See m_xd_seat_mutex for the locks each one needs.
  PlayerId GetXdOpponentLocked() const;
  bool XdSlotStaleLocked(PlayerId opponent) const;
  bool ResetXdSlotLocked(bool announce);
  bool XdStartOpponentLeft();
  // ---NETPLAY--- thread only.
  void SendXdSeats();

  // ---NETPLAY--- thread only, once per second off the ping tick.
  void UpdateAutoPadBuffer();

  // pulled from OnConnect()
  void AssignNewUserAPad(const Client& player);
  // pulled from OnConnect()
  // returns the PID given
  PlayerId GiveFirstAvailableIDTo(ENetPeer* player);

  NetSettings m_settings;

  // Atomic: the room UI reads both flags from its own thread through
  // IsStartingOrRunning().
  std::atomic<bool> m_is_running{false};
  bool m_do_loop = false;
  Common::Timer m_ping_timer;
  u32 m_ping_key = 0;
  bool m_update_pings = false;
  u32 m_current_game = 0;
  unsigned int m_target_buffer_size = 0;
  PadMappingArray m_pad_map;
  GBAConfigArray m_gba_config;
  PadMappingArray m_wiimote_map;
  unsigned int m_save_data_synced_players = 0;
  unsigned int m_codes_synced_players = 0;
  bool m_saves_synced = true;
  bool m_codes_synced = true;
  std::atomic<bool> m_start_pending{false};
  bool m_host_input_authority = false;
  PlayerId m_current_golfer = 1;
  PlayerId m_pending_golfer = 0;

  // --- automatic pad buffer (host-owned; see UpdateAutoPadBuffer) ---
  // Toggled from the GUI thread, read from the netplay thread.
  std::atomic<bool> m_auto_buffer_enabled{true};
  // Everything below is owned by the ---NETPLAY--- thread and never locked.
  bool m_auto_buffer_was_enabled = true;
  bool m_auto_buffer_was_running = false;
  unsigned int m_auto_buffer_raise_streak = 0;
  unsigned int m_auto_buffer_lower_streak = 0;
  u64 m_auto_buffer_quiet_until_ms = 0;
  u64 m_auto_buffer_last_change_ms = 0;

  std::map<PlayerId, Client> m_players;

  // XD Netplay seats (GetXdOpponentLocked).
  // NETPLAY thread only: hands out serials and queue places.
  u32 m_xd_counter = 0;
  // Makes a Start's claim of the seat and the saves exclusive with a TeamData write.
  // Lock order: s_host_write_mutex (Android) -> m_xd_seat_mutex -> m_crit.game (ClaimXdStart
  // takes and drops it) -> m_crit.players -> m_crit.async_queue_write. Only ever taken by a thread
  // holding no m_crit lock, and the NETPLAY thread never takes s_host_write_mutex. XDNetplay's own
  // locks (BattleCustomizer, TeamInjector) may be taken under it, never while m_crit.players is
  // held. Nothing under it may wait on a UI thread (no RunOnObject, PanicAlert, modal or blocking
  // JNI call), because the host UI thread waits on it in ClaimXdStart.
  std::mutex m_xd_seat_mutex;
  // Written under m_xd_seat_mutex; read by OnConnect under m_crit.game, which ClaimXdStart takes
  // once after raising it. Up from ClaimXdStart until ReleaseXdStart; by then m_start_pending or
  // m_is_running carries the refusal if a start began.
  std::atomic<bool> m_xd_frozen{false};
  // Under m_xd_seat_mutex: the serial whose TeamData last reached the GBA 2 slot, 0 when only the
  // host's own data is there, and that player's name for the "cleared" line.
  u32 m_xd_slot_owner = 0;
  std::string m_xd_slot_owner_name;
  // The pid ClaimXdStart seated for the start it froze; 0 for a start that is not an XD Start.
  // Checked just before the game starts, since a leave before m_start_pending rises aborts nothing.
  std::atomic<PlayerId> m_xd_start_opponent{0};

  std::unordered_map<u32, std::vector<std::pair<PlayerId, u64>>> m_timebase_by_frame;
  bool m_desync_detected = false;

  struct
  {
    std::recursive_mutex game;
    // lock order
    std::recursive_mutex players;
    std::recursive_mutex async_queue_write;
    std::recursive_mutex chunked_data_queue_write;
  } m_crit;

  Common::SPSCQueue<AsyncQueueEntry> m_async_queue;
  Common::SPSCQueue<ChunkedDataQueueEntry> m_chunked_data_queue;

  SyncIdentifier m_selected_game_identifier;
  std::string m_selected_game_name;
  std::thread m_thread;
  Common::Event m_chunked_data_event;
  Common::Event m_chunked_data_complete_event;
  std::thread m_chunked_data_thread;
  u32 m_next_chunked_data_id = 0;
  std::unordered_map<u32, unsigned int> m_chunked_data_complete_count;
  bool m_abort_chunked_data = false;

  ENetHost* m_server = nullptr;
  Common::TraversalClient* m_traversal_client = nullptr;
  NetPlayUI* m_dialog = nullptr;
  NetPlayIndex m_index;
};
}  // namespace NetPlay
