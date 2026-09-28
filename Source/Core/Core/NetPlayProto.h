// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/Sram.h"
#include "VideoCommon/VideoConfig.h"

namespace DiscIO
{
enum class Region;
}
namespace IOS::HLE::FS
{
class FileSystem;
}
namespace PowerPC
{
enum class CPUCore;
}
namespace HLE_XD
{
struct XdDigest;
}

namespace NetPlay
{
struct GBAConfig
{
  bool enabled = false;
  bool has_rom = false;
  std::string title;
  std::array<u8, 20> hash{};
};

using PlayerId = u8;
using FrameNum = u32;
using PadIndex = s8;
using PadMappingArray = std::array<PlayerId, 4>;
using GBAConfigArray = std::array<GBAConfig, 4>;

// XD Netplay: the room format id of Multi (UICommon/XDNetplay/FormatRules.h FORMAT_MULTI, which
// static_asserts against this). Core cannot include UICommon.
constexpr int XD_FORMAT_MULTI = 9;

// Which wire format a pad entry uses: a GBA channel sends only its buttons. Channel 0 of an XD
// multi battle (xd_multi_p1) is a GBA in the pad map but sends the full GC status, because it is
// the host's controller until XD asks for a GBA, and its entries carry the GBA keys and the
// port's control byte in analogA/analogB. Every machine picks the format with this one rule.
inline bool PadTravelsAsGba(const GBAConfigArray& gba, bool xd_multi_p1, size_t ch)
{
  return ch < gba.size() && gba[ch].enabled && !(ch == 0 && xd_multi_p1);
}

// OrreLink: this build's CPU-architecture class, sent at connect so the host can
// refuse to hand an x86_64 guest a JITARM64 core enum (or an arm64 guest JIT64):
// PowerPC.cpp's InitializeCPUCore would otherwise fall back to the platform JIT in
// silence, and two different recompilers are neither timing- nor FP-identical
// (the v1.5.9 macOS-vs-Windows desync). _M_X86_64 / _M_ARM_64 come from the root
// CMakeLists.txt (the Android NDK aarch64 build takes the same branch) and from
// Source/VSProps/Base.Dolphin.props.
constexpr const char* LocalCpuArch()
{
#if defined(_M_X86_64)
  return "x86_64";
#elif defined(_M_ARM_64)
  return "arm64";
#else
  return "other";
#endif
}

struct NetSettings
{
  bool cpu_thread = false;
  PowerPC::CPUCore cpu_core{};
  bool enable_cheats = false;
  bool enable_hardcore = false;
  int selected_language = 0;
  bool override_region_settings = false;
  bool dsp_hle = false;
  bool dsp_enable_jit = false;
  bool ram_override_enable = false;
  u32 mem1_size = 0;
  u32 mem2_size = 0;
  DiscIO::Region fallback_region{};
  bool allow_sd_writes = false;
  bool oc_enable = false;
  float oc_factor = 0;
  bool vi_oc_enable = false;
  float vi_oc_factor = 0;
  Common::EnumMap<ExpansionInterface::EXIDeviceType, ExpansionInterface::MAX_SLOT> exi_device{};
  int memcard_size_override = -1;

  std::array<u32, Config::SYSCONF_SETTINGS.size()> sysconf_settings{};

  bool efb_access_enable = false;
  bool bbox_enable = false;
  bool force_progressive = false;
  bool efb_to_texture_enable = false;
  bool xfb_to_texture_enable = false;
  bool disable_copy_to_vram = false;
  bool immediate_xfb_enable = false;
  bool efb_emulate_format_changes = false;
  int safe_texture_cache_color_samples = 0;
  bool perf_queries_enable = false;
  bool float_exceptions = false;
  bool divide_by_zero_exceptions = false;
  bool fprf = false;
  bool accurate_nans = false;
  bool accurate_fmadds = false;
  bool disable_icache = false;
  bool sync_on_skip_idle = false;
  bool sync_gpu = false;
  int sync_gpu_max_distance = 0;
  int sync_gpu_min_distance = 0;
  float sync_gpu_overclock = 0;
  bool jit_follow_branch = false;
  bool fast_disc_speed = false;
  bool mmu = false;
  bool fastmem = false;
  bool skip_ipl = false;
  bool load_ipl_dump = false;
  bool vertex_rounding = false;
  int internal_resolution = 0;
  bool efb_scaled_copy = false;
  bool fast_depth_calc = false;
  bool enable_pixel_lighting = false;
  bool widescreen_hack = false;
  TextureFilteringMode force_texture_filtering = TextureFilteringMode::Default;
  AnisotropicFilteringMode max_anisotropy = AnisotropicFilteringMode::Default;
  bool force_true_color = false;
  bool disable_copy_filter = false;
  bool disable_fog = false;
  bool arbitrary_mipmap_detection = false;
  float arbitrary_mipmap_detection_threshold = 0;
  bool enable_gpu_texture_decoding = false;
  bool defer_efb_copies = false;
  int efb_access_tile_size = 0;
  bool efb_access_defer_invalidation = false;

