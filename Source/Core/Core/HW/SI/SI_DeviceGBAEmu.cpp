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

    // Latch once the joybus link is up AND real data (READ/WRITE) has flowed:
    // the party read is done and the battle is underway. Never auto-reset after
    // this. Otherwise a rare coincidence during turn 1 -- Emerald briefly leaves
    // JOYBUS mode at the "Link standby -> battle scene" transition (link_now
    // false) while an 8s+ animation has let handshake_active lapse and a
    // STATUS/RESET probe lands in that window -- would satisfy the guard below
    // and reboot Emerald out from under XD, dropping the link and crashing the
    // fight with no message. Pre-battle window-reopening is unaffected: during
    // pure copyright-screen probing XD sends only STATUS/RESET, so m_last_data_cmd
    // is still 0 and the brief JOYBUS blip does not trip the latch early.
    if (link_now && m_last_data_cmd != 0)
      m_link_established = true;

    const bool probing = m_last_cmd == EBufferCommands::CMD_RESET ||
                         m_last_cmd == EBufferCommands::CMD_STATUS;
    const bool handshake_active =
        m_last_data_cmd != 0 && (m_timestamp_sent - m_last_data_cmd) < tps * 8;
    if (probing && !link_now && !handshake_active && !m_link_established &&
        !NetPlay::IsNetPlayRunning())
    {
      const u64 min_interval = tps * 4;
      const bool window_just_closed = m_link_was_enabled;
      const bool cooldown_elapsed =
          m_last_auto_reset == 0 || m_timestamp_sent - m_last_auto_reset > min_interval;
      if (window_just_closed || cooldown_elapsed)
      {
        // Async: reset on the GBA's own event thread. Calling Reset() here
        // would Flush() (block the CPU thread until the event thread drains),
        // which deadlocks the whole emulator on Android where the event
        // thread can stall in the synchronous frame callback.
        m_core->RequestReset(m_timestamp_sent);
        m_last_auto_reset = m_timestamp_sent;
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
    if (m_device_number == 2 && Config::Get(Config::MAIN_GBA_PRACTICE_DUMMY))
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

  // Use X button as a reset signal for NetPlay/Movies
  if (pad_status.button & PadButton::PAD_BUTTON_X)
    m_core->Reset();

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
