// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace Memory
{
class MemoryManager;
}  // namespace Memory

// OrreLink: deterministic clock for Pokemon XD (GXXE01) netplay rooms that mix CPU
// architectures. HLE Replace hooks on OSGetTick/OSGetTime return a value that is a pure
// function of game state plus a host-synced salt instead of Dolphin's block-granular time
// base, which differs by a few dozen cycles between JIT64 and JITARM64 because the two
// recompilers split blocks differently and the tick only advances at block ends. XD seeds
// its battle RNG and its GBA link key from that time base (the v1.5.9 field desyncs).
namespace HLE_XD
{
void Install(Core::System& system);  // called from HLE::PatchFixedFunctions (every HLE::Reload)
void Shutdown();                     // called when the emulation session ends (Core::EmuThread)
bool IsInstalled();
u32 GetSalt();
u32 GetSeed();
u64 GetPeriod();
u64 CallCount();
u64 DefaultIncrement();  // per-call step of the Default-class counter (logged next to the period)
u32 GetFrame(Core::System& system);  // XD's main-loop pass counter (0x804EA8A8); ordinal key + pf=
u64 FrameExactTimeBase(Core::System& system);  // T0 + period * fields (no per-call term)
u32 GetFields();       // clock unit: SDK VI fields sampled at the main-loop 'now' stamp
u64 LastNowTicks();    // CoreTiming ticks at the last 'now' stamp (diagnostics only)
u64 LastNowDelta();    // ticks between the last two 'now' stamps (diagnostics only)
u32 ClockModel();      // 2 = v1.5.12 field clock; logged so a reader knows what lf= means

void OSGetTick(const Core::CPUThreadGuard& guard);  // hooks 0x800b225c
void OSGetTime(const Core::CPUThreadGuard& guard);  // hooks 0x800b2244

// XD multi battles: XD's "controller in socket 1" wait (0x801044D4, called with r3 == 1 from the
// VS loop and the menu error paths). A Start hook that only counts the calls, for the port-1
// device's planner: while XD waits there, port 1 must be a controller again. It never writes
// guest state. Installed from PatchFixedFunctions (every HLE::Reload) only on the GXXE01 DOL with
// SI port 1 set to the XD Multi device (the netplay layer sets it in a Multi battle).
void InstallMultiHooks(Core::System& system);
void Socket1WaitHook(const Core::CPUThreadGuard& guard);
struct Socket1Wait
{
  bool installed = false;
  u64 calls = 0;  // matching calls this emulation session
  u64 tick = 0;   // CoreTiming ticks at the last one
  u32 lr = 0;     // its caller
};
// CPU thread only.
Socket1Wait GetSocket1Wait();

// Pokemon XD battle state, read from fixed MEM1 addresses (BAT identity mapping, no MMU) and
// shared by the gba_detect 'xd' lines and the netplay state check. Every field is game RAM that
// is identical on every machine running the same instruction stream: no XFB, EFB copy, audio
// buffer or other per-machine data. is_xd is false (and the rest zero) when the disc at
// 0x80000000 is not GXXE. CPU thread only, about 1.7 KB of reads. Read it at a point in the
// game's own code (a GBA WRITE), not at a CoreTiming event: machines whose JITs split blocks
// differently reach the same event after different instructions.
struct XdDigest
{
  bool is_xd = false;
  u32 seed = 0;   // main PRNG seed
  u32 jb = 0;     // SDK CARD unlock LCG (logged as jb=)
  u32 flags = 0;  // battle flag word
  u32 ctx = 0;    // the battle context cell as read (0 between battles)
  u32 crc_a = 0;  // battle-engine scalars and the battle event struct
  u32 crc_b = 0;  // party state of the live battle context; 0 when there is none
  u32 crc_c = 0;  // GBA driver state, link state, battle mode, format ruleset entry
};
XdDigest ComputeXdDigest(const Memory::MemoryManager& memory);
// One word over every XdDigest field, for the netplay state check.
u32 XdDigestHash(const XdDigest& digest);
}  // namespace HLE_XD