  bool savedata_load = false;
  bool savedata_write = false;
  bool savedata_sync_all_wii = false;

  bool strict_settings_sync = false;
  bool sync_codes = false;
  std::string save_data_region;
  bool golf_mode = false;
  bool use_fma = false;
  bool hide_remote_gbas = false;
  // OrreLink v1.5.11: XD deterministic clock (Core/HLE/HLE_XD.cpp). Serialized LAST in the
  // StartGame packet, after the SRAM bytes, in this order.
  bool xd_deterministic_clock = false;
  u32 xd_clock_salt = 0;
  u32 xd_rng_seed = 0;
  // XD multi battles: channel 0 is the port-1 controller that becomes a GBA
  // (SIDEVICE_GC_GBA_XDMULTI), and its pad entries travel in GC format with the GBA keys and the
  // port's control byte in analogA/analogB (see PadTravelsAsGba and HW/SI/XDMultiCtl.h).
  // Serialized LAST in StartGame, after xd_rng_seed.
  bool xd_multi_p1 = false;

  Sram sram;

  // These are sent separately from the other settings
  PadMappingArray pad_map{};
  GBAConfigArray gba_config{};
  PadMappingArray wiimote_map{};

  // These aren't sent over the network directly
  bool is_hosting = false;
  PlayerId local_player_id;
  std::array<std::string, 4> gba_rom_paths{};
};

struct NetTraversalConfig
{
  NetTraversalConfig() = default;
  NetTraversalConfig(bool use_traversal_, std::string traversal_host_, u16 traversal_port_,
                     u16 traversal_port_alt_ = 0)
      : use_traversal{use_traversal_}, traversal_host{std::move(traversal_host_)},
        traversal_port{traversal_port_}, traversal_port_alt{traversal_port_alt_}
  {
  }

  bool use_traversal = false;
  std::string traversal_host;
  u16 traversal_port = 0;
  u16 traversal_port_alt = 0;
};

enum class MessageID : u8
{
  ConnectionSuccessful = 0,

  PlayerJoin = 0x10,
  PlayerLeave = 0x11,

  ChatMessage = 0x30,
  // XD Netplay: a joiner submits their own Showdown team, which the host
  // writes into the GBA save it will sync at start. Safe to add without a
  // protocol version bump: netplay already refuses to connect across builds.
  TeamData = 0x31,

  ChunkedDataStart = 0x40,
  ChunkedDataEnd = 0x41,
  ChunkedDataPayload = 0x42,
  ChunkedDataProgress = 0x43,
  ChunkedDataComplete = 0x44,
  ChunkedDataAbort = 0x45,

  PadData = 0x60,
  PadMapping = 0x61,
  PadBuffer = 0x62,
  PadHostData = 0x63,
  GBAConfig = 0x64,

  WiimoteData = 0x70,
  WiimoteMapping = 0x71,

  GolfRequest = 0x90,
  GolfSwitch = 0x91,
  GolfAcquire = 0x92,
  GolfRelease = 0x93,
  GolfPrepare = 0x94,

  StartGame = 0xA0,
  ChangeGame = 0xA1,
  StopGame = 0xA2,
  DisableGame = 0xA3,
  GameStatus = 0xA4,
  ClientCapabilities = 0xA5,
  HostInputAuthority = 0xA6,
  PowerButton = 0xA7,
  LiveStyle = 0xA8,  // XD Netplay: host changes the running game's battle style
  XdWatch = 0xA9,    // XD Netplay: client -> server. bool: this player only watches.
  // XD Netplay: server -> clients. Display only: u8 mode (0 1v1, 1 Multi), PlayerId seat[3] (the
  // holders of SI ports 2, 3, 4; 0 = open; a 1v1 room uses only seat[1], port 3), u8 team_bits
  // (bit i: seat[i] has a team in; in 1v1 bit 1 means the slot holds the opponent's team), u8 n,
  // n x PlayerId watchers. No version field: netplay refuses mismatched builds.
  XdSeats = 0xAA,
  XdFormat = 0xAB,   // XD Netplay: server -> clients. u8: the room's battle format (FormatRules id).
  // XD Netplay room notice, both ways. Client -> server: u8 XdNoticeKind, u8 arg. Server ->
  // clients: u8 XdNoticeKind, PlayerId who, u8 arg. Each client writes the line itself, into the
  // room chat only, never on screen.
  XdNotice = 0xAC,

  TimeBase = 0xB0,
  DesyncDetected = 0xB1,
  // XD Netplay state check (v1.7.5). Client -> server: u8 kind, u32 tag, u32 value, then u32
  // seed, crcA, crcB, crcC for the log (0 for kind 0). Kind 0: the hash of the pad entries
  // consumed, tag = pop count of the marker pad. Kind 1-4: the XD state hash read at a GC->GBA
  // WRITE of SI port kind-1, tag = that port's WRITE count. Server -> clients on a mismatch that
  // is announced: u8 kind, u32 tag, u8 scope (0 players, 1 one spectator), PlayerId spectator,
  // u8 component bits (1 inputs, 2 XD state).
  StateDigest = 0xB2,
  StateMismatch = 0xB3,

