// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <SFML/Network/Packet.hpp>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
  // After any reset: the GBA 2 slot holds the seated opponent's submitted team (true), or the
  // host's spare team (false). slot_owner_name is that opponent's name when true.
  bool slot_is_guest = false;
  std::string slot_owner_name;
  // A Multi room's Start (the room format is Multi). opponent stays 0 and the slot fields unused;
  // seats[i] is SI port i+2: its holder, that connection's serial (which names its stage file),
  // name, and whether a team is in. With MultiFillSeats a lone guest (on any seat) plays Seats 3
  // and 4 and the host (serial 0) Seat 2; otherwise an empty Seat 2 is the host and an empty
  // Seat 3 or 4 repeats the other one.
  bool multi = false;
  struct Seat
  {
    PlayerId pid = 0;
    u32 serial = 0;
    std::string name;
    bool team_in = false;
  };
  std::array<Seat, 3> seats{};
};

// XD Netplay: whose team the GBA 2 slot holds when the room's format changes. Spare: the host's
// own spare team. Guest: the seated opponent's submitted team (owner_pid, owner_name). Orphan: the
// team of a player who no longer holds the seat; ClaimXdStart resets it before any Start reads it.
enum class XdSlotState
{
  Spare,
  Guest,
  Orphan,
};
struct XdSlotView
{
  XdSlotState state = XdSlotState::Spare;
  PlayerId owner_pid = 0;
  std::string owner_name;
};
// What SetXdFormat did. busy: a start is claimed or a battle is running, nothing changed.
struct XdFormatChange
{
  bool busy = false;
  XdSlotView slot;
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
  // XD Netplay: the room's format is Multi (four seats). Any thread.
  bool IsXdMultiRoom() const { return m_xd_room_format.load() == XD_FORMAT_MULTI; }
  // Host UI thread.
  bool IsHostInputAuthority() const { return m_host_input_authority; }
  // A Multi start needs the recompilers kept (single core, XD clock in a mixed room); the
  // "safest" mixed-room override would force the Cached Interpreter instead. Takes m_crit.players.
  bool XdMultiNeedsNormalCore();
  // Every connection's serial (the key of its Multi stage file). Takes m_crit.players.
  std::vector<u32> XdRoomSerials();

  // XD Netplay seats, host UI thread only (Qt GUI thread, or the Android main thread under
  // s_host_write_mutex). ClaimXdStart picks the opponent by GetXdOpponentLocked, resets the GBA 2
  // slot if it holds someone else's team, and refuses TeamData and new joins until
  // ReleaseXdStart. Call RequestStartGame between the two, never under any other netplay lock.
  XdStartClaim ClaimXdStart();
  void ReleaseXdStart();

  // XD Netplay: the room's battle format (a FormatRules id), shown to every player. Starts as the
  // host's MAIN_XD_FORMAT when the server is created. Safe to read from any thread.
  int GetXdRoomFormat() const { return m_xd_room_format.load(); }
  // Host UI thread only (on Android under s_host_write_mutex), after the caller has passed the
  // host-write check, validated the id and written MAIN_XD_FORMAT, all outside every netplay lock
  // (Config::Set runs its callbacks synchronously, a JNI call on Android). Under m_xd_seat_mutex,
  // so no TeamData write can land in between: stores the format, calls judge with a snapshot of
  // the GBA 2 slot (judge may read files; it must not touch a UI or wait on a UI thread), then
  // sends the format and the announce line to everyone. Refuses, changing nothing, while a start
  // is claimed or a battle is running.
  XdFormatChange SetXdFormat(int format, const std::function<void(const XdSlotView&)>& judge,
                             const std::string& announce);
  // A chat line from the server to one player only. Safe from the UI threads.
  void SendXdNote(PlayerId pid, const std::string& msg);

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

  // One machine's XD Netplay state check report for one tag (MessageID::StateDigest).
  struct StateReport
  {
    u32 inputs = 0;
    u32 xd = 0;
    u32 seed = 0;
    u32 crc_a = 0;
    u32 crc_b = 0;
    u32 crc_c = 0;
  };

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
    // A pong has come back, so ping is a measurement (a LAN guest can really read 0 ms).
    bool has_ping = false;
    u32 current_game = 0;

    Common::QoSSession qos_session;

    // XD Netplay seats. Written only on the NETPLAY thread (OnConnect before the emplace, the
    // XdWatch case under m_crit.players). xd_serial names this connection and is never reused;
    // xd_queue is its place in line for the opponent seat (lower is earlier).
    u32 xd_serial = 0;
    u32 xd_queue = 0;
    bool xd_watching = false;
    // The SI port this player holds (3 in 1v1; 2, 3 or 4 in Multi), 0 for none, and in Multi
    // whether a team of theirs is staged. NETPLAY thread only, under m_crit.players.
    u8 xd_seat = 0;
    bool xd_stage = false;

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
  bool XdStartPlayerLeft();
  // The ports a room seats joiners on: {3} in 1v1, {2, 3, 4} in Multi. Caller holds
  // m_crit.players, or is the NETPLAY thread.
  std::vector<u8> XdSeatPorts() const;
  PlayerId XdSeatHolderLocked(u8 port) const;
  // ---NETPLAY--- thread, under m_crit.players: unseat watchers and players on ports the room no
  // longer uses, then fill each open port with the earliest in line. In a Multi room nothing moves
  // while a start is claimed or a game runs; the fill runs when it ends.
  void FillXdSeatsLocked();
  // ---NETPLAY--- thread only.
  void SendXdSeats();

