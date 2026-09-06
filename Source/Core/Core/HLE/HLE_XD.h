// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

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
}  // namespace HLE_XD