  ComputeGameDigest = 0xC0,
  GameDigestProgress = 0xC1,
  GameDigestResult = 0xC2,
  GameDigestAbort = 0xC3,
  GameDigestError = 0xC4,

  Ready = 0xD0,
  NotReady = 0xD1,

  Ping = 0xE0,
  Pong = 0xE1,
  PlayerPingData = 0xE2,
  // XD Netplay, client -> server once a second from a player, over the pads other machines own:
  // u16 longest pad wait in ms, u16 pops that waited over 1 ms, u16 pops, u16 the smallest queue
  // depth seen before a pop (0xFFFF: no such pop). The automatic buffer lowers only while every
  // player keeps slack, and adds a cushion when one keeps waiting.
  PadHealth = 0xE3,

  SyncSaveData = 0xF1,
  SyncCodes = 0xF2,
};

// XD Netplay room notices (MessageID::XdNotice).
enum class XdNoticeKind : u8
{
  LeftWindow = 0,   // a player's game window lost input focus, or their game is paused
  BackInWindow = 1,
  GbaStartFailed = 2,  // arg: the SI port (0-3) whose GBA core did not start
  // Server -> clients only: the same, from a player who only watches; the battle goes on.
  GbaStartFailedWatcher = 3,
  // 4 is kept for a later per-seat notice.
  // Server -> clients only, who = 0: a Multi battle's worst route needs more input delay than the
  // cap (once per game, mid-battle; the copy before the boot goes out as a plain chat line).
  DelayAtLimit = 5,
};

enum class ConnectionError : u8
{
  NoError = 0,
  ServerFull = 1,
  GameRunning = 2,
  VersionMismatch = 3,
  NameTooLong = 4
};

enum class SyncSaveDataID : u8
{
  Notify = 0,
  Success = 1,
  Failure = 2,
  RawData = 3,
  GCIData = 4,
  WiiData = 5,
  GBAData = 6
};

enum class SyncCodeID : u8
{
  Notify = 0,
  NotifyGecko = 1,
  NotifyAR = 2,
  GeckoData = 3,
  ARData = 4,
  Success = 5,
  Failure = 6,
};

constexpr u32 MAX_NAME_LENGTH = 30;
constexpr size_t CHUNKED_DATA_UNIT_SIZE = 16384;
constexpr u32 MAX_ENET_MTU = 1392;  // see https://github.com/lsalzman/enet/issues/132

enum : u8
{
  DEFAULT_CHANNEL,
  CHUNKED_DATA_CHANNEL,
  CHANNEL_COUNT
};

struct PadDetails
{
  std::string player_name{};
  bool is_local = false;
  int local_pad = 0;
  bool hide_gba = false;
};

std::string GetPlayerMappingString(PlayerId pid, const PadMappingArray& pad_map,
                                   const GBAConfigArray& gba_config,
                                   const PadMappingArray& wiimote_map);
bool IsNetPlayRunning();
void SetSIPollBatching(bool state);
void SendPowerButtonEvent();
std::string GetGBASavePath(int pad_num);
// <User>/GBA/NetPlayTemp<slot+1>.sav: a joiner's synced copy of the host's GBA save for SI port
// slot+1, and in an XD multi battle the host's own boot copy of that port's team.
std::string GetGBANetplayTempPath(int slot);
PadDetails GetPadDetails(int pad_num);
// The local pad whose GBA input drives this machine's GBA on SI channel `channel` in the running
// netplay game, -1 when that GBA is not this machine's; outside a netplay game, the channel
// itself. Lock-free (atomics filled at OnStartGame): safe from a UI thread per input event.
int GbaInputPadFor(int channel);
// Whether this machine's local GameCube pad `local_pad` plays a GameCube port in the running
// netplay game (a joiner's never does: its only ports are GBAs); outside a netplay game, true.
// Lock-free, like GbaInputPadFor.
bool LocalGcPadPlays(int local_pad);
// True when netplay is running and the calling core is the one booted for the current game.
// Takes crit_netplay_client: never call it from anything NetPlay_GetInput calls.
bool IsCurrentGameCore();
// A GBA core of the current game failed to start on this machine (SI port 0-3): tell the room and
// stop the game. Takes crit_netplay_client, like IsCurrentGameCore.
void ReportGbaStartFailure(int port);
// State check, XD part: XD's battle state read at the write_count-th GC->GBA WRITE of SI port
// `port`, a fixed point in the game's own code on every machine. CPU thread, from the GBA device;
// takes crit_netplay_client, like IsCurrentGameCore.
void ReportXdStateSample(int port, u32 write_count, const HLE_XD::XdDigest& digest);
int NumLocalWiimotes();
}  // namespace NetPlay