  // ---NETPLAY--- thread only, once per second off the ping tick.
  void UpdateAutoPadBuffer();

  // XD Netplay state check and room notices (v1.7.5). ---NETPLAY--- thread only.
  void OnStateDigest(PlayerId pid, u8 kind, u32 tag, const StateReport& report);
  void CheckSpectatorReport(PlayerId pid, u8 kind, u32 tag, const StateReport& report,
                            const StateReport& agreed);
  void SendStateMismatch(u8 kind, u32 tag, u8 scope, PlayerId spectator, u8 comps);
  // The automatic buffer's view of the players' PadHealth reports since the last 1 Hz tick.
  struct PadHealthTick
  {
    bool all_reported = true;  // every player in the game reported since the last tick
    bool low = false;          // a player had fewer than AUTOBUF_KEEP_DEPTH entries of slack
    bool starving = false;     // a player waited on most pops with an empty queue
    std::string detail;
  };
  PadHealthTick TakePadHealthTick();
  void OnXdNoticeFrom(const Client& player, u8 kind, u8 arg);
  void SendXdNoticeToAll(XdNoticeKind kind, PlayerId who, u8 arg);

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
  // PadHealth (v1.7.5): what each player reported since the last tick. A lower goes through only
  // while every player keeps slack; a player that keeps waiting gets a cushion (why=cushion).
  struct PadHealthState
  {
    bool reported = false;
    bool low = false;
    bool starving = false;
    std::string detail;
  };
  std::map<PlayerId, PadHealthState> m_pad_health;
  u32 m_auto_buffer_cushion_streak = 0;
  bool m_auto_buffer_veto_logged = false;
  // Multi: the "input delay is at its limit" chat line went out for this game.
  std::atomic<bool> m_auto_buffer_limit_warned{false};

  // XD Netplay state check (OnStateDigest), all ---NETPLAY--- thread, indexed by report kind
  // (0 inputs, 1-4 XD state at GBA port kind-1). m_sd_game is the game the rest belongs to.
  // m_sd_pending: reports per tag still waiting for a player's. m_sd_agreed: the players' common
  // report per tag, kept a while for spectators, who report later. m_sd_xd_streak: consecutive
  // judged XD tags that differed; one alone is logged, two in a row are announced.
  static constexpr size_t STATE_KINDS = 5;
  u32 m_sd_game = 0;
  std::array<std::map<u32, std::map<PlayerId, StateReport>>, STATE_KINDS> m_sd_pending;
  std::array<std::map<u32, StateReport>, STATE_KINDS> m_sd_agreed;
  std::array<u32, STATE_KINDS> m_sd_judged{};  // the newest tag the players were compared for
  std::array<u32, STATE_KINDS> m_sd_xd_streak{};
  bool m_sd_players_warned = false;
  u32 m_sd_detail_lines = 0;
  u32 m_sd_transient_lines = 0;
  bool m_sd_incomplete_logged = false;
  std::unordered_set<PlayerId> m_sd_spectators_warned;
  std::map<PlayerId, std::array<u32, STATE_KINDS>> m_sd_spec_streak;
  // XD Netplay notices: players away from the game window (pid -> game), and when each last
  // left, for the rate limit.
  std::map<PlayerId, u32> m_xd_away;
  std::map<PlayerId, u64> m_xd_last_left_ms;

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
  std::atomic<u32> m_xd_slot_owner{0};
  std::string m_xd_slot_owner_name;
  // The room's battle format (GetXdRoomFormat). Written only by SetXdFormat under
  // m_xd_seat_mutex; read anywhere. OnConnect sends it to each joiner, and SetXdFormat's broadcast
  // queues behind that on the NETPLAY thread, so every player ends on the latest value.
  std::atomic<int> m_xd_room_format{0};
  // The pid ClaimXdStart seated for the start it froze; 0 for a start that is not an XD Start.
  // Checked just before the game starts, since a leave before m_start_pending rises aborts nothing.
  std::atomic<PlayerId> m_xd_start_opponent{0};
  // The same for a Multi Start: Seats 2-4 (0 = none).
  std::array<std::atomic<PlayerId>, 3> m_xd_start_seats{};
  // Set by every XD claim: the claimed start is a Multi start. SetupNetSettings copies it into
  // xd_multi_p1.
  std::atomic<bool> m_xd_multi_start{false};
  // The running game's xd_multi_p1, for the pad relays on the NETPLAY thread (the only field of
  // m_settings they read, so they never race SetupNetSettings on the GUI thread).
  std::atomic<bool> m_xd_multi_wire{false};
  // A seat fill is due (a format change, a Multi fill deferred by a start or a game). NETPLAY
  // thread runs it; m_xd_was_busy (NETPLAY thread only) spots the end of a start or a game.
  std::atomic<bool> m_xd_seats_dirty{false};
  bool m_xd_was_busy = false;

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
