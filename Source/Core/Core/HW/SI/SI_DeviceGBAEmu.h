// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef HAS_LIBMGBA

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>

#include "Common/CommonTypes.h"
#include "Core/HW/SI/SI_Device.h"

namespace HW::GBA
{
class Core;
}  // namespace HW::GBA

class GBAHostInterface;

namespace SerialInterface
{
// One entry per SI channel; must match MAX_SI_CHANNELS (static_assert in the
// .cpp, which is where SI.h is included -- pulling SI.h in here would be
// circular).
constexpr size_t GBA_LINK_DIAG_CHANNELS = 4;

// Purely observational snapshot of one GBA port's joybus link state, published
// for the Android on-screen diagnostic (and anything else that wants ground
// truth on why XD is or isn't detecting a socket). Written from the emulation
// thread inside the existing poll, read from the UI thread, hence atomics --
// the fields are independent counters, so a torn set across fields is fine.
//
// NOTHING here feeds back into the link/reset logic: this is a tap, not a
// control input. Keep it that way.
struct GBALinkDiag
{
  // Joybus link window currently open (Core::IsLinkEnabled()).
  std::atomic<bool> link{false};
  // Number of times the link window has opened (false -> true edges).
  std::atomic<u32> window_count{0};
  // Auto-resets actually issued for this port (solo + netplay + X-button).
  std::atomic<u32> reset_count{0};
  // m_link_established: XD handshake seen over a live link.
  std::atomic<bool> established{false};
  // m_battle_locked: sustained traffic latched the port against auto-reset.
  std::atomic<bool> locked{false};
  // Last joybus command byte XD sent (0xFF reset / 0x00 status / 0x14 read...).
  std::atomic<u8> last_cmd{0};
  // Polls where XD was probing (RESET/STATUS) -- i.e. XD is looking at us.
  std::atomic<u32> probe_count{0};
};

// Defined once in SI_DeviceGBAEmu.cpp.
extern std::array<GBALinkDiag, GBA_LINK_DIAG_CHANNELS> g_gba_link_diag;

class CSIDevice_GBAEmu final : public ISIDevice
{
public:
  CSIDevice_GBAEmu(Core::System& system, SIDevices device, int device_number);
  ~CSIDevice_GBAEmu() override;

  int RunBuffer(u8* buffer, int request_length) override;
  int TransferInterval() override;
  DataResponse GetData(u32& hi, u32& low) override;
  void SendCommand(u32 command, u8 poll) override;
  void DoState(PointerWrap& p) override;
  void OnEvent(u64 userdata, s64 cycles_late) override;

private:
  enum class NextAction
  {
    SendCommand,
    WaitTransferTime,
    ReceiveResponse
  };

  NextAction m_next_action = NextAction::SendCommand;
  EBufferCommands m_last_cmd{};
  u64 m_timestamp_sent = 0;
  u64 m_last_auto_reset = 0;
  u64 m_last_data_cmd = 0;
  bool m_link_was_enabled = false;
  u64 m_last_link_seen = 0;
  u64 m_quiet_until = 0;
  bool m_link_established = false;
  // Sustained joybus traffic means a real party exchange, not a stray probe.
  static constexpr u32 DATA_CMDS_FOR_BATTLE = 64;
  u32 m_data_cmd_count = 0;
  bool m_battle_locked = false;
  bool m_netplay_pad_is_local = true;
  bool m_netplay_locality_cached = false;
  u16 m_keys = 0;

  // Diagnostic-only mirror of the previous poll's link state, so window_count
  // can count open edges without reading (or perturbing) m_link_was_enabled,
  // which the auto-reset guard above depends on.
  bool m_diag_prev_link = false;

  std::shared_ptr<HW::GBA::Core> m_core;
  std::shared_ptr<GBAHostInterface> m_gbahost;
};
}  // namespace SerialInterface
#endif  // HAS_LIBMGBA
