// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAS_LIBMGBA

#include "Core/HW/SI/SI_DeviceGBAEmu.h"

#include <vector>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GBACore.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Host.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"

namespace SerialInterface
{
static s64 GetSyncInterval(const SystemTimers::SystemTimersManager& timers)
{
  return timers.GetTicksPerSecond() / 1000;
}

CSIDevice_GBAEmu::CSIDevice_GBAEmu(Core::System& system, SIDevices device, int device_number)
    : ISIDevice(system, device, device_number)
{
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
    const bool handshake_active = m_last_data_cmd != 0 && elapsed_since(m_last_data_cmd) < tps * 8;

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
    if (link_now && handshake_active)
      m_link_established = true;

    // Once this much traffic has crossed a live link in one burst, XD has
    // genuinely read a party and the battle is underway: lock the port so no
    // auto-reset can fire for the duration. Rebooting Emerald mid-battle drops
    // the link and kills the fight with no message, so that has to be
    // impossible rather than merely unlikely, and a party is hundreds of
    // commands where a failed probe is a handful.
    if (m_data_cmd_count >= DATA_CMDS_FOR_BATTLE)
      m_battle_locked = true;

    // The lock releases only when the battle is unambiguously over: XD probing
    // again with nothing flowing for a full minute. In-battle animations starve
    // data for seconds, never that long, so this cannot fire mid-fight -- but
    // without it a rematch could never re-link, which is the same wedge from the
    // other direction.
    if (m_battle_locked && probing && !link_now && elapsed_since(m_last_data_cmd) > tps * 60)
      m_battle_locked = false;

    // ...and the latch is not permanent. If XD is probing again with no data
    // having flowed for half a minute, the battle this latch was protecting is
    // over (or never started) and the port has to be recoverable. The threshold
    // is deliberately far longer than the 8 s handshake window: in-battle
    // animations can starve data commands for a while, and re-arming the reset
    // mid-fight is exactly the crash the latch exists to prevent.
    if (m_link_established && !m_battle_locked && probing && !link_now &&
        (m_last_data_cmd == 0 || elapsed_since(m_last_data_cmd) > tps * 30))
    {
      m_link_established = false;
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
    }
    m_core->SetLinkAudioMuted((!m_link_established && !m_battle_locked) ||
                              (m_netplay_locality_cached && !m_netplay_pad_is_local));

    if (probing && !link_now && !handshake_active && !m_link_established && !m_battle_locked)
    {
      // Must exceed the slowest reset->link-window-open time, or the cooldown
      // resets the core before Emerald's window ever opens (livelock). Measured:
      // bundled open-source BIOS 2.35 s, official BIOS 4.78 s. The steady-state
      // cadence comes from the window_just_closed edge, so this floor only
      // gates the bootstrap and missed-edge recovery paths.
      const u64 min_interval = tps * 6;
      const bool window_just_closed = m_link_was_enabled;
      const bool cooldown_elapsed =
          m_last_auto_reset == 0 || elapsed_since(m_last_auto_reset) > min_interval;
      if (window_just_closed || cooldown_elapsed)
      {
        if (!NetPlay::IsNetPlayRunning())
        {
          // Async: reset on the GBA's own event thread. Calling Reset() here
          // would Flush() (block the CPU thread until the event thread drains),
          // which deadlocks the whole emulator on Android where the event
          // thread can stall in the synchronous frame callback.
          m_core->RequestReset(m_timestamp_sent);
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
        }
        m_last_auto_reset = m_timestamp_sent;
        m_data_cmd_count = 0;
      }
    }
    m_link_was_enabled = link_now;
    m_core->SendJoybusCommand(m_timestamp_sent, TransferInterval(), buffer, m_keys);

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
  if (pad_status.button & PadButton::PAD_BUTTON_X)
    m_core->RequestReset(m_timestamp_sent);

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
