// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef HAS_LIBMGBA

#include <array>
#include <memory>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/HW/SI/SI_Device.h"
#include "InputCommon/GCPadStatus.h"

namespace SerialInterface
{
class CSIDevice_GCController;
class CSIDevice_GBAEmu;

// Port 1 for XD's multi battles ("GBA + GBA VS GBA + GBA"). XD drives the whole
// mode from the controller in socket 1, then asks for a GBA in that same
// socket. This device is a GC controller (Pad) until XD's socket-1 prompt,
// answers NOREP+RDST for a moment so XD's PADRead unplugs the pad (Unplug),
// then becomes an integrated GBA (Gba) until the battle or the prompt ends.
//
// Solo, the planner (EvaluateTransitions) decides from XD RAM, SIPOLL, the pad
// sample, CoreTiming ticks and the inner GBA's RunBuffer results, and a switch
// happens on the poll that decided it. In netplay only the host plans: a
// decision is posted (XDMultiCtl.h), rides the next channel-0 pad entry the
// host pushes, and every machine applies it when it pops that entry, with no
// further condition, so the mode is identical everywhere at every pop. Only
// channel 0 owns a GBA core; on any other channel the device is an inert GC
// controller.
class CSIDevice_XDMultiPort final : public ISIDevice
{
public:
  CSIDevice_XDMultiPort(Core::System& system, SIDevices device, int device_number);
  ~CSIDevice_XDMultiPort() override;

  int RunBuffer(u8* buffer, int request_length) override;
  int TransferInterval() override;
  DataResponse GetData(u32& hi, u32& low) override;
  void SendCommand(u32 command, u8 poll) override;
  void DoState(PointerWrap& p) override;
  void OnEvent(u64 userdata, s64 cycles_late) override;

  enum class Mode : u8
  {
    Pad = 0,
    Unplug = 1,
    Gba = 2,
  };

private:
  // One read of the XD words the mode machine uses (all zero off XD).
  struct XdRam
  {
    bool is_xd = false;
    std::array<u32, 4> ctx{};
    u8 ctxch = 0;
    u32 ctx28 = 0;
    u32 ctxmode = 0;
    u8 errflag = 0;
    u32 bflag = 0;
    std::array<u32, 4> link_a{};
    std::array<u32, 4> link_b{};
  };

  // One synced pad pop, decoded: the GC status XD sees, plus the GBA keys, the Reset and the
  // command that rode with it (netplay) or were read locally (solo).
  struct Sample
  {
    GCPadStatus pad{};
    u16 keys = 0;  // GBA KEYINPUT order, 10 bits
    bool reset = false;
    u8 cmd = 0;  // XDMultiCtl::Command
  };

  XdRam ReadXdRam() const;
  Sample PopSynced();
  void ApplyCtl(const Sample& sample, u64 now);
  void ApplyGbaInput(const Sample& sample, bool keys_active);
  int RunGba(u8* buffer, int request_length, u64 now);
  // The planner: host (or solo) only.
  void EvaluateTransitions(u64 now, bool en0, const XdRam& ram);
  void EvaluatePad(const XdRam& ram, bool at_prompt, bool prompt_live, bool combo_fire, u64 now);
  void EvaluateUnplug(const XdRam& ram, bool at_prompt, bool err_edge, bool combo_fire, bool en0,
                      u64 now);
  void EvaluateGba(const XdRam& ram, bool err_edge, int err_ctx, bool combo_fire, u64 now);
  // Solo: Execute now. Netplay host: post it for the stream.
  void Decide(u8 cmd, const char* reason, u64 now);
  // Every machine, at the pop that carries the command (solo: the deciding poll).
  void Execute(u8 cmd, const char* reason, u64 now);
  void SwitchMode(Mode to, const char* reason, u64 now);
  void Replug(const char* reason, u64 now);
  static GCPadStatus ToGba(const GCPadStatus& pad);
  static u16 KeysToPadButtons(u16 keys);

  // Diagnostics (gba_detect log). Never read by control.
  std::string Snapshot(u64 now) const;
  void Log(u64 now, const std::string& detail, bool flush = false) const;
  void LogDiagnostics(const XdRam& ram, bool en0, u64 now);

  std::unique_ptr<CSIDevice_GCController> m_controller;
  // Only on channel 0.
  std::unique_ptr<CSIDevice_GBAEmu> m_gba;

  // Serialized (DoState order).
  Mode m_mode = Mode::Pad;
  u64 m_mode_tick = 0;
  bool m_armed = true;
  u32 m_trigger_streak = 0;
  bool m_gba_pending = false;
  u8 m_pending_cmd = 0;
  bool m_data_seen = false;
  u64 m_last_data_tick = 0;
  u32 m_probe_fail_streak = 0;
  bool m_battle_seen = false;
  u32 m_bflag_prev = 0;
  u64 m_battle_end_tick = 0;
  std::array<u32, 4> m_ctx_prev{};
  u8 m_errflag_prev = 0;
  bool m_want_pad = false;
  u8 m_want_reason = 0;
  u64 m_cond_since = 0;
  bool m_replug6_done = false;
  u16 m_last_buttons = 0;
  u64 m_combo_since = 0;
  bool m_combo_released = true;
  u64 m_link_lost_since = 0;
  // After DoMarker("XDMultiPort2"). Pops left in which a re-plugged GBA stays silent (a count
  // of GetData pops, never a tick, so it ends at the same pop everywhere); every pop of this
  // device; the socket-1 wait count at the last attach or re-plug.
  u32 m_silent_pops = 0;
  u64 m_pops = 0;
  u64 m_s1_base = 0;

  // Not serialized, fixed at construction. m_netplay: a netplay game (the stream decides the
  // mode); m_is_local: this machine owns channel 0 (the host), so it plans; m_boot_seq tags its
  // posts.
  bool m_netplay = false;
  bool m_is_local = true;
  u64 m_boot_seq = 0;

  // Not serialized: true until the first DoState/GetData/RunBuffer/OnEvent.
  bool m_fresh = true;

  // Diagnostics only. Not serialized, never read by control.
  bool m_diag_have_prev = false;
  bool m_diag_prev_en0 = false;
  u32 m_diag_en0_lines = 0;
  u32 m_diag_prev_ctx28 = 0;
  u32 m_diag_ctx_lines = 0;
  u64 m_diag_last_sum = 0;
  u64 m_diag_pad_since = 0;
  u32 m_diag_pad_rejects = 0;
  u32 m_diag_silent_rejects = 0;
  std::array<bool, 256> m_diag_reject_logged{};
  bool m_diag_padok_armed = false;
  bool m_diag_padwait_logged = false;
  u64 m_diag_padok_start = 0;
  std::array<u32, 4> m_diag_prev_ctx{};
  u8 m_diag_prev_errflag = 0;
};
}  // namespace SerialInterface

#endif  // HAS_LIBMGBA
