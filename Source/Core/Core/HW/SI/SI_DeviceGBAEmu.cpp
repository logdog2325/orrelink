// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAS_LIBMGBA

#include "Core/HW/SI/SI_DeviceGBAEmu.h"

#include <array>
#include <atomic>
#include <vector>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GBACore.h"
#include "Core/HW/GBADetectLog.h"
#include "Core/HLE/HLE_XD.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Host.h"
#include "Core/NetPlayProto.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"

namespace SerialInterface
{
static_assert(GBA_LINK_DIAG_CHANNELS == static_cast<size_t>(MAX_SI_CHANNELS),
              "The link diagnostic needs one slot per SI channel");

std::array<GBALinkDiag, GBA_LINK_DIAG_CHANNELS> g_gba_link_diag;

// Read-only lookup for the diagnostic tap. Returns nullptr for anything that is
// not a real SI channel, so no caller can index out of bounds.
static GBALinkDiag* DiagSlot(int device_number)
{
  if (device_number < 0 || static_cast<size_t>(device_number) >= g_gba_link_diag.size())
    return nullptr;
  return &g_gba_link_diag[static_cast<size_t>(device_number)];
}

static s64 GetSyncInterval(const SystemTimers::SystemTimersManager& timers)
{
  return timers.GetTicksPerSecond() / 1000;
}

CSIDevice_GBAEmu::CSIDevice_GBAEmu(Core::System& system, SIDevices device, int device_number)
    : ISIDevice(system, device, device_number)
{
  GBADetectLog::OnDeviceCreated();  // opens the session log BEFORE Core::Start fires H1
  // g_gba_link_diag is process-lifetime; a session that ended mid-battle
  // leaves established/locked latched true. Zero this channel's slot so the
  // cross-socket regime read below never sees a PREVIOUS session's state (and
  // so every netplay client starts from the same slate -- the determinism of
  // that read depends on this).
  if (GBALinkDiag* diag = DiagSlot(m_device_number))
  {
    diag->link.store(false, std::memory_order_relaxed);
    diag->window_count.store(0, std::memory_order_relaxed);
    diag->reset_count.store(0, std::memory_order_relaxed);
    diag->established.store(false, std::memory_order_relaxed);
    diag->locked.store(false, std::memory_order_relaxed);
    diag->probe_count.store(0, std::memory_order_relaxed);
    diag->last_cmd.store(0, std::memory_order_relaxed);
  }
  // Read once; in netplay every client constructs its devices from the same
  // synced config, and never re-reading means a mid-session toggle can't split
  // the clients' reset behavior.
  m_edge_reset_enabled = Config::Get(Config::MAIN_GBA_EDGE_RESET);
  m_fullboot_presence = Config::Get(Config::MAIN_GBA_FULLBOOT_PRESENCE);
  // Every log self-identifies the reset policy it ran under.
  GBADetectLog::LogEvent(m_device_number, 0, "policy",
                         fmt::format("edge={} fullboot={}", m_edge_reset_enabled ? 1 : 0,
                                     m_fullboot_presence ? 1 : 0));
  m_core = std::make_shared<HW::GBA::Core>(system, m_device_number);
  m_core->Start(system.GetCoreTiming().GetTicks());
  m_gbahost = Host_CreateGBAHost(m_core);
  m_core->SetHost(m_gbahost);
  system.GetSerialInterface().ScheduleEvent(m_device_number,
                                            GetSyncInterval(system.GetSystemTimers()));
}

CSIDevice_GBAEmu::~CSIDevice_GBAEmu()
{
  m_system.GetSerialInterface().RemoveEvent(m_device_number);
  m_core->Stop();
  m_gbahost.reset();
  m_core.reset();
  GBADetectLog::OnDeviceDestroyed();
}

namespace
{
// Pokemon XD (GXXE01 USA, main.dol sha256 c8659341...78f15) battle state that
// decides a link battle, reverse-engineered from the binary and cross-checked
// adversarially. Every fixed address is a .bss/.sbss/.sdata location in MEM1
// (BAT identity mapping: read directly, no MMU). ~1.6 KB per hook.
struct XdRange
{
  u32 addr;
  u32 len;
};
constexpr u32 XD_DISC_ID_GXXE = 0x47585845;  // "GXXE" at 0x80000000; anything else: no line
// Main PRNG (the game's own LCG): seed = seed * 0x343FD + 0x269EC3, stepped by
// every rand_u16 (407 callers) / rand_float (238) call; written once at boot from
// OSGetTime's low word (0x800efa58). The pointer cell at +4 always reads
// 0x804E8610 (its only setter has no callers).
constexpr u32 XD_RNG_SEED = 0x804E8610;
// SDK CARD unlock LCG state (ANSI LCG, 0x804e81e0 = r13-31808), seeded from OSGetTick by
// CARD DummyLen 0x800c0a50 / __CARDUnlock 0x800c0b14 (all 12 references) -- NOT JoyBoot: it
// only moves on memory-card mount/unlock. Logged raw as jb= (name kept for log compatibility);
// under the v1.5.11 clock those three reads are per-site ordinals, so it stays identical.
constexpr u32 XD_JOYBOOT_LCG = 0x804E81E0;
constexpr u32 XD_BATTLE_FLAGS = 0x804EB938;  // u32 bit flags, 263 refs in the engine
// crcA: battle-engine scalars (.sbss: flags, command-ring indices, turn counters,
// scratch-block pointer) + the .sdata battle event struct.
constexpr XdRange XD_CRC_A[] = {{0x804EB8D0, 0xB0}, {0x804E85C0, 0x20}};
// crcB: party state of the LIVE battle context. The context is a runtime
// pointer (cell 0x804af528) to one of two 0x6ef0-byte objects (ctx[0]=0x804a1744,
// ctx[1]=0x804a8634); trainer = ctx+0x64+t*0x3744; party entry =
// trainer+0x97c+i*0x300 (record at +4: species u16, +4 current HP u16, +0x11
// level, +0x28 status, +0x80 moves/PP, +0x90 max HP); active slot =
// trainer+0x1b7c+s*0x894. Skipped (crcB=0) when the pointer is not one of the
// two objects; the pointer is logged as ctx=.
constexpr u32 XD_CTX_PTR = 0x804AF528;
constexpr u32 XD_CTX_A = 0x804A1744;
constexpr u32 XD_CTX_B = 0x804A8634;
constexpr u32 XD_TRAINER_OFF = 0x64;
constexpr u32 XD_TRAINER_STRIDE = 0x3744;
constexpr u32 XD_PARTY_OFF = 0x97C;
constexpr u32 XD_PARTY_STRIDE = 0x300;
constexpr u32 XD_SLOT_OFF = 0x1B7C;
constexpr u32 XD_SLOT_STRIDE = 0x894;
// crcC (informational, never gates a line): GBA driver per-channel state, .sbss
// link state, battle mode word, and the ruleset table entry the format pin
// writes (0x804334C0 = BattleCustomizer's ORRE_RULESET_BASE).
constexpr XdRange XD_CRC_C[] = {
    {0x80428338, 0x20}, {0x804EA768, 0x28}, {0x80429A00, 0x04}, {0x804334C0, 0x90}};
constexpr u32 XD_UPLOAD_WRITES = 26900;  // multiboot client upload, same as the OSD phase logic
constexpr u32 XD_MAX_LINES_PER_SOCKET = 80000;  // ~6 MB worst case, under the file cap

size_t AppendEmu(const Memory::MemoryManager& memory, u8* dst, size_t at, u32 addr, u32 len)
{
  memory.CopyFromEmu(dst + at, addr, len);
  return at + len;
}
}  // namespace

// One gba_detect 'xd' line: RNG seed + CRC32s of the battle state, keyed by
// wseq = m_diag_wr_count (this socket's session-lifetime GC->GBA WRITE ordinal,
// identical on every client up to the divergence, unlike the emulated tick).
// Written on the forced events (link-established, battle-locked, upload-done)
// and otherwise at a battle-phase WRITE only when seed/crcA/crcB changed since
// the last line on this socket, at most ~60 lines/s, never during the multiboot
// upload (26,900 WRITEs that never touch battle state). Two players' logs then
// carry identical lines at identical wseq until the first divergence:
//   diff <(grep ' xd ' a.log | sed -E 's/^t=[0-9]+ //') <(... b.log ...)
// seed equal + crcB different = arithmetic diverged; seed different first = RNG
// consumption diverged earlier; seed different on the very first line = boot-
// time time-base divergence (compare the t= of the first 'joy >' lines).
void CSIDevice_GBAEmu::LogXdState(const u8* request, const char* why, bool forced)
{
  if (!Core::IsCPUThread() || m_xd_lines >= XD_MAX_LINES_PER_SOCKET)
    return;
  const auto& memory = m_system.GetMemory();
  if (memory.Read_U32(0x80000000) != XD_DISC_ID_GXXE)
    return;
  const u64 tps = m_system.GetSystemTimers().GetTicksPerSecond();
  if (!forced)
  {
    // Not during the client upload (battle-locked but the ~27k-WRITE stream is
    // the multiboot image, not battle traffic), and never faster than one line
    // per emulated frame.
    const bool uploading =
        m_battle_locked && (m_diag_wr_count - m_osd_upload_base) < XD_UPLOAD_WRITES;
    if (uploading)
      return;
    if (m_xd_last_tick != 0 && m_timestamp_sent - m_xd_last_tick < tps / 60)
      return;
  }

  const u32 seed = memory.Read_U32(XD_RNG_SEED);
  const u32 joyboot = memory.Read_U32(XD_JOYBOOT_LCG);
  const u32 flags = memory.Read_U32(XD_BATTLE_FLAGS);
  const u32 ctx = memory.Read_U32(XD_CTX_PTR);
  // OrreLink v1.5.12: XD's frame counters and the frame-exact logical time base
  // (HLE_XD, no per-call term). Identical on every machine at the same wseq while the games are
  // in step; lt=00000000 when the clock is not installed (solo, same-arch room).
  // v1.5.12: lf = the clock unit (sampled VI fields, 60/s in menus AND battle); pf = XD's
  // pass counter (30/s in battle); lt = Base + P*lf. Clock off: lf falls back to pf.
  const u32 pf = HLE_XD::GetFrame(m_system);
  const u32 lf = HLE_XD::IsInstalled() ? HLE_XD::GetFields() : pf;
  const u32 lt =
      HLE_XD::IsInstalled() ? static_cast<u32>(HLE_XD::FrameExactTimeBase(m_system)) : 0;

  u8 buf[2048];
  size_t n = 0;
  for (const XdRange& r : XD_CRC_A)
    n = AppendEmu(memory, buf, n, r.addr, r.len);
  const u32 crc_a = Common::ComputeCRC32(buf, n);

  u32 crc_b = 0;
  if (ctx == XD_CTX_A || ctx == XD_CTX_B)
  {
    n = 0;
    for (u32 t = 0; t < 2; t++)
    {
      const u32 trainer = ctx + XD_TRAINER_OFF + t * XD_TRAINER_STRIDE;
      for (u32 i = 0; i < 6; i++)
      {
        const u32 record = trainer + XD_PARTY_OFF + i * XD_PARTY_STRIDE + 4;
        n = AppendEmu(memory, buf, n, record, 0x30);
        n = AppendEmu(memory, buf, n, record + 0x80, 0x20);
      }
      for (u32 sl = 0; sl < 2; sl++)
        n = AppendEmu(memory, buf, n, trainer + XD_SLOT_OFF + sl * XD_SLOT_STRIDE, 0x40);
    }
    crc_b = Common::ComputeCRC32(buf, n);
  }

  n = 0;
  for (const XdRange& r : XD_CRC_C)
    n = AppendEmu(memory, buf, n, r.addr, r.len);
  const u32 crc_c = Common::ComputeCRC32(buf, n);

  // jb (the JoyBoot LCG) participates: it is the very word the v1.5.9 evidence
  // points at, and both sockets' key exchanges share it.
  const bool changed = !m_xd_have_last || seed != m_xd_last_seed || joyboot != m_xd_last_jb ||
                       crc_a != m_xd_last_crc[0] || crc_b != m_xd_last_crc[1];
  if (!changed && !forced)
    return;
  m_xd_have_last = true;
  m_xd_last_seed = seed;
  m_xd_last_jb = joyboot;
  m_xd_last_crc[0] = crc_a;
  m_xd_last_crc[1] = crc_b;
  m_xd_last_tick = m_timestamp_sent;
  ++m_xd_lines;

  std::string data;
  if (request != nullptr && m_last_cmd == EBufferCommands::CMD_WRITE_GBA)
  {
    data = fmt::format(" data={:02x}{:02x}{:02x}{:02x}", request[1], request[2], request[3],
                       request[4]);
  }
  if (forced)
  {
    // Clock state on the forced events only ('xdclk' never matches grep ' xd '). n= is
    // EXPECTED to differ slightly between machines: it counts jittered poll iterations that
    // by design reach no game state.
    GBADetectLog::LogEvent(
        m_device_number, m_timestamp_sent, "xdclk",
        // R= is a LIVE read of the SDK retraceCount at this SI event, not at 'now': it may
        // differ by 1 across machines (xdclk is never in the compare set). dn= is CoreTiming
        // ticks between the last two 'now' stamps: 8,108,100 per field.
        fmt::format("why={} mode={} S={} F={} R={} dn={} n={} inc={} salt={:08x} "
                    "seed_cfg={:08x} period={}",
                    why, HLE_XD::IsInstalled() ? "on" : "off", lf, pf,
                    memory.Read_U32(0x804EAC54), HLE_XD::LastNowDelta(), HLE_XD::CallCount(),
                    HLE_XD::DefaultIncrement(), HLE_XD::GetSalt(), HLE_XD::GetSeed(),
                    HLE_XD::GetPeriod()),
        false);
  }
  GBADetectLog::LogEvent(
      m_device_number, m_timestamp_sent, "xd",
      fmt::format("wseq={} why={}{} lf={} pf={} lt={:08x} seed={:08x} jb={:08x} ctx={:08x} "
                  "flags={:08x} crcA={:08x} crcB={:08x} crcC={:08x}",
                  m_diag_wr_count, why, data, lf, pf, lt, seed, joyboot, ctx, flags, crc_a, crc_b,
                  crc_c),
      false);  // bounded; the <=1/s summary line flushes
}

int CSIDevice_GBAEmu::RunBuffer(u8* buffer, int request_length)
{
  switch (m_next_action)
  {
  case NextAction::SendCommand:
  {
#ifdef _DEBUG
    NOTICE_LOG_FMT(SERIALINTERFACE, "{} cmd {:02x} [> {:02x}{:02x}{:02x}{:02x}]", m_device_number,
                   buffer[0], buffer[1], buffer[2], buffer[3], buffer[4]);
#endif
    m_last_cmd = static_cast<EBufferCommands>(buffer[0]);
    m_timestamp_sent = m_system.GetCoreTiming().GetTicks();

    // Colosseum/XD detect a GBA only while the connected game's joybus link is
    // open. In Emerald that window is the copyright screen: its cartridge code
    // writes RCNT=0xC000 (link on) at boot and clears it a second or two later
    // unless the GC's 0xFF reset has already begun the handshake. So detection
    // needs XD's probe to land inside that short window. Resetting the core
    // re-runs the copyright screen to reopen it; this replaces the desktop
    // "reset the GBA at the connection screen" ritual, with no user action.
    //
    // Re-arm the reset the moment the window closes (link true -> false) so a
    // fresh window opens every couple seconds until the handshake catches,
    // instead of a coarse fixed timer. Never reset while the link is open (an
    // in-progress handshake holds the window), nor faster than the core's
    // reset->copyright boot time, nor during netplay (host drives resets).
    // A READ/WRITE command means XD is mid-handshake (uploading its multiboot
    // client / exchanging data), not just idle-probing. Record it so the
    // auto-reset backs off and lets the connection complete instead of
    // rebooting the GBA out from under an active handshake (the failure mode:
    // XD reports "checking connection... connection failed" and the GBA loops
    // its boot screen).
    if (m_last_cmd == EBufferCommands::CMD_READ_GBA ||
        m_last_cmd == EBufferCommands::CMD_WRITE_GBA)
    {
      // I3 (gba_detect.log): a data command after >=1s of data silence starts a
      // conversation we want captured with content -- the client hello, its
      // post-hello report and any first GC WRITE all follow such silences (the
      // rate-limited class logging provably missed the report in one session).
      const u64 tps_now = static_cast<u64>(m_system.GetSystemTimers().GetTicksPerSecond());
      if ((m_last_data_cmd == 0 ||
           (m_timestamp_sent > m_last_data_cmd && m_timestamp_sent - m_last_data_cmd > tps_now)) &&
          m_diag_cmd_burst < 32)
      {
        m_diag_cmd_burst = 32;
      }
      m_last_data_cmd = m_timestamp_sent;
    }

    const u64 tps = static_cast<u64>(m_system.GetSystemTimers().GetTicksPerSecond());
    const bool link_now = m_core->IsLinkEnabled();

    const bool probing = m_last_cmd == EBufferCommands::CMD_RESET ||
                         m_last_cmd == EBufferCommands::CMD_STATUS;
    // Savestate loads can leave a recorded tick ahead of the current one, so
    // never let these differences wrap around into a huge elapsed time.
    const auto elapsed_since = [this](u64 then) -> u64 {
      return (then == 0 || m_timestamp_sent < then) ? 0 : m_timestamp_sent - then;
    };
    // Emerald's GameCubeMultiBoot_Init turns JOYBUS mode OFF and immediately back
    // ON (two adjacent stores to RCNT) and re-runs that every frame while it
    // waits for the GameCube. The link flag therefore blinks constantly during a
    // normal handshake, and an SI probe landing in one of those gaps used to
    // look exactly like "the window just closed" -- which rebooted Emerald in
    // the middle of the handshake it was trying to complete, thousands of times
    // per attempt. Require the link to have been down for a full second before
    // treating it as genuinely closed; that swallows the per-frame toggling
    // while still noticing the real close at the end of the connection screen.
    if (link_now)
    {
      m_last_link_seen = m_timestamp_sent;
      // Rising edge = a boot link window opened since the last poll that saw the
      // link down. Tracked independently of the diagnostics so the edge-reset
      // trigger below never depends on the diag slot.
      if (!m_link_was_enabled)
        m_window_since_reset = true;
    }
    const bool handshake_active = m_last_data_cmd != 0 && elapsed_since(m_last_data_cmd) < tps * 8;

    // Count consecutive polls where XD is probing a DEAD link. A back-out to the
    // "connect a GBA" screen floods RESET/STATUS at a down link continuously; a
    // bounded battle animation cannot build a long run, and a lone stale poll
    // after a polling gap is a run of 1. Corroborates a genuine tear-down.
    if (link_now)
      m_probe_link_down_streak = 0;
    else if (probing && m_probe_link_down_streak < 0xffffffffu)
      ++m_probe_link_down_streak;

    // Count data commands only while the link is actually up, and only within
    // one unbroken burst: a failed handshake also pushes READ/WRITE at a dead
    // socket, so a lifetime total would creep to the battle threshold purely
    // from XD retrying, and lock the port out exactly like the bug below.
    const bool data_cmd_now =
        m_last_cmd == EBufferCommands::CMD_READ_GBA || m_last_cmd == EBufferCommands::CMD_WRITE_GBA;
    if (!handshake_active)
      m_data_cmd_count = 0;
    else if (data_cmd_now && link_now && m_data_cmd_count < DATA_CMDS_FOR_BATTLE)
      ++m_data_cmd_count;

    // Latch once the joybus link is up AND real data (READ/WRITE) is flowing:
    // the party read is done and the battle is underway. Don't auto-reset after
    // this. Otherwise a rare coincidence during turn 1 -- Emerald briefly leaves
    // JOYBUS mode at the "Link standby -> battle scene" transition (link_now
    // false) while an 8s+ animation has let handshake_active lapse and a
    // STATUS/RESET probe lands in that window -- would satisfy the guard below
    // and reboot Emerald out from under XD, dropping the link and crashing the
    // fight with no message.
    //
    // The data command must be RECENT. XD reads the two sockets in sequence, so
    // the port it isn't working on yet can see an old stray data command and,
    // seconds later, a boot-time link window; latching on that coincidence wedged
    // the port for the rest of the session (auto-reset off, window never reopens,
    // "checking connection" forever). That is the socket-3 detection failure.
    // Held until a genuine back-out (see the rematch release below). A timed
    // clear keyed on DATA recency was tried once and fired during team preview
    // -- a slow human pick starves data but NOT the link -- so the release below
    // keys on LINK recency instead, which team preview keeps fresh.
    if (link_now && handshake_active)
      m_link_established = true;

    // Once this much traffic has crossed a live link in one burst, XD has
    // genuinely read a party and the battle is underway: lock the port so no
    // auto-reset can fire for the duration. Rebooting Emerald mid-battle drops
    // the link and kills the fight with no message, so that has to be
    // impossible rather than merely unlikely, and a party is hundreds of
    // commands where a failed probe is a handful.
    if (m_data_cmd_count >= DATA_CMDS_FOR_BATTLE && !m_battle_locked)
    {
      m_battle_locked = true;
      // One shot at first lock: which battle FORMAT did the game actually
      // choose? The doubles-targeting field investigation died on exactly this
      // being unrecoverable after the fact. The mode word at VS-ctx+8 selects
      // the link record (0=single/rec5, 1=double/rec7, 2=tag/rec8), and the
      // record's own leading bytes (type, trainers-per-side, style, party) are
      // the layout the GBAs get told -- an absent fourth participant in tag
      // mode reads back from here. Try-reads: a non-XD game on the link (or
      // pre-boot state) must log zeros, never raise an alert. Observational
      // only; log is local, so netplay determinism is untouched.
      if (Core::IsCPUThread())
      {
        const Core::CPUThreadGuard guard(m_system);
        const auto rd = [&guard](u32 addr) {
          const auto v = PowerPC::MMU::HostTryRead<u32>(guard, addr);
          return v ? v->value : 0u;
        };
        std::string recs;
        for (int i = 5; i <= 8; i++)
        {
          const u32 base = 0x80B1CDE0 + static_cast<u32>(i) * 0x3C;
          recs += fmt::format(" rec{}={:08x}+{:04x}", i, rd(base), rd(base + 4) >> 16);
        }
        GBADetectLog::LogEvent(m_device_number, m_system.GetCoreTiming().GetTicks(), "vsformat",
                               fmt::format("mode={:08x} quick={:08x}{}", rd(0x80429A00),
                                           rd(0x804299FC), recs));
      }
    }

    // Rematch: release the session latches ONLY on a real back-out to XD's
    // connection screen, so a second battle re-links without a fresh netplay
    // room. Distinguishing signal, keyed on LINK recency (never data recency):
    // the joybus link has been continuously down FAR longer than any battle
    // animation (~8 s worst case) -- team preview holds the link up, so a slow
    // pick can never reach this -- AND XD is flooding a dead socket with probes.
    // Reads only synced state, so every netplay client releases on the same
    // poll. One-shot: clearing the latches makes this false next poll and hands
    // off to the normal auto-reset guard, which reopens the boot link window.
    // 15 s of CONTINUOUS link-down (was 45 s): on-device a player backing out
    // after a successful connect and retrying within the hold found the socket
    // still latched -- a guaranteed park against a live client. The horizon
    // only ever needed to outlast in-battle link gaps, and every recorded
    // battle holds the link UP throughout (the client latches RCNT; the worst
    // observed in-battle DATA gap is ~5 s and is covered separately by
    // handshake_active's 8 s window; Emerald's screen-transition RCNT blinks
    // refresh m_last_link_seen every up-sample, so continuous down-time can't
    // accumulate mid-session). Post-back-out teardown drops the link in ~3.5 s,
    // so release now lands ~18 s after backing out instead of ~50.
    const bool backed_out = (m_battle_locked || m_link_established) && probing && !link_now &&
                            !handshake_active && m_last_link_seen != 0 &&
                            elapsed_since(m_last_link_seen) > tps * 15 &&
                            m_probe_link_down_streak >= 64;
    if (backed_out)
    {
      GBADetectLog::LogEvent(
          m_device_number, m_timestamp_sent, "rematch-release",
          fmt::format("was_locked={} was_est={} link_down_ago={} down_streak={}", m_battle_locked,
                      m_link_established, elapsed_since(m_last_link_seen), m_probe_link_down_streak));
      m_link_established = false;
      m_battle_locked = false;
      m_data_cmd_count = 0;
    }


    // Silence the GBA until its link is actually established: the boot jingle
    // otherwise replays on every auto-reset while XD probes. Under netplay,
    // remote players' GBAs additionally stay muted for the whole session so
    // each player hears only their own handheld. Host-side audio only -- no
    // effect on emulation state, so no desync concern.
    if (NetPlay::IsNetPlayRunning() && !m_netplay_locality_cached)
    {
      m_netplay_pad_is_local = NetPlay::GetPadDetails(m_device_number).is_local;
      m_netplay_locality_cached = true;
      // One-shot per socket: makes two clients' logs alignable at a glance
      // (which machine owns which socket). Uses the same cached value the audio
      // mute path already trusts, so it inherits its safety.
      GBADetectLog::LogEvent(m_device_number, m_timestamp_sent, "role",
                             fmt::format("netplay=1 local={}", m_netplay_pad_is_local ? 1 : 0));
    }
    m_core->SetLinkAudioMuted((!m_link_established && !m_battle_locked) ||
                              (m_netplay_locality_cached && !m_netplay_pad_is_local));

    if (probing && !link_now && !handshake_active && !m_link_established && !m_battle_locked)
    {
      // NO on-screen advice fires from here. A press-silence heuristic cannot
      // tell "parked at the connect screen" from "idling on any menu" -- the
      // wire looks identical (XD polls the sockets constantly either way), and
      // on device the hint misfired both during a live handoff and on the boot
      // menu. The recovery rule (back out with B, re-enter) lives in the
      // release notes; the roll-state log lines still record every park.
      //
      // Is any OTHER GBA socket already engaged? Used by the legacy full-boot
      // presence-regime split. g_gba_link_diag is written by each device
      // during its own poll on this same thread, and est/lck derive purely
      // from synced emulation state, so the read is deterministic across
      // netplay clients.
      bool other_socket_engaged = false;
      if (m_fullboot_presence)
      {
        auto& si_for_diag = m_system.GetSerialInterface();
        for (int ch = 0; ch < MAX_SI_CHANNELS; ++ch)
        {
          if (ch == m_device_number || si_for_diag.GetDeviceType(ch) != GetDeviceType())
            continue;
          if (GBALinkDiag* other = DiagSlot(ch);
              other && (other->established.load(std::memory_order_relaxed) ||
                        other->locked.load(std::memory_order_relaxed)))
          {
            other_socket_engaged = true;
            break;
          }
        }
      }
      // Two ways to decide the GBA should reboot and reopen its boot link
      // window:
      //
      // EDGE (default, config-gated): a window opened this boot cycle and the
      // link has now been continuously down >= 250 ms (+ dither, below). The
      // debounce is what makes this race-free: Emerald's GameCubeMultiBoot_Init
      // blinks RCNT within a frame while handshaking, so a live handshake
      // refreshes m_last_link_seen every up-sample and can never accumulate the
      // down-time; only a genuinely closed window can.
      //
      // UNIFORM FAST REGIME (default): every pre-arm socket cycles at ~1.1-1.5 s
      // showing only the BIOS's short (~133 ms) boot beacon. The on-device roll
      // forensics (9/9 across three builds) settled why: XD's menu-entry arming
      // completes ~0.6-1.0 s after the player's final press and PARKS iff
      // Emerald's LONG cartridge window overlaps that span -- while every
      // recorded arm latched within ~20 ms of a window-open edge, almost always
      // a beacon. The long window is only needed AFTER arming, for the AXVE
      // ladder, and the post-arm boot supplies it: the est latch stops all
      // resets the moment XD engages, so the engaged boot runs uninterrupted
      // into the cartridge window (proven in every successful session). This
      // is byte-identical to the handoff regime that has a 100% record.
      //
      // LEGACY (config MAIN_GBA_FULLBOOT_PRESENCE=True): the v0.4.35 per-phase
      // split -- full ~8 s boots (long window exposed pre-arm) while no socket
      // is engaged, fast cycling once one is. Kept as an escape hatch. If ever
      // re-enabled, never pick a spacing in 3.45-4.79 s: that resets in the
      // dead gap before the cartridge window.
      //
      // DITHER: a deterministic 0-350 ms term hashed from the reset ordinal so
      // consecutive cycles never repeat a length -- the v0.4.33 no-arm mode
      // included a cycle of exactly 65.0000 video frames, which a frame-grid
      // sampler could stay phase-locked against forever. Pure function of
      // synced state (netplay-identical).
      //
      // TIMER (backstop, always on): link down >= 1 s and 6 s since the last
      // reset -- fires only when a boot never opened a window at all, and is
      // the sole path when the edge trigger is toggled off.
      const u64 min_spacing =
          (m_fullboot_presence && !other_socket_engaged) ? tps * 5 : tps;
      // 350 ms cap in fast mode keeps the worst inter-edge gap ~1.5 s (chained
      // short cycles included); the >>16 spreads the hash so the dither is not
      // a fixed arithmetic step from cycle to cycle.
      const u64 dither_range = min_spacing == tps ? 350u : 450u;
      const u64 dither =
          ((static_cast<u64>(m_auto_reset_ordinal) * 2654435761u >> 16) % dither_range) *
          (tps / 1000);
      const bool link_idle = m_last_link_seen == 0 || elapsed_since(m_last_link_seen) > tps;
      const bool timer_fire =
          link_idle && (m_last_auto_reset == 0 || elapsed_since(m_last_auto_reset) > tps * 6);
      const bool edge_fire =
          m_edge_reset_enabled && m_window_since_reset && m_last_link_seen != 0 &&
          elapsed_since(m_last_link_seen) > tps / 4 + dither &&
          (m_last_auto_reset == 0 || elapsed_since(m_last_auto_reset) > min_spacing);
      if (edge_fire || timer_fire)
      {
        if (!NetPlay::IsNetPlayRunning())
        {
          // Async: reset on the GBA's own event thread. Calling Reset() here
          // would Flush() (block the CPU thread until the event thread drains),
          // which deadlocks the whole emulator on Android where the event
          // thread can stall in the synchronous frame callback.
          m_core->RequestReset(m_timestamp_sent);
          if (GBALinkDiag* diag = DiagSlot(m_device_number))
            diag->reset_count.fetch_add(1, std::memory_order_relaxed);
        }
        else if (const NetPlay::PadDetails details = NetPlay::GetPadDetails(m_device_number);
                 details.is_local && details.local_pad < 4)
        {
          // Netplay: resetting only the local mirror of this core would desync
          // the other players' copies of it. Ride the input stream instead --
          // the pad's owner injects the X-button reset signal (the same path
          // the desktop GBA window's Reset action uses), netplay replicates it
          // to everyone, and every client resets this core at the same synced
          // poll (the PAD_BUTTON_X handler below).
          Pad::SetGBAReset(details.local_pad, true);
          if (GBALinkDiag* diag = DiagSlot(m_device_number))
            diag->reset_count.fetch_add(1, std::memory_order_relaxed);
        }
        m_last_auto_reset = m_timestamp_sent;
        m_data_cmd_count = 0;
        m_window_since_reset = false;  // a fresh boot must open its own window
        ++m_auto_reset_ordinal;
        GBADetectLog::LogEvent(
            m_device_number, m_timestamp_sent, "auto-reset",
            fmt::format("{} reason={} mode={} last_seen_ago={} ord={} dither={}",
                        NetPlay::IsNetPlayRunning() ? "net" : "solo",
                        edge_fire ? "edge" : "timer", min_spacing == tps ? "fast" : "full",
                        elapsed_since(m_last_link_seen), m_auto_reset_ordinal,
                        dither / (tps / 1000)));
        if (m_diag_cmd_burst < 8)
          m_diag_cmd_burst = 8;  // capture the commands right after the reboot
      }
    }
    // Armed-rescue: XD can arm on the cartridge window's last ~150 ms, latch
    // m_link_established off the ladder's first data, and then lose the window
    // mid-ladder -- est=1 with the link genuinely down wedges the socket (the
    // pre-arm trigger above requires !est) while XD retries against silence.
    // Release: link continuously down >= 0.5 s AND >= 8 s since our reset (a
    // healthy arm has completed AXVE by +7.0 s and holds the link up; the
    // post-upload client-reboot gap is covered by m_battle_locked, latched
    // during the upload). Clear the latch and reboot through the same
    // solo/netplay-synced path. Provably unreachable in every phase of the
    // recorded successful sessions.
    else if (probing && !link_now && m_link_established && !m_battle_locked &&
             m_last_link_seen != 0 && elapsed_since(m_last_link_seen) >= tps / 2 &&
             m_last_auto_reset != 0 && elapsed_since(m_last_auto_reset) > tps * 8)
    {
      m_link_established = false;
      if (!NetPlay::IsNetPlayRunning())
      {
        m_core->RequestReset(m_timestamp_sent);
        if (GBALinkDiag* diag = DiagSlot(m_device_number))
          diag->reset_count.fetch_add(1, std::memory_order_relaxed);
      }
      else if (const NetPlay::PadDetails details = NetPlay::GetPadDetails(m_device_number);
               details.is_local && details.local_pad < 4)
      {
        Pad::SetGBAReset(details.local_pad, true);
        if (GBALinkDiag* diag = DiagSlot(m_device_number))
          diag->reset_count.fetch_add(1, std::memory_order_relaxed);
      }
      m_last_auto_reset = m_timestamp_sent;
      m_data_cmd_count = 0;
      m_window_since_reset = false;
      ++m_auto_reset_ordinal;
      GBADetectLog::LogEvent(m_device_number, m_timestamp_sent, "rescue-reset",
                             fmt::format("{} last_seen_ago={}",
                                         NetPlay::IsNetPlayRunning() ? "net" : "solo",
                                         elapsed_since(m_last_link_seen)));
      if (m_diag_cmd_burst < 8)
        m_diag_cmd_burst = 8;
    }
    // Per-roll state line: whenever a new GC-pad press is observed, snapshot
    // this socket's link state on the same clock. One line per press per
    // socket makes every menu-entry roll self-classifying against the park
    // law (window type/age at the arming-completion span) with no
    // cross-referencing.
    if (const u64 press = GBADetectLog::LastPadPress();
        press != 0 && press != m_diag_last_press_seen)
    {
      m_diag_last_press_seen = press;
      GBADetectLog::LogEvent(
          m_device_number, m_timestamp_sent, "roll-state",
          fmt::format("link={} est={} lck={} win_age_ms={} last_dur_ms={} ord={} cfg={}",
                      link_now ? 1 : 0, m_link_established ? 1 : 0, m_battle_locked ? 1 : 0,
                      m_diag_window_open_tick == 0 ?
                          0 :
                          elapsed_since(m_diag_window_open_tick) / (tps / 1000),
                      m_diag_last_window_dur_ms, m_auto_reset_ordinal,
                      m_fullboot_presence ? "full" : "fast"));
    }

    // Diagnostic tap. Strictly a mirror of the state decided above -- no branch
    // below this point reads any of it, and nothing above it was moved or
    // changed. Placed here so it sees the final values of this poll.
    if (GBALinkDiag* diag = DiagSlot(m_device_number))
    {
      if (link_now && !m_diag_prev_link)
      {
        diag->window_count.fetch_add(1, std::memory_order_relaxed);
        m_diag_window_open_tick = m_timestamp_sent;
        m_diag_first_cmd_logged = false;
        // gap_ms = time since the previous window closed: the direct per-edge
        // measure of the inter-presence gap XD's armed watcher must bridge.
        GBADetectLog::LogEvent(
            m_device_number, m_timestamp_sent, "window-open",
            fmt::format("gap_ms={}",
                        m_diag_window_close_tick == 0 ?
                            0 :
                            elapsed_since(m_diag_window_close_tick) / (tps / 1000)));
        // I2: capture the first probes INTO the fresh window with responses, so
        // the status byte each listener presents (BIOS boot listener vs
        // Emerald's cartridge listener) is on the record per window.
        if (m_diag_cmd_burst < 4)
          m_diag_cmd_burst = 4;
      }
      else if (!link_now && m_diag_prev_link)
      {
        // Falling edge with duration: a ~0.13 s window is a BIOS boot beacon,
        // a ~2.7 s window is Emerald's cartridge listener -- the distinction
        // the presence-regime policy lives on, now measured per window.
        m_diag_window_close_tick = m_timestamp_sent;
        m_diag_last_window_dur_ms = m_diag_window_open_tick == 0 ?
                                        0 :
                                        elapsed_since(m_diag_window_open_tick) / (tps / 1000);
        GBADetectLog::LogEvent(m_device_number, m_timestamp_sent, "window-close",
                               fmt::format("dur_ms={}", m_diag_last_window_dur_ms));
      }
      // first-data: the armed-watcher signature -- XD's first non-STATUS
      // command into a fresh window came ~17-20 ms after window-open in every
      // recorded arm. Its absence across many windows = XD's watcher was never
      // armed (the exact datum earlier eras' logs lacked).
      if (link_now && !m_diag_first_cmd_logged &&
          m_last_cmd != EBufferCommands::CMD_STATUS)
      {
        m_diag_first_cmd_logged = true;
        GBADetectLog::LogEvent(
            m_device_number, m_timestamp_sent, "first-data",
            fmt::format("dt_ms={} cmd={:02x}",
                        m_diag_window_open_tick == 0 ?
                            0 :
                            elapsed_since(m_diag_window_open_tick) / (tps / 1000),
                        static_cast<u8>(m_last_cmd)));
      }
      if (probing)
        diag->probe_count.fetch_add(1, std::memory_order_relaxed);
      diag->link.store(link_now, std::memory_order_relaxed);
      diag->established.store(m_link_established, std::memory_order_relaxed);
      diag->locked.store(m_battle_locked, std::memory_order_relaxed);
      diag->last_cmd.store(static_cast<u8>(m_last_cmd), std::memory_order_relaxed);

      // Link progress. The wait players feel is XD's own fixed
      // multiboot ladder per GBA -- ~4 s of key exchange, then a ~26,900-write
      // upload of the ~108 KB client, then ~1.4 s of client boot -- run for one
      // socket and then the other, back to back. Nothing is stuck during it,
      // but with no feedback people back out mid-upload (the field's "GBA
      // failed to connect"). So record what is happening: one gba_detect log
      // line per phase and one per 10% of the upload (the field asked for no
      // on-screen text here). Reads only this device's own synced state; never
      // a control input.
      {
        constexpr u32 UPLOAD_WRITES = 26900;  // measured, bit-identical across sessions
        // m_diag_wr_count is a session-lifetime counter (never reset across a
        // rematch), so this upload is counted from the lock edge, not from 0.
        if (m_battle_locked && m_osd_phase < 2)
          m_osd_upload_base = m_diag_wr_count;
        const u32 uploaded = m_diag_wr_count - m_osd_upload_base;
        int phase = 0;
        if (m_link_established && !m_battle_locked)
          phase = 1;
        else if (m_battle_locked && uploaded < UPLOAD_WRITES - 200)
          phase = 2;
        else if (m_battle_locked)
          phase = 3;
        // Logged, not shown: the field asked for no on-screen text during the
        // connection screen (the link now succeeds every time); the log keeps
        // the same phase/percent trail for diagnosing a slow or stuck socket.
        if (phase != m_osd_phase)
        {
          m_osd_phase = phase;
          m_osd_bucket = -1;
          if (phase == 1)
            GBADetectLog::LogEvent(m_device_number, m_timestamp_sent, "link-progress",
                                   "negotiating (key exchange, ~4 s)");
          else if (phase == 3)
            GBADetectLog::LogEvent(
                m_device_number, m_timestamp_sent, "link-progress",
                fmt::format("upload done ({} writes), client starting", uploaded));
          if (phase == 3)
            LogXdState(buffer, "upload-done", /*forced=*/true);
        }
        if (phase == 2)
        {
          const int bucket = static_cast<int>(uploaded * 10 / UPLOAD_WRITES);
          if (bucket != m_osd_bucket)
          {
            m_osd_bucket = bucket;
            GBADetectLog::LogEvent(m_device_number, m_timestamp_sent, "link-progress",
                                   fmt::format("uploading client {}% ({} writes, ~{} s left)",
                                               bucket * 10, uploaded, (10 - bucket) * 3 / 2));
          }
        }
      }

      // H5/H6: latch rising edges. Arm a short command burst on the H5 edge so
      // the log captures the exact commands crossing the link when XD is
      // recognized. window-open deliberately does NOT arm a burst (Review #1 I4).
      if (m_link_established && !m_diag_prev_established)
      {
        GBADetectLog::LogEvent(m_device_number, m_timestamp_sent, "link-established",
                               fmt::format("data_cmd_count={}", m_data_cmd_count));
        LogXdState(buffer, "link-established", /*forced=*/true);
        if (m_diag_cmd_burst < 16)
          m_diag_cmd_burst = 16;
      }
      if (m_battle_locked && !m_diag_prev_locked)
      {
        GBADetectLog::LogEvent(m_device_number, m_timestamp_sent, "battle-locked",
                               fmt::format("data_cmd_count={}", m_data_cmd_count));
        LogXdState(buffer, "battle-locked", /*forced=*/true);
      }
      m_diag_prev_established = m_link_established;
      m_diag_prev_locked = m_battle_locked;

      // I4: ungated data-command counters -- ground truth for "did XD ever
      // READ/WRITE this socket", independent of every line-suppression rule.
      if (data_cmd_now)
      {
        if (m_last_cmd == EBufferCommands::CMD_READ_GBA)
          ++m_diag_rd_count;
        else
          ++m_diag_wr_count;
      }

      // <=1/sec summary mirroring the on-screen telemetry. Throttled on emulated
      // ticks, so no transition rate can push it past one line/sec/channel.
      if (m_diag_last_summary_tick == 0 ||
          (m_timestamp_sent > m_diag_last_summary_tick &&
           m_timestamp_sent - m_diag_last_summary_tick > tps))
      {
        GBADetectLog::LogSummary(
            m_device_number, m_timestamp_sent, m_core->IsStarted(), /*gba=*/true,
            m_netplay_pad_is_local, link_now, diag->window_count.load(std::memory_order_relaxed),
            diag->reset_count.load(std::memory_order_relaxed),
            diag->probe_count.load(std::memory_order_relaxed), m_link_established, m_battle_locked,
            static_cast<u8>(m_last_cmd), m_diag_rd_count, m_diag_wr_count);
        m_diag_last_summary_tick = m_timestamp_sent;
      }
    }
    m_diag_prev_link = link_now;

    m_link_was_enabled = link_now;

    // No quiet window anymore: it was built to shelter the (abandoned) bundled
    // BIOS's cartridge listener, and in solo it was provably inert -- commands
    // at a down link never reach the core in the first place. Every command is
    // sent; m_diag_cmd_sent stays for logger stability (always true now).
    m_diag_cmd_sent = true;
    m_core->SendJoybusCommand(m_timestamp_sent, TransferInterval(), buffer, m_keys);

    // H8 (gba_detect.log): bounded joybus command dump. buffer[0..4] still hold
    // the request here (ReceiveResponse's GetJoybusResponse only overwrites it
    // on the next poll). Bounded so the hot handshake path never pays per-poll
    // cost: the first 64 commands of the session, a short count-bounded burst
    // armed by a rare transition (auto-reset / link-established), OR a
    // command-class change -- and the class-change path is rate-limited to a
    // minimum tick gap (Review #1 I4). In the socket-3 failure the class stays
    // at 1 (STATUS), so class_changed is false and H8 falls silent after the
    // boot budget; the healthy upload oscillates class and is capped at ~200/s.
    const u8 cmd_class = probing ? 1u : (data_cmd_now ? 2u : 0u);
    const bool class_changed = cmd_class != m_diag_prev_cmd_class;
    m_diag_prev_cmd_class = cmd_class;
    const bool joy_gap_ok =
        m_diag_last_joy_tick == 0 || elapsed_since(m_diag_last_joy_tick) > tps / 200;
    m_diag_log_this_response = false;
    if (m_diag_cmd_log_count < 64 || m_diag_cmd_burst > 0 || (class_changed && joy_gap_ok))
    {
      GBADetectLog::LogJoybus(m_device_number, m_timestamp_sent, '>', buffer, request_length,
                              m_diag_cmd_sent, 0, m_battle_locked);
      m_diag_last_joy_tick = m_timestamp_sent;
      if (m_diag_cmd_log_count < 64)
        ++m_diag_cmd_log_count;
      if (m_diag_cmd_burst > 0)
        --m_diag_cmd_burst;
      m_diag_log_this_response = true;  // pair the response in ReceiveResponse
    }

    // 'xd' battle-state checksum at battle-phase GC->GBA WRITEs (the command
    // whose payload first differed between machines in the v1.5.9 report).
    // Gated inside LogXdState: upload suppressed, changed-state only, <=60/s.
    if (m_link_established && m_last_cmd == EBufferCommands::CMD_WRITE_GBA)
      LogXdState(buffer, "write", /*forced=*/false);

    auto& si = m_system.GetSerialInterface();
    si.RemoveEvent(m_device_number);
    si.ScheduleEvent(m_device_number,
                     TransferInterval() + GetSyncInterval(m_system.GetSystemTimers()));
    for (int i = 0; i < MAX_SI_CHANNELS; ++i)
    {
      if (i == m_device_number || si.GetDeviceType(i) != GetDeviceType())
        continue;
      si.RemoveEvent(i);
      si.ScheduleEvent(i, 0, static_cast<u64>(TransferInterval()));
    }

    m_next_action = NextAction::WaitTransferTime;
    [[fallthrough]];
  }

  case NextAction::WaitTransferTime:
  {
    const int elapsed_time =
        static_cast<int>(m_system.GetCoreTiming().GetTicks() - m_timestamp_sent);
    // Tell SI to ask again after TransferInterval() cycles
    if (TransferInterval() > elapsed_time)
      return 0;
    m_next_action = NextAction::ReceiveResponse;
    [[fallthrough]];
  }

  case NextAction::ReceiveResponse:
  {
    m_next_action = NextAction::SendCommand;
    const auto response_length = m_core->GetJoybusResponse(buffer);

    // H9 (gba_detect.log). A withheld command (m_diag_cmd_sent == false) makes
    // the core return 0 with nothing having been asked, so it must NEVER be
    // scored as GBA silence -- only genuinely-sent probes are classified
    // (Review #3 Q1). buffer now holds the response bytes.
    {
      const bool probe_cmd = m_last_cmd == EBufferCommands::CMD_STATUS ||
                             m_last_cmd == EBufferCommands::CMD_RESET;
      const bool resp_nonzero = response_length != 0;
      if (m_diag_cmd_sent && probe_cmd && resp_nonzero != m_diag_prev_resp_nonzero)
      {
        GBADetectLog::LogEvent(m_device_number, m_timestamp_sent,
                               resp_nonzero ? "gba-answering" : "gba-silent",
                               fmt::format("cmd={:02x}", static_cast<u8>(m_last_cmd)));
        m_diag_prev_resp_nonzero = resp_nonzero;
      }
      if (m_diag_log_this_response)
      {
        m_diag_log_this_response = false;
        if (m_diag_cmd_sent)
        {
          if (resp_nonzero)
            GBADetectLog::LogJoybus(m_device_number, m_timestamp_sent, '<', buffer,
                                    static_cast<int>(response_length), true,
                                    m_core->GetJoyStatAfterCommand(), m_battle_locked);
          else
            GBADetectLog::LogEvent(m_device_number, m_timestamp_sent, "no-response",
                                   fmt::format("cmd={:02x}", static_cast<u8>(m_last_cmd)));
        }
        // sent==0: the '>' line already recorded sent=0 (withheld); nothing to pair.
      }
    }

    if (response_length == 0)
      return -1;

#ifdef _DEBUG
    const Common::Log::LogLevel log_level =
        (m_last_cmd == EBufferCommands::CMD_STATUS || m_last_cmd == EBufferCommands::CMD_RESET) ?
            Common::Log::LogLevel::LERROR :
            Common::Log::LogLevel::LWARNING;
    GENERIC_LOG_FMT(Common::Log::LogType::SERIALINTERFACE, log_level,
                    "{}                              [< {:02x}{:02x}{:02x}{:02x}{:02x}] ({})",
                    m_device_number, buffer[0], buffer[1], buffer[2], buffer[3], buffer[4],
                    response_length);
#endif

    return response_length;
  }
  }

  // This should never happen, but appease MSVC which thinks it might.
  ERROR_LOG_FMT(SERIALINTERFACE, "Unknown state {}\n", static_cast<int>(m_next_action));
  return -1;
}

int CSIDevice_GBAEmu::TransferInterval()
{
  return SIDevice_GetGBATransferTime(m_system.GetSystemTimers(), m_last_cmd);
}

DataResponse CSIDevice_GBAEmu::GetData(u32& hi, u32& low)
{
  GCPadStatus pad_status{};
  if (!NetPlay::IsNetPlayRunning())
  {
    pad_status = Pad::GetGBAStatus(m_device_number);

    // Practice dummy: drive GBA port 3 so a single player can spar against a
    // scripted opponent without netplay. Press A to confirm/attack, and inject
    // a D-pad tap between A presses so XD's "select a Pokemon" screen advances
    // to a *different* Pokemon each cycle instead of re-confirming the same
    // slot forever (which never completes team selection).
    // Not before the link is up: Emerald's joybus window lives on the copyright
    // screen, and holding A walks straight past it, so a dummy that starts
    // mashing at boot stops socket 3 from ever being detected.
    if (m_device_number == 2 && (m_link_established || m_battle_locked) &&
        Config::Get(Config::MAIN_GBA_PRACTICE_DUMMY))
    {
      const u64 tenths = m_system.GetCoreTiming().GetTicks() /
                         (m_system.GetSystemTimers().GetTicksPerSecond() / 10);
      const u64 phase = tenths % 7;
      if (phase < 2)
      {
        pad_status.button |= PadButton::PAD_BUTTON_A;
      }
      else if (phase == 4)
      {
        // Move the cursor in the A-release gap. Alternate Right/Down each cycle
        // to sweep a 2D roster grid and avoid wedging against an edge; a single
        // 0.1s tap steps exactly one slot (too short to trigger auto-repeat).
        pad_status.button |= (tenths / 7 % 2 == 0) ? PadButton::PAD_BUTTON_RIGHT
                                                   : PadButton::PAD_BUTTON_DOWN;
      }
    }
  }
  SerialInterface::CSIDevice_GCController::HandleMoviePadStatus(m_system.GetMovie(),
                                                                m_device_number, &pad_status);

  static constexpr std::array<PadButton, 10> buttons_map = {
      PadButton::PAD_BUTTON_A,      // A
      PadButton::PAD_BUTTON_B,      // B
      PadButton::PAD_TRIGGER_Z,     // Select
      PadButton::PAD_BUTTON_START,  // Start
      PadButton::PAD_BUTTON_RIGHT,  // Right
      PadButton::PAD_BUTTON_LEFT,   // Left
      PadButton::PAD_BUTTON_UP,     // Up
      PadButton::PAD_BUTTON_DOWN,   // Down
      PadButton::PAD_TRIGGER_R,     // R
      PadButton::PAD_TRIGGER_L,     // L
  };

  m_keys = 0;
  for (size_t i = 0; i < buttons_map.size(); ++i)
    m_keys |= static_cast<u16>(static_cast<bool>((pad_status.button & buttons_map[i]))) << i;

  // Use X button as a reset signal for NetPlay/Movies. Reset asynchronously on
  // the core's own event thread -- the synchronous Reset() here can deadlock on
  // Android exactly like the auto-reset above. RequestReset is tick-anchored
  // (the core runs to the given GC timestamp before resetting), so every
  // netplay client still resets this core at the same emulated time.
  const bool x_now = (pad_status.button & PadButton::PAD_BUTTON_X) != 0;
  if (x_now)
  {
    m_core->RequestReset(m_timestamp_sent);
    if (GBALinkDiag* diag = DiagSlot(m_device_number))
      diag->reset_count.fetch_add(1, std::memory_order_relaxed);
    if (!m_diag_prev_xreset)
      GBADetectLog::LogEvent(m_device_number, m_timestamp_sent, "x-reset", {});
  }
  m_diag_prev_xreset = x_now;

  return DataResponse::NoData;
}

void CSIDevice_GBAEmu::SendCommand(u32 command, u8 poll)
{
}

void CSIDevice_GBAEmu::DoState(PointerWrap& p)
{
  p.Do(m_next_action);
  p.Do(m_last_cmd);
  p.Do(m_timestamp_sent);
  p.Do(m_keys);
  m_core->DoState(p);
}

void CSIDevice_GBAEmu::OnEvent(u64 userdata, s64 cycles_late)
{
  m_core->SyncJoybus(m_system.GetCoreTiming().GetTicks() + userdata, m_keys);

  const auto num_cycles = userdata + GetSyncInterval(m_system.GetSystemTimers());
  m_system.GetSerialInterface().ScheduleEvent(m_device_number, num_cycles);
}
}  // namespace SerialInterface
#endif  // HAS_LIBMGBA
