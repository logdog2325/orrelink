// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

// XD multi battles in netplay: how the port-1 device's mode switches reach every machine.
//
// Only the host plans (SI_DeviceXDMultiPort.cpp). A decision is posted here on the host's CPU
// thread, taken by the next channel-0 pad entry the host pushes (NetPlayClient::PollLocalPad) and
// applied by every machine when it pops that entry, so all machines switch at the same pop. The
// entry is a GC pad status whose analog A/B bytes carry the control instead of analog levels:
//
//   analogA  bits 0-7  GBA keys 0-7 (A, B, Select, Start, Right, Left, Up, Down)
//   analogB  bit 0     RESET: the GBA window's Reset (the X reset of the plain GBA device)
//            bits 1-3  CMD (Command below)
//            bits 4-5  GBA keys 8-9 (R, L)
//            bits 6-7  zero
//
// Only the first copy pushed on a poll carries RESET and CMD; the copies a buffer raise adds keep
// the keys (held keys are level state) and clear bits 0-3. The popping device rebuilds analog A/B
// from the A/B buttons exactly as GCPadEmu does, so XD never sees these bytes.
//
// Outside this outbox and the wire nothing is shared. Built outside USE_MGBA (the netplay client
// uses it either way).
namespace XDMultiCtl
{
enum Command : u8
{
  CMD_NONE = 0,
  CMD_TO_UNPLUG = 1,
  CMD_TO_GBA = 2,
  CMD_TO_PAD = 3,
  CMD_REPLUG = 4,
};

constexpr u8 WIRE_RESET = 0x01;
constexpr u8 WIRE_CMD_SHIFT = 1;
constexpr u8 WIRE_CMD_MASK = 0x0E;
constexpr u8 WIRE_CTL_MASK = 0x0F;  // RESET and CMD: first copy only
constexpr u8 WIRE_KEYS_HI_SHIFT = 4;
constexpr u8 WIRE_KEYS_HI_MASK = 0x30;

const char* CommandName(u8 cmd);

// CPU thread (host). Posts cmd for the game whose Core::GetBootSequence() is boot_seq. Refused
// (false) while a command is waiting to be taken or has been taken and not yet applied.
bool Post(u8 cmd, u64 boot_seq);
// CPU thread (host), under crit_netplay_client in the channel-0 push: the waiting command, or
// CMD_NONE. A command posted under another boot sequence is dropped. A taken command stays in
// flight until MarkApplied.
u8 Take(u64 boot_seq);
// True while a command waits or is in flight.
bool Busy();
// The host's device popped the entry that carried the command (valid or not).
void MarkApplied();
// A new port-1 device: forget anything the previous game left.
void Reset();
}  // namespace XDMultiCtl
