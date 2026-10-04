// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAS_LIBMGBA

#include "Core/HW/SI/SI_DeviceXDMultiPort.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE_XD.h"
#include "Core/HW/GBADetectLog.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_DeviceGBAEmu.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "Core/HW/SI/XDMultiCtl.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"

namespace SerialInterface
{
namespace
{
// XD USA (GXXE) main.dol. See the port-1 device spec for where each is written.
constexpr u32 XD_DISC_ID_GXXE = 0x47585845;
constexpr u32 XD_CTX = 0x804354A8;       // u32 ctx[4]: per-channel connection state
constexpr u32 XD_CTX_CH = 0x804354C9;    // u8: channel being connected
constexpr u32 XD_CTX_24 = 0x804354CC;    // log only
constexpr u32 XD_CTX_28 = 0x804354D0;    // u32: 0x8004F80C wait result
constexpr u32 XD_CTX_MODE = 0x804354D4;  // u32: flow mode, 3 = socket 1 must be a GBA
constexpr u32 XD_CTX_30 = 0x804354D8;    // u8, log only
constexpr u32 XD_CTX_48 = 0x804354F0;    // u8, log only
constexpr u32 XD_ERRFLAG = 0x804EA840;   // u8: set when the link error handler posts
constexpr u32 XD_BFLAG = 0x804EA834;     // u32: 1 while the VS battle runner runs
constexpr u32 XD_VSROW = 0x804349EC;
// The team combination index (XD's picker, state 0xAC) and the four battle slots' SI port (u32,
// 1-based, at +0x24 of each 0x1320 battle record). Battle setup lays the slots out from the
// picked row, slots 0-1 one side and 2-3 the other.
constexpr u32 XD_COMBO = 0x804349E4;
constexpr u32 XD_SLOT_PORT = 0x80429A34;
constexpr u32 XD_SLOT_STRIDE = 0x1320;
constexpr u32 XD_LAYOUT_MULTI = 3;
constexpr u32 XD_FORMAT = 0x80429A00;
constexpr u32 XD_LINK_A = 0x80428338;
constexpr u32 XD_LINK_B = 0x80428348;
constexpr u32 XD_PORT_MASK = 0x80445AFE;
constexpr u32 XD_PORT_RECS = 0x80444AF8;
constexpr u32 XD_PORT_REC_STRIDE = 0x7C;
constexpr u32 XD_SI_TYPE0 = 0x803D4E20;
constexpr u32 XD_SI_TT0 = 0x80442CD0;
constexpr u32 XD_SI_POLL = 0x803D4E10;
constexpr u32 XD_PAD_EN = 0x804EACBC;
constexpr u32 XD_PAD_RST = 0x804EACC0;
constexpr u32 XD_PAD_WAIT = 0x804EACC8;
constexpr u32 XD_RST_CH = 0x804E819C;
constexpr u32 XD_TYPE_TABLE = 0x803226C0;
constexpr u32 XD_TYPE_DEBOUNCE = 0x803226D0;

constexpr u32 CTX_PROMPT = 2;
constexpr u32 CTX_LINKING = 4;
constexpr u32 CTX_LINKED = 5;
constexpr u32 CTX_CANCELLED = 6;
constexpr u32 CTX_ERROR = 7;
constexpr u32 MODE_MULTI = 3;

constexpr u16 COMBO_BUTTONS = PAD_BUTTON_X | PAD_BUTTON_Y | PAD_TRIGGER_L | PAD_TRIGGER_R;
constexpr int STICK_DEADZONE = 48;
constexpr u8 TRIGGER_DIGITAL = 0xC0;
// A re-plugged GBA stays silent this many pops of this device (about 150 ms at XD's 120 polls a
// second, the old tick-based window), so XD's type table sees it absent.
constexpr u32 REPLUG_SILENT_POPS = 18;
// Post-battle, when the socket-1 wait hook is installed (HLE_XD): the hook replaces both the 1 s
// quiet rule and the 5 s late rule. Set this to true to keep the 5 s rule next to the hook (the
// fallback if a converted GBA turns out unable to drive XD's post-battle menu).
constexpr bool KEEP_POST_BATTLE_LATE_WITH_HOOK = false;

enum WantReason : u8
{
  WANT_NONE = 0,
  WANT_ERR,
  WANT_CANCEL,
  WANT_COMBO,
  WANT_POST_BATTLE,
  WANT_POST_BATTLE_LATE,
  WANT_MODE_EXIT,
  WANT_ORPHAN,
  WANT_LINK_LOST,
  WANT_SOCKET1_WAIT,
};

const char* WantReasonName(u8 reason)
{
  switch (reason)
  {
  case WANT_ERR:
    return "err";
  case WANT_CANCEL:
    return "cancel";
  case WANT_COMBO:
    return "combo";
  case WANT_POST_BATTLE:
    return "post-battle";
  case WANT_POST_BATTLE_LATE:
    return "post-battle-late";
  case WANT_MODE_EXIT:
    return "mode-exit";
  case WANT_ORPHAN:
    return "orphan";
  case WANT_LINK_LOST:
    return "link-lost";
  case WANT_SOCKET1_WAIT:
    return "socket1-wait";
  default:
    return "none";
  }
}

const char* ModeName(CSIDevice_XDMultiPort::Mode mode)
{
  switch (mode)
  {
  case CSIDevice_XDMultiPort::Mode::Pad:
    return "pad";
  case CSIDevice_XDMultiPort::Mode::Unplug:
    return "unplug";
  case CSIDevice_XDMultiPort::Mode::Gba:
    return "gba";
  }
  return "?";
}

// Timer starts use 0 as "not started".
u64 Stamp(u64 now)
{
  return now == 0 ? 1 : now;
}

u64 Since(u64 now, u64 then)
{
  return now > then ? now - then : 0;
}
}  // namespace

CSIDevice_XDMultiPort::CSIDevice_XDMultiPort(Core::System& system, SIDevices device,
                                             int device_number)
    : ISIDevice(system, device, device_number)
{
  m_controller =
      std::make_unique<CSIDevice_GCController>(system, SIDEVICE_GC_CONTROLLER, device_number);
  const u64 now = system.GetCoreTiming().GetTicks();
  if (device_number == 0)
  {
    m_gba = std::make_unique<CSIDevice_GBAEmu>(system, SIDEVICE_GC_GBA_EMULATED, device_number);
    m_mode_tick = now;
    // This game's CPU thread, before any pop; the previous game's has ended. A command the last
    // game posted and never applied must not reach this one.
    XDMultiCtl::Reset();
    m_boot_seq = Core::GetBootSequence();
    m_netplay = NetPlay::IsNetPlayRunning();
    m_is_local = !m_netplay || NetPlay::GetPadDetails(0).is_local;
    Log(now,
        fmt::format("init ch=0 mode=pad armed=1 netplay={} plans={}", m_netplay ? 1 : 0,
                    m_is_local ? 1 : 0),
        true);
  }
  else
  {
    Log(now, fmt::format("inert ch={}", device_number), true);
  }
}

CSIDevice_XDMultiPort::~CSIDevice_XDMultiPort() = default;

void CSIDevice_XDMultiPort::Log(u64 now, const std::string& detail, bool flush) const
{
  GBADetectLog::LogEvent(m_device_number, now, "xdmulti", detail, flush);
}

CSIDevice_XDMultiPort::XdRam CSIDevice_XDMultiPort::ReadXdRam() const
{
  XdRam ram;
  const auto& memory = m_system.GetMemory();
  if (memory.Read_U32(0x80000000) != XD_DISC_ID_GXXE)
    return ram;
  ram.is_xd = true;
  for (u32 i = 0; i < 4; ++i)
    ram.ctx[i] = memory.Read_U32(XD_CTX + 4 * i);
  ram.ctxch = memory.Read_U8(XD_CTX_CH);
  ram.ctx28 = memory.Read_U32(XD_CTX_28);
  ram.ctxmode = memory.Read_U32(XD_CTX_MODE);
  ram.errflag = memory.Read_U8(XD_ERRFLAG);
  ram.bflag = memory.Read_U32(XD_BFLAG);
  for (u32 i = 0; i < 4; ++i)
  {
    ram.link_a[i] = memory.Read_U32(XD_LINK_A + 4 * i);
    ram.link_b[i] = memory.Read_U32(XD_LINK_B + 4 * i);
  }
  return ram;
}

GCPadStatus CSIDevice_XDMultiPort::ToGba(const GCPadStatus& pad)
{
  constexpr u16 PASS = PAD_BUTTON_A | PAD_BUTTON_B | PAD_TRIGGER_Z | PAD_BUTTON_START |
                       PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_UP | PAD_BUTTON_DOWN;
  GCPadStatus out{};
  u16 button = pad.button & PASS;
  const int x = static_cast<int>(pad.stickX) - GCPadStatus::MAIN_STICK_CENTER_X;
  const int y = static_cast<int>(pad.stickY) - GCPadStatus::MAIN_STICK_CENTER_Y;
  if (x < -STICK_DEADZONE)
    button |= PAD_BUTTON_LEFT;
  if (x > STICK_DEADZONE)
    button |= PAD_BUTTON_RIGHT;
  if (y > STICK_DEADZONE)
    button |= PAD_BUTTON_UP;
  if (y < -STICK_DEADZONE)
    button |= PAD_BUTTON_DOWN;
  if ((pad.button & PAD_TRIGGER_L) || pad.triggerLeft >= TRIGGER_DIGITAL)
    button |= PAD_TRIGGER_L;
  if ((pad.button & PAD_TRIGGER_R) || pad.triggerRight >= TRIGGER_DIGITAL)
    button |= PAD_TRIGGER_R;
  out.button = button;
  return out;
}

u16 CSIDevice_XDMultiPort::KeysToPadButtons(u16 keys)
{
  // CSIDevice_GBAEmu::GbaKeysFromPad's order, inverted.
  static constexpr std::array<u16, 10> buttons = {
      PAD_BUTTON_A,     PAD_BUTTON_B,    PAD_TRIGGER_Z, PAD_BUTTON_START, PAD_BUTTON_RIGHT,
      PAD_BUTTON_LEFT,  PAD_BUTTON_UP,   PAD_BUTTON_DOWN, PAD_TRIGGER_R, PAD_TRIGGER_L};
  u16 out = 0;
  for (size_t i = 0; i < buttons.size(); ++i)
  {
    if ((keys >> i) & 1)
      out |= buttons[i];
  }
  return out;
}

CSIDevice_XDMultiPort::Sample CSIDevice_XDMultiPort::PopSynced()
{
  // Exactly one synced pad pop per call. Solo also reads the GBA window here: GetGBAStatus
  // consumes its reset request, so it is sampled once, never in netplay or movie playback.
  auto& movie = m_system.GetMovie();
  const bool netplay_now = NetPlay::IsNetPlayRunning();
  const bool solo = !netplay_now && !movie.IsPlayingInput();
  Sample sample;
  GCPadStatus gl{};
  if (!netplay_now)
    sample.pad = Pad::GetStatus(m_device_number);
  if (solo)
    gl = Pad::GetGBAStatus(m_device_number);
  CSIDevice_GCController::HandleMoviePadStatus(movie, m_device_number, &sample.pad);
  m_controller->ApplyOriginRequest(sample.pad);
  ++m_pops;
  if (m_silent_pops > 0)
    --m_silent_pops;

  if (m_netplay && netplay_now)
  {
    // The Multi wire (XDMultiCtl.h): keys and control in the analog bytes, which XD then sees
    // rebuilt from the A/B buttons exactly as GCPadEmu builds them.
    const u8 a = sample.pad.analogA;
    const u8 b = sample.pad.analogB;
    sample.reset = (b & XDMultiCtl::WIRE_RESET) != 0;
    sample.cmd = (b & XDMultiCtl::WIRE_CMD_MASK) >> XDMultiCtl::WIRE_CMD_SHIFT;
    sample.keys = static_cast<u16>(
        a | (((b & XDMultiCtl::WIRE_KEYS_HI_MASK) >> XDMultiCtl::WIRE_KEYS_HI_SHIFT) << 8));
    sample.pad.analogA = (sample.pad.button & PAD_BUTTON_A) ? 0xFF : 0x00;
    sample.pad.analogB = (sample.pad.button & PAD_BUTTON_B) ? 0xFF : 0x00;
  }
  else
  {
    sample.keys = CSIDevice_GBAEmu::GbaKeysFromPad(gl) & 0x3FF;
    sample.reset = (gl.button & PAD_BUTTON_X) != 0;
  }
  m_last_buttons = sample.pad.button;
  return sample;
}

void CSIDevice_XDMultiPort::ApplyCtl(const Sample& sample, u64 now)
{
  if (sample.cmd == XDMultiCtl::CMD_NONE)
    return;
  // No latch and no other condition: every machine pops this entry at the same poll and runs the
  // same command there. A GBA transfer still in flight drains to the inner GBA (RunBuffer).
  Execute(sample.cmd, XDMultiCtl::CommandName(sample.cmd), now);
  if (m_is_local)
    XDMultiCtl::MarkApplied();
}

void CSIDevice_XDMultiPort::ApplyGbaInput(const Sample& sample, bool keys_active)
{
  // The GBA sees its keys only while port 1 is a GBA; the Reset (X) reaches it in every mode,
  // like the plain GBA device's.
  GCPadStatus g = keys_active ? ToGba(sample.pad) : GCPadStatus{};
  if (keys_active)
    g.button |= KeysToPadButtons(sample.keys);
  if (sample.reset)
    g.button |= PAD_BUTTON_X;
  m_gba->ApplyPadStatus(g);
}

void CSIDevice_XDMultiPort::EvaluateTransitions(u64 now, bool en0, const XdRam& ram)
{
  const u64 tps = m_system.GetSystemTimers().GetTicksPerSecond();

  const bool at_prompt = ram.is_xd && ram.ctxmode == MODE_MULTI && ram.ctx[0] == CTX_PROMPT &&
                         ram.ctxch == 0;
  // XD is still waiting at the prompt: 0x8004F80C clears ctx28 on entry and
  // returns at once while any ctx is 7. An error abort from the prompt leaves
  // ctx[0] == 2 behind with ctx28 == 8 and a ctx at 7 until the next flow.
  bool prompt_live = at_prompt && ram.ctx28 == 0;
  for (int i = 0; i < 4; ++i)
  {
    if (ram.ctx[i] == CTX_ERROR)
      prompt_live = false;
  }

  // Error edge: a ctx turning 7 (primary) or the error flag rising.
  bool err_edge = false;
  int err_ctx = -1;
  if (ram.is_xd && ram.ctxmode == MODE_MULTI)
  {
    for (int i = 0; i < 4; ++i)
    {
      if (ram.ctx[i] == CTX_ERROR && m_ctx_prev[i] != CTX_ERROR)
      {
        err_edge = true;
        err_ctx = i;
        break;
      }
    }
    if (!err_edge && ram.errflag == 1 && m_errflag_prev == 0)
      err_edge = true;
  }

  // Escape combo: X+Y+L+R held continuously for 3 s; fires once per hold.
  bool combo_fire = false;
  const bool combo_now = (m_last_buttons & COMBO_BUTTONS) == COMBO_BUTTONS;
  if (combo_now && m_combo_released)
  {
    if (m_combo_since == 0)
    {
      m_combo_since = Stamp(now);
    }
    else if (Since(now, m_combo_since) >= 3 * tps)
    {
      combo_fire = true;
      m_combo_released = false;
      m_combo_since = 0;
    }
  }
  else if (!combo_now)
  {
    m_combo_since = 0;
  }
  if ((m_last_buttons & COMBO_BUTTONS) == 0)
  {
    m_combo_released = true;
    m_combo_since = 0;
  }

  if (ram.ctx[0] != CTX_CANCELLED)
    m_replug6_done = false;

  if (!ram.is_xd && m_mode != Mode::Pad)
  {
    if (!m_gba_pending)
      Decide(XDMultiCtl::CMD_TO_PAD, "not-xd", now);
  }
  else
  {
    switch (m_mode)
    {
    case Mode::Pad:
      EvaluatePad(ram, at_prompt, prompt_live, combo_fire, now);
      break;
    case Mode::Unplug:
      EvaluateUnplug(ram, at_prompt, err_edge, combo_fire, en0, now);
      break;
    case Mode::Gba:
      EvaluateGba(ram, err_edge, err_ctx, combo_fire, now);
      break;
    }
  }

  m_ctx_prev = ram.ctx;
  m_errflag_prev = ram.errflag;
  m_bflag_prev = ram.bflag;
}

void CSIDevice_XDMultiPort::EvaluatePad(const XdRam& ram, bool at_prompt, bool prompt_live,
                                        bool combo_fire, u64 now)
{
  if (!at_prompt)
  {
    m_armed = true;
    m_trigger_streak = 0;
    if (combo_fire)
      Log(now, "combo-ignored");
    return;
  }
  if (m_armed)
  {
    if (m_trigger_streak == 0)
      Log(now, "trigger-seen");
    ++m_trigger_streak;
    if (m_trigger_streak >= 2)
    {
      m_armed = false;
      Decide(XDMultiCtl::CMD_TO_UNPLUG, "socket1", now);
    }
    return;
  }
  // Manual re-trigger after a combo escape, only while XD still waits there.
  if (combo_fire)
  {
    if (!prompt_live)
    {
      Log(now, "combo-ignored");
      return;
    }
    Log(now, "combo-fired");
    Decide(XDMultiCtl::CMD_TO_UNPLUG, "combo", now);
  }
}

void CSIDevice_XDMultiPort::EvaluateUnplug(const XdRam& ram, bool at_prompt, bool err_edge,
                                           bool combo_fire, bool en0, u64 now)
{
  const u64 tps = m_system.GetSystemTimers().GetTicksPerSecond();
  if (err_edge)
  {
    Decide(XDMultiCtl::CMD_TO_PAD, "err", now);
    return;
  }
  if (combo_fire)
  {
    Log(now, "combo-fired");
    Decide(XDMultiCtl::CMD_TO_PAD, "combo", now);
    return;
  }
  if (!at_prompt)
  {
    if (m_cond_since == 0)
      m_cond_since = Stamp(now);
    if (Since(now, m_cond_since) >= tps / 2)
      Decide(XDMultiCtl::CMD_TO_PAD, "cancel", now);
    return;
  }
  m_cond_since = 0;
  if (!en0)
  {
    Decide(XDMultiCtl::CMD_TO_GBA, "en0-clear", now);
    return;
  }
  if (Since(now, m_mode_tick) >= 2 * tps)
  {
    // The snapshot carries EN1-3 and SISR.
    Log(now, fmt::format("unplug-stuck {}", Snapshot(now)), true);
    Decide(XDMultiCtl::CMD_TO_GBA, "unplug-timeout", now);
  }
}

void CSIDevice_XDMultiPort::EvaluateGba(const XdRam& ram, bool err_edge, int err_ctx,
                                        bool combo_fire, u64 now)
{
  const u64 tps = m_system.GetSystemTimers().GetTicksPerSecond();

  // Battle bookkeeping.
  if (ram.bflag != 0 && m_bflag_prev == 0)
  {
    m_battle_seen = true;
    Log(now, "battle start");
  }
  else if (ram.bflag == 0 && m_bflag_prev != 0)
  {
    m_battle_end_tick = now;
    Log(now, "battle end");
  }

  const u64 quiet = Since(now, std::max(m_last_data_tick, m_mode_tick));

  // Cancel: XD parked a cancelled socket (B, or an earlier accept) in its
  // "controller in socket 1" wait. Never ctx 4 and never ctx28 == 0.
  const bool cancel_cond = ram.ctxmode == MODE_MULTI && ram.ctxch <= 3 &&
                           ram.ctx[ram.ctxch] == CTX_CANCELLED &&
                           (ram.ctx28 == 2 || ram.ctx28 == 4);
  if (!cancel_cond)
    m_cond_since = 0;
  else if (m_cond_since == 0)
    m_cond_since = Stamp(now);
  const bool cancel = cancel_cond && Since(now, m_cond_since) >= tps / 4;

  // Link lost after the connect flow, before the battle. XD marks a dropped
  // socket 7 only inside the connect flow (0x8004F6A0); later drops go to the
  // menu error path 0x80081724, which waits for a socket 1 controller. Same
  // test as 0x8004F6A0: a socket at 4 or 5 whose port is not linked
  // (0x80028C5C: link A != 0 and link B == 0).
  bool link_lost_cond = false;
  int lost_socket = -1;
  if (!m_battle_seen && ram.bflag == 0 && ram.ctxmode == MODE_MULTI && ram.ctxch == 0 &&
      ram.ctx[0] == CTX_LINKED)
  {
    for (int i = 0; i < 4; ++i)
    {
      const bool linked = ram.link_a[i] != 0 && ram.link_b[i] == 0;
      if ((ram.ctx[i] == CTX_LINKING || ram.ctx[i] == CTX_LINKED) && !linked)
      {
        link_lost_cond = true;
        lost_socket = i;
        break;
      }
    }
  }
  if (!link_lost_cond)
    m_link_lost_since = 0;
  else if (m_link_lost_since == 0)
    m_link_lost_since = Stamp(now);
  const bool link_lost = link_lost_cond && Since(now, m_link_lost_since) >= tps;

  // XD waits for the socket-1 controller (the HLE counter, HLE_XD.h): at least two calls since
  // the last attach or re-plug, the latest within 100 ms. While the hook is installed it replaces
  // the post-battle timing rules below: XD itself says when it wants the controller back, so
  // "battle again" and "change combination" keep the GBA and nothing flips at an idle menu.
  const HLE_XD::Socket1Wait s1 = HLE_XD::GetSocket1Wait();
  const bool socket1_wait =
      s1.installed && s1.calls - m_s1_base >= 2 && Since(now, s1.tick) <= tps / 10;
  const bool post_battle_quiet_rule = !s1.installed;
  const bool post_battle_late_rule = !s1.installed || KEEP_POST_BATTLE_LATE_WITH_HOOK;

  if (!m_want_pad)
  {
    u8 reason = WANT_NONE;
    if (err_edge)
      reason = WANT_ERR;
    else if (socket1_wait)
      reason = WANT_SOCKET1_WAIT;
    else if (cancel)
      reason = WANT_CANCEL;
    else if (combo_fire)
      reason = WANT_COMBO;
    else if (link_lost)
      reason = WANT_LINK_LOST;
    else if (post_battle_quiet_rule && m_battle_seen && ram.bflag == 0 &&
             Since(now, m_battle_end_tick) >= tps && quiet >= tps)
      reason = WANT_POST_BATTLE;
    else if (post_battle_late_rule && m_battle_seen && ram.bflag == 0 &&
             Since(now, m_battle_end_tick) >= 5 * tps)
      reason = WANT_POST_BATTLE_LATE;
    else if (ram.ctxmode != MODE_MULTI && ram.bflag == 0 && quiet >= 4 * tps)
      reason = WANT_MODE_EXIT;
    else if (m_data_seen && !m_battle_seen && quiet >= 20 * tps && m_probe_fail_streak >= 600)
      reason = WANT_ORPHAN;

    if (reason != WANT_NONE)
    {
      m_want_pad = true;
      m_want_reason = reason;
      if (reason == WANT_COMBO)
        Log(now, "combo-fired");
      std::string detail;
      if (reason == WANT_ERR)
        detail = fmt::format(" ctx7={}", err_ctx);
      else if (reason == WANT_LINK_LOST)
        detail = fmt::format(" ch={}", lost_socket);
      else if (reason == WANT_SOCKET1_WAIT)
        detail = fmt::format(" lr={:08x} calls={}", s1.lr, s1.calls - m_s1_base);
      Log(now,
          fmt::format("want-pad reason={}{} pend={}", WantReasonName(reason), detail,
                      m_gba_pending ? 1 : 0),
          true);
    }
  }
  else if (combo_fire)
  {
    Log(now, "combo-ignored");
  }

  if (m_want_pad && !m_gba_pending)
  {
    Decide(XDMultiCtl::CMD_TO_PAD, WantReasonName(m_want_reason), now);
    return;
  }

  // Re-plug: XD re-enters the socket-1 wait while the GBA may still read as
  // present in its type table, which only 3+ non-GBA updates clear.
  const bool retry6 = ram.ctx[0] == CTX_CANCELLED && ram.ctx28 == 0;
  if (retry6)
    ++m_trigger_streak;
  else
    m_trigger_streak = 0;

  if (m_gba_pending || m_want_pad || ram.ctxmode != MODE_MULTI || ram.ctxch != 0)
    return;

  int kind = 0;
  if (ram.ctx[0] == CTX_PROMPT && m_ctx_prev[0] != CTX_PROMPT)
    kind = 2;
  else if (retry6 && m_trigger_streak >= 2 && !m_replug6_done)
    kind = 6;
  if (kind == 0)
    return;

  if (kind == 6)
    m_replug6_done = true;
  Decide(XDMultiCtl::CMD_REPLUG, kind == 6 ? "replug-6" : "replug-2", now);
}

void CSIDevice_XDMultiPort::Decide(u8 cmd, const char* reason, u64 now)
{
  if (!m_netplay)
  {
    // Solo: the switch lands on the poll that decided it (the field-proven timing).
    Execute(cmd, reason, now);
    return;
  }
  // Netplay host: the next channel-0 entry pushed carries it; every machine applies it at the pop
  // of that entry. The caller only plans while nothing is waiting or in flight.
  if (XDMultiCtl::Post(cmd, m_boot_seq))
  {
    Log(now,
        fmt::format("post cmd={} reason={} pop={} mode={}", XDMultiCtl::CommandName(cmd), reason,
                    m_pops, ModeName(m_mode)),
        true);
  }
}

void CSIDevice_XDMultiPort::Execute(u8 cmd, const char* reason, u64 now)
{
  const Mode from = m_mode;
  bool valid = false;
  switch (cmd)
  {
  case XDMultiCtl::CMD_TO_UNPLUG:
    valid = from == Mode::Pad;
    break;
  case XDMultiCtl::CMD_TO_GBA:
    valid = from == Mode::Unplug || from == Mode::Pad;
    break;
  case XDMultiCtl::CMD_TO_PAD:
    valid = from == Mode::Gba || from == Mode::Unplug;
    break;
  case XDMultiCtl::CMD_REPLUG:
    valid = from == Mode::Gba;
    break;
  default:
    break;
  }
  // The same line on every machine of a netplay game (nothing per-machine in it).
  if (m_netplay)
  {
    Log(now, fmt::format("apply cmd={} pop={} mode={}{}", XDMultiCtl::CommandName(cmd), m_pops,
                         ModeName(from), valid ? "" : " invalid"),
        true);
  }
  if (!valid)
    return;

  switch (cmd)
  {
  case XDMultiCtl::CMD_TO_UNPLUG:
    SwitchMode(Mode::Unplug, reason, now);
    break;
  case XDMultiCtl::CMD_TO_GBA:
    SwitchMode(Mode::Gba, reason, now);
    break;
  case XDMultiCtl::CMD_TO_PAD:
    SwitchMode(Mode::Pad, reason, now);
    break;
  case XDMultiCtl::CMD_REPLUG:
    Replug(reason, now);
    break;
  default:
    break;
  }
}

void CSIDevice_XDMultiPort::Replug(const char* reason, u64 now)
{
  m_silent_pops = REPLUG_SILENT_POPS;
  m_s1_base = HLE_XD::GetSocket1Wait().calls;
  m_gba->ReleaseLinkLatches("xdmulti-replug");
  // Anchored where the X-button reset anchors: the last command sent to this GBA.
  m_gba->RequestSyncedReset(m_gba->GetTimestampSent(), "xdmulti-replug");
  Log(now, fmt::format("replug {} {}", reason, Snapshot(now)), true);
}

void CSIDevice_XDMultiPort::SwitchMode(Mode to, const char* reason, u64 now)
{
  // No transfer condition: a GBA transfer in flight drains to the inner GBA (RunBuffer).
  if (to == m_mode)
    return;

  const u64 tps = m_system.GetSystemTimers().GetTicksPerSecond();
  const Mode from = m_mode;
  const u64 dur_ms = Since(now, m_mode_tick) / std::max<u64>(tps / 1000, 1);
  const std::string snapshot = Snapshot(now);

  m_mode = to;
  m_mode_tick = now;
  m_cond_since = 0;
  m_link_lost_since = 0;
  m_want_pad = false;
  m_want_reason = WANT_NONE;
  m_trigger_streak = 0;
  m_silent_pops = 0;

  Log(now,
      fmt::format("mode from={} to={} reason={} dur_ms={} armed={} {}", ModeName(from),
                  ModeName(to), reason, dur_ms, m_armed ? 1 : 0, snapshot),
      true);

  if (to == Mode::Gba)
  {
    m_data_seen = false;
    m_battle_seen = false;
    m_probe_fail_streak = 0;
    m_last_data_tick = 0;
    // The prompt's own 2 is not a re-plug.
    m_ctx_prev[0] = ReadXdRam().ctx[0];
    m_s1_base = HLE_XD::GetSocket1Wait().calls;
    m_gba->ReleaseLinkLatches("xdmulti-attach");
    // A fresh Emerald boot, the same path sockets 2-4 take, even on a rematch
    // where the old client is still running. Anchored where the X-button reset anchors.
    m_gba->RequestSyncedReset(m_gba->GetTimestampSent(), "xdmulti-attach");
    m_diag_ctx_lines = 0;
    m_diag_padok_armed = false;
  }
  else if (to == Mode::Pad)
  {
    if (from == Mode::Gba)
      m_gba->ReleaseLinkLatches("xdmulti-detach");
    // Fresh controller: clears the combo timer and restores the default
    // origin. XD's PADReset re-sends the analog mode.
    m_controller =
        std::make_unique<CSIDevice_GCController>(m_system, SIDEVICE_GC_CONTROLLER, m_device_number);
    m_diag_pad_since = Stamp(now);
    m_diag_padok_armed = true;
    m_diag_padwait_logged = false;
    m_diag_padok_start = now;
  }
}

DataResponse CSIDevice_XDMultiPort::GetData(u32& hi, u32& low)
{
  m_fresh = false;
  if (!m_gba)
    return m_controller->GetData(hi, low);

  const u64 now = m_system.GetCoreTiming().GetTicks();

  // 1. The pop, and the command that rode with it (netplay).
  const Sample sample = PopSynced();
  ApplyCtl(sample, now);

  // 2. The planner: solo, or the netplay host while no command is waiting or in flight. It sees
  // the mode every machine has at this pop.
  const bool en0 = m_system.GetSerialInterface().IsChannelPollEnabled(m_device_number);
  const XdRam ram = ReadXdRam();
  if (m_is_local && (!m_netplay || !XDMultiCtl::Busy()))
    EvaluateTransitions(now, en0, ram);
  LogDiagnostics(ram, en0, now);

  // 3. Respond in the committed mode with the popped status.
  if (m_mode == Mode::Pad)
  {
    // The field-proven controller, including Success while EN0 is clear.
    const DataResponse r = m_controller->GetDataFromStatus(sample.pad, hi, low);
    ApplyGbaInput(sample, /*keys_active=*/false);
    return r;
  }

  if (m_mode == Mode::Unplug)
  {
    hi = 0;
    low = 0;
    ApplyGbaInput(sample, /*keys_active=*/false);
    return en0 ? DataResponse::ErrorNoResponseReady : DataResponse::NoData;
  }

  ApplyGbaInput(sample, /*keys_active=*/true);
  if (en0)
  {
    hi = 0;
    low = 0;
    return DataResponse::ErrorNoResponseReady;
  }
  return DataResponse::NoData;
}

int CSIDevice_XDMultiPort::RunBuffer(u8* buffer, int request_length)
{
  m_fresh = false;
  if (!m_gba)
    return m_controller->RunBuffer(buffer, request_length);

  const u64 now = m_system.GetCoreTiming().GetTicks();
  const u8 cmd = buffer[0];

  // A transfer in flight finishes on the GBA whatever the mode, so a switch applied at a pop never
  // cuts one short, on any machine.
  if (m_gba_pending)
    return RunGba(buffer, request_length, now);

  switch (m_mode)
  {
  case Mode::Pad:
    switch (static_cast<EBufferCommands>(cmd))
    {
    case EBufferCommands::CMD_STATUS:
    case EBufferCommands::CMD_RESET:
    case EBufferCommands::CMD_DIRECT:
    case EBufferCommands::CMD_ORIGIN:
    case EBufferCommands::CMD_RECALIBRATE:
    {
      // The pad's own pop, as the plain controller's RunBuffer does it, and any command on it.
      const Sample sample = PopSynced();
      ApplyCtl(sample, now);
      if (sample.reset)
        ApplyGbaInput(sample, /*keys_active=*/false);
      if (m_mode == Mode::Unplug)
        return -1;
      if (m_mode == Mode::Gba)
        return RunGba(buffer, request_length, now);
      if (static_cast<EBufferCommands>(cmd) == EBufferCommands::CMD_DIRECT)
      {
        // One pop per command: the data comes from this sample, not from a second pop.
        u32 high = 0;
        u32 low = 0;
        if (!sample.pad.isConnected)
          return -1;
        m_controller->GetDataFromStatus(sample.pad, high, low);
        for (int i = 0; i < 4; i++)
        {
          buffer[i + 0] = (high >> (24 - (i * 8))) & 0xff;
          buffer[i + 4] = (low >> (24 - (i * 8))) & 0xff;
        }
        return sizeof(high) + sizeof(low);
      }
      return m_controller->RunBufferWithStatus(buffer, request_length, sample.pad);
    }
    default:
      // GBA link traffic (0x14/0x15/0x1D...) finds no GBA here.
      ++m_diag_pad_rejects;
      if (!m_diag_reject_logged[cmd])
      {
        m_diag_reject_logged[cmd] = true;
        Log(now, fmt::format("pad-reject cmd={:02x}", cmd));
      }
      return -1;
    }

  case Mode::Unplug:
    return -1;

  case Mode::Gba:
    break;
  }

  return RunGba(buffer, request_length, now);
}

int CSIDevice_XDMultiPort::RunGba(u8* buffer, int request_length, u64 now)
{
  const u8 cmd = buffer[0];
  if (m_silent_pops > 0 && !m_gba_pending)
  {
    ++m_diag_silent_rejects;
    return -1;
  }

  if (!m_gba_pending)
    m_pending_cmd = cmd;
  const int r = m_gba->RunBuffer(buffer, request_length);
  m_gba_pending = r == 0;

  const u8 sent = m_pending_cmd;
  const bool data_cmd = sent == static_cast<u8>(EBufferCommands::CMD_READ_GBA) ||
                        sent == static_cast<u8>(EBufferCommands::CMD_WRITE_GBA);
  const bool probe_cmd = sent == static_cast<u8>(EBufferCommands::CMD_STATUS) ||
                         sent == static_cast<u8>(EBufferCommands::CMD_RESET);
  if (r > 0 && data_cmd)
  {
    m_last_data_tick = now;
    if (!m_data_seen)
    {
      m_data_seen = true;
      Log(now, fmt::format("first-data cmd={:02x}", sent));
    }
  }
  if (r > 0)
    m_probe_fail_streak = 0;
  else if (r < 0 && probe_cmd && m_probe_fail_streak < 0xffffffffu)
    ++m_probe_fail_streak;
  return r;
}

int CSIDevice_XDMultiPort::TransferInterval()
{
  if (!m_gba)
    return m_controller->TransferInterval();
  const bool silent = m_silent_pops > 0 && !m_gba_pending;
  if (m_gba_pending || (m_mode == Mode::Gba && !silent))
    return m_gba->TransferInterval();
  return m_controller->TransferInterval();
}

void CSIDevice_XDMultiPort::SendCommand(u32 command, u8 poll)
{
  // Only the pad sees direct commands; anything else would PanicAlert in it.
  if (m_gba && m_mode != Mode::Pad)
    return;

  m_controller->SendCommand(command, poll);

  // The controller's own rumble is a no-op for this device type, so drive the
  // local pad directly, mirroring CSIDevice_GCController::SendCommand.
  const UCommand controller_command(command);
  if (static_cast<EDirectCommands>(controller_command.command) == EDirectCommands::CMD_WRITE)
  {
    const int pad_num = CSIDevice_GCController::NetPlay_InGamePadToLocalPad(m_device_number);
    if (pad_num < 4 &&
        m_system.GetSerialInterface().GetDeviceType(pad_num) == SIDEVICE_GC_GBA_XDMULTI)
    {
      Pad::Rumble(pad_num, controller_command.parameter1 == 1 ? 1.0 : 0.0);
    }
  }
}

void CSIDevice_XDMultiPort::OnEvent(u64 userdata, s64 cycles_late)
{
  m_fresh = false;
  // The channel's sync event always belongs to the GBA core.
  if (m_gba)
    m_gba->OnEvent(userdata, cycles_late);
}

void CSIDevice_XDMultiPort::DoState(PointerWrap& p)
{
  p.Do(m_mode);
  p.Do(m_mode_tick);
  p.Do(m_armed);
  p.Do(m_trigger_streak);
  p.Do(m_gba_pending);
  p.Do(m_pending_cmd);
  p.Do(m_data_seen);
  p.Do(m_last_data_tick);
  p.Do(m_probe_fail_streak);
  p.Do(m_battle_seen);
  p.Do(m_bflag_prev);
  p.Do(m_battle_end_tick);
  p.Do(m_ctx_prev);
  p.Do(m_errflag_prev);
  p.Do(m_want_pad);
  p.Do(m_want_reason);
  p.Do(m_cond_since);
  p.Do(m_replug6_done);
  p.Do(m_last_buttons);
  p.Do(m_combo_since);
  p.Do(m_combo_released);
  p.Do(m_link_lost_since);
  p.DoMarker("XDMultiPort2");
  p.Do(m_silent_pops);
  p.Do(m_pops);
  p.Do(m_s1_base);

  m_controller->DoState(p);
  if (m_gba)
    m_gba->DoState(p);

  if (p.IsReadMode())
  {
    if (m_fresh && m_gba)
    {
      // This device was created by SerialInterfaceManager::DoState for this
      // load, after CoreTiming restored the saved sync event: drop the
      // duplicate from the constructor (or a removal by the old device's
      // destructor) and keep exactly one chain.
      auto& si = m_system.GetSerialInterface();
      si.RemoveEvent(m_device_number);
      si.ScheduleEvent(m_device_number, m_system.GetSystemTimers().GetTicksPerSecond() / 1000);
    }
    m_diag_have_prev = false;
    m_diag_padok_armed = false;
    Log(m_system.GetCoreTiming().GetTicks(),
        fmt::format("state-load mode={} fresh={}", ModeName(m_mode), m_fresh ? 1 : 0), true);
  }
  m_fresh = false;
}

std::string CSIDevice_XDMultiPort::Snapshot(u64 now) const
{
  const auto& memory = m_system.GetMemory();
  const auto& si = m_system.GetSerialInterface();
  const u64 tps = m_system.GetSystemTimers().GetTicksPerSecond();
  const u64 ms = std::max<u64>(tps / 1000, 1);
  const u32 sisr = si.GetStatusRegisterForDiag();

  std::string out;
  if (memory.Read_U32(0x80000000) == XD_DISC_ID_GXXE)
  {
    out = fmt::format(
        "vs={:08x} ctx={:08x},{:08x},{:08x},{:08x} ch={} c28={} c24={:08x} cmode={} s30={:02x} "
        "s48={:02x} fmt={:08x} bflag={} ef={} mask={:02x}",
        memory.Read_U32(XD_VSROW), memory.Read_U32(XD_CTX), memory.Read_U32(XD_CTX + 4),
        memory.Read_U32(XD_CTX + 8), memory.Read_U32(XD_CTX + 12), memory.Read_U8(XD_CTX_CH),
        memory.Read_U32(XD_CTX_28), memory.Read_U32(XD_CTX_24), memory.Read_U32(XD_CTX_MODE),
        memory.Read_U8(XD_CTX_30), memory.Read_U8(XD_CTX_48), memory.Read_U32(XD_FORMAT),
        memory.Read_U32(XD_BFLAG), memory.Read_U8(XD_ERRFLAG), memory.Read_U8(XD_PORT_MASK));
    out += fmt::format(" type0={:08x} tt0={:08x} poll={:08x} padEn={:08x} padRst={:08x} "
                       "padWait={:08x} rstCh={:08x}",
                       memory.Read_U32(XD_SI_TYPE0), memory.Read_U32(XD_SI_TT0),
                       memory.Read_U32(XD_SI_POLL), memory.Read_U32(XD_PAD_EN),
                       memory.Read_U32(XD_PAD_RST), memory.Read_U32(XD_PAD_WAIT),
                       memory.Read_U32(XD_RST_CH));
    out += fmt::format(" tbl={:08x},{:08x},{:08x},{:08x} deb={:08x},{:08x},{:08x},{:08x}",
                       memory.Read_U32(XD_TYPE_TABLE), memory.Read_U32(XD_TYPE_TABLE + 4),
                       memory.Read_U32(XD_TYPE_TABLE + 8), memory.Read_U32(XD_TYPE_TABLE + 12),
                       memory.Read_U32(XD_TYPE_DEBOUNCE), memory.Read_U32(XD_TYPE_DEBOUNCE + 4),
                       memory.Read_U32(XD_TYPE_DEBOUNCE + 8),
                       memory.Read_U32(XD_TYPE_DEBOUNCE + 12));
    for (u32 p = 0; p < 4; ++p)
      out += fmt::format(" lA{}={:08x}", p, memory.Read_U32(XD_LINK_A + 4 * p));
    for (u32 p = 0; p < 4; ++p)
      out += fmt::format(" lB{}={:08x}", p, memory.Read_U32(XD_LINK_B + 4 * p));
    for (u32 i = 0; i < 4; ++i)
    {
      const u32 rec = XD_PORT_RECS + XD_PORT_REC_STRIDE * i;
      out += fmt::format(" rec{}={{{},{:08x},{:08x},{}}}", i, memory.Read_U32(rec),
                         memory.Read_U32(rec + 4), memory.Read_U32(rec + 0xC),
                         static_cast<s8>(memory.Read_U8(rec + 0x3E)));
    }
  }
  else
  {
    out = "not-xd";
  }
  out += fmt::format(" en0={} en1={} en2={} en3={} sisr={:08x}",
                     si.IsChannelPollEnabled(0) ? 1 : 0, si.IsChannelPollEnabled(1) ? 1 : 0,
                     si.IsChannelPollEnabled(2) ? 1 : 0, si.IsChannelPollEnabled(3) ? 1 : 0,
                     sisr & 0x38383838u);
  const u64 quiet_base = std::max(m_last_data_tick, m_mode_tick);
  out += fmt::format(" pend={} silent={} quiet_ms={} pfs={} linkdiag(racy)={}",
                     m_gba_pending ? 1 : 0, m_silent_pops,
                     Since(now, quiet_base) / ms, m_probe_fail_streak,
                     (m_gba && m_gba->IsLinkUpForDiag()) ? 1 : 0);
  return out;
}

void CSIDevice_XDMultiPort::LogBattleSides(u64 now)
{
  const auto& memory = m_system.GetMemory();
  std::array<u32, 4> ports{};
  u32 seen = 0;
  bool valid = memory.Read_U32(XD_VSROW) == XD_LAYOUT_MULTI;
  for (u32 k = 0; k < 4; ++k)
  {
    ports[k] = memory.Read_U32(XD_SLOT_PORT + k * XD_SLOT_STRIDE);
    if (ports[k] < 1 || ports[k] > 4 || (seen & (1u << ports[k])) != 0)
      valid = false;
    else
      seen |= 1u << ports[k];
  }
  valid = valid && ports[0] == 1;
  Log(now, fmt::format("sides combo={} slots={},{},{},{} valid={}", memory.Read_U32(XD_COMBO),
                       ports[0], ports[1], ports[2], ports[3], valid ? 1 : 0));
  if (!valid || !m_netplay || !m_is_local)
    return;
  u8 packed = 0;
  for (u32 k = 0; k < 4; ++k)
    packed |= static_cast<u8>((ports[k] - 1) << (2 * k));
  NetPlay::ReportXdMultiSides(packed);
}

void CSIDevice_XDMultiPort::LogDiagnostics(const XdRam& ram, bool en0, u64 now)
{
  const u64 tps = m_system.GetSystemTimers().GetTicksPerSecond();
  const u64 ms = std::max<u64>(tps / 1000, 1);

  // The teams, once per battle (battle again and change combination included), on every machine
  // at the same poll: reads only. The port-1 owner sends them to the room for the chat line.
  if (ram.is_xd && ram.bflag != 0 && m_diag_prev_bflag == 0)
    LogBattleSides(now);
  m_diag_prev_bflag = ram.bflag;

  if (m_diag_have_prev && en0 != m_diag_prev_en0 && m_diag_en0_lines < 256)
  {
    ++m_diag_en0_lines;
    Log(now, fmt::format("en0 {} mode={}", en0 ? 1 : 0, ModeName(m_mode)));
  }

  if (m_mode == Mode::Gba && ram.is_xd && m_diag_have_prev && m_diag_ctx_lines < 64 &&
      (ram.ctx != m_diag_prev_ctx || ram.ctx28 != m_diag_prev_ctx28 ||
       ram.errflag != m_diag_prev_errflag))
  {
    ++m_diag_ctx_lines;
    Log(now, fmt::format("ctx {:08x},{:08x},{:08x},{:08x} ch={} c28={} cmode={} ef={} bflag={}",
                         ram.ctx[0], ram.ctx[1], ram.ctx[2], ram.ctx[3], ram.ctxch, ram.ctx28,
                         ram.ctxmode, ram.errflag, ram.bflag));
  }

  // pad-ok watch after a switch back to the pad: XD released socket 1 once its
  // port-1 record reads +0xC == 0 and +4 == 0.
  if (m_mode == Mode::Pad && m_diag_padok_armed && ram.is_xd)
  {
    const auto& memory = m_system.GetMemory();
    bool released = false;
    for (u32 i = 0; i < 4; ++i)
    {
      const u32 rec = XD_PORT_RECS + XD_PORT_REC_STRIDE * i;
      if (memory.Read_U32(rec) == 1)
      {
        released = memory.Read_U32(rec + 0xC) == 0 && memory.Read_U32(rec + 4) == 0;
        break;
      }
    }
    if (released)
    {
      m_diag_padok_armed = false;
      Log(now,
          fmt::format("pad-ok ms={} {}", Since(now, m_diag_padok_start) / ms, Snapshot(now)),
          true);
    }
    else if (!m_diag_padwait_logged && Since(now, m_diag_padok_start) >= 5 * tps)
    {
      m_diag_padwait_logged = true;
      Log(now, fmt::format("pad-wait {}", Snapshot(now)), true);
    }
  }
  else if (m_mode != Mode::Pad)
  {
    m_diag_padok_armed = false;
  }

  const bool sum_window =
      m_mode != Mode::Pad || (m_diag_pad_since != 0 && Since(now, m_diag_pad_since) < 10 * tps);
  if (sum_window && (m_diag_last_sum == 0 || Since(now, m_diag_last_sum) >= 2 * tps))
  {
    const u64 elapsed = m_diag_last_sum == 0 ? 0 : Since(now, m_diag_last_sum);
    const u64 per_s = elapsed == 0 ? 0 : m_diag_pad_rejects * tps / elapsed;
    Log(now, fmt::format("sum mode={} armed={} want={} rej={} rej_per_s={} silent_rej={} {}",
                         ModeName(m_mode), m_armed ? 1 : 0, WantReasonName(m_want_reason),
                         m_diag_pad_rejects, per_s, m_diag_silent_rejects, Snapshot(now)),
        true);
    m_diag_last_sum = now;
    m_diag_pad_rejects = 0;
    m_diag_silent_rejects = 0;
  }

  m_diag_prev_en0 = en0;
  m_diag_prev_ctx28 = ram.ctx28;
  m_diag_prev_ctx = ram.ctx;
  m_diag_prev_errflag = ram.errflag;
  m_diag_have_prev = true;
}
}  // namespace SerialInterface

#endif  // HAS_LIBMGBA
