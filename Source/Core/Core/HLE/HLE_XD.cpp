// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE_XD.h"

#include <array>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"

#include "Core/Config/SessionSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace HLE_XD
{
namespace
{
// Pokemon XD GXXE01 (NTSC-U) main.dol. Every address below is fixed in that binary; Install()
// refuses any other disc/DOL by checking the disc ID and the two hooked opcodes.
constexpr u32 XD_DISC_ID = 0x47585845;      // "GXXE" at 0x80000000
constexpr u32 ADDR_OSGETTIME = 0x800b2244;  // mftbu r3; mftb r4; mftbu r5; cmpw; bne; blr
constexpr u32 ADDR_OSGETTICK = 0x800b225c;  // mftb r3; blr
constexpr u32 OP_MFTBU_R3 = 0x7C6D42E6;
constexpr u32 OP_MFTB_R3 = 0x7C6C42E6;
// XD's main-loop frame counter, r13-21880: zeroed once at 0x8005c330 (entry of the main-loop
// function 0x8005c2fc), incremented at 0x8005c6f4 (end of every pass of 0x8005c6a0..0x8005c6f8),
// read only by 0x8005c6e4 / 0x80150e54 / 0x80151018. Pure game state: identical on every
// machine that executes the same instruction stream.
constexpr u32 ADDR_FRAME_COUNTER = 0x804EA8A8;

// Call sites (LR = bl address + 4) with special values; everything else is "Default".
constexpr u32 LR_RNG_SEEDER = 0x800efaa4;     // bl OSGetTime @0x800efaa0; stw r4 -> RNG 0x804E8610
constexpr u32 LR_JOYBOOT_KEY = 0x8002d45c;  // bl OSGetTick @0x8002d458; key=(r3&0xFFFFFF)|0xDD<<24
constexpr u32 LR_VI_INIT_STAMP = 0x802af138;  // bl OSGetTime @0x802af134; first VI-module stamp
constexpr u32 LR_MAIN_PRELOOP_STAMP = 0x8005c68c;  // main loop pre-loop stamp: one period back
// Stamps that feed game-visible timing get the frame-exact value (no per-call term).
constexpr std::array<u32, 6> FRAME_EXACT_LRS = {
    0x8005c6ac, 0x8005c6e4,  // main loop 0x8005c2fc: now / prev stamps -> per-frame us delta
    0x802ae5b0, 0x802ae9b0,  // VI swap stamps (0x802ae490 / 0x802ae898) -> +0x74 frame ratio
    0x8005c7bc,              // 0x8005c71c: allocator base += OSGetTime.lo & 0x7e0
    0x800e8364,              // __EXIProbe: 100 ms units gating memory-card attach (300 ms window)
};
// Per-site call ordinal within the frame: deterministic and still varies per object.
constexpr std::array<u32, 4> ORDINAL_LRS = {
    0x801e3028,                          // 0x801e2b04: obj+0x17 = OSGetTick() % tmpl->0x5c
    0x800c0a74, 0x800c0aa4, 0x800c0b58,  // CARD DummyLen / __CARDUnlock LCG seeds (0x804E81E0)
};
// Sites that must see the real time base (alarms, audio, real-date stamps, reset, crash).
constexpr std::array<u32, 14> PASSTHROUGH_LRS = {
    0x800b2288,                          // __OSGetSystemTime: OSAlarm/decrementer, DVD, SI, CARD
    0x800bc8b0, 0x800bc8f4, 0x800bc9dc,  // __AI_SRC_INIT measurement + spin (boot, before VI)
    0x801cc528,                          // save-file calendar timestamp
    0x800c4b78, 0x800c533c, 0x800c5bc4,  // CARD directory entry time (seconds since epoch)
    0x800c3ea8,                          // __CARDFormatRegionAsync formatTime / serial
    0x800ad0d4, 0x800ad114,              // OSResetSystem waits
    0x800abf4c,                          // __OSUnhandledException report
    0x80181c2c, 0x80182014,              // AI DMA callback stamp / mixer elapsed-us (IRQ context)
};
// Default-class reads (JoyBoot polls/timeouts, boot spins) advance a per-boot counter by this
// much per call: a spin that waits N ticks exits after about N/3 reads, close to the ~3.3 TB
// ticks one poll iteration costs on hardware. Never reaches game state (see Read()).
constexpr u64 DEFAULT_INCREMENT = 3;

enum class Site
{
  Default,
  FrameExact,
  InitStamp,
  PreLoopStamp,
  Ordinal,
  Passthrough,
};

struct Ordinal
{
  u32 frame = 0;
  u32 count = 0;
};

bool s_installed = false;
u32 s_salt = 0;
u32 s_seed = 0;
u64 s_period = 0;
u64 s_calls = 0;
std::array<Ordinal, ORDINAL_LRS.size()> s_ordinals{};

Site Classify(u32 lr, size_t* ordinal_index)
{
  for (const u32 x : PASSTHROUGH_LRS)
  {
    if (lr == x)
      return Site::Passthrough;
  }
  for (const u32 x : FRAME_EXACT_LRS)
  {
    if (lr == x)
      return Site::FrameExact;
  }
  if (lr == LR_VI_INIT_STAMP)
    return Site::InitStamp;
  if (lr == LR_MAIN_PRELOOP_STAMP)
    return Site::PreLoopStamp;
  for (size_t i = 0; i < ORDINAL_LRS.size(); ++i)
  {
    if (lr == ORDINAL_LRS[i])
    {
      *ordinal_index = i;
      return Site::Ordinal;
    }
  }
  return Site::Default;
}

u64 Base(Core::System& system)
{
  // fake_TB_start_value derives from the host's initial RTC that NetPlay already syncs
  // (NetPlayServer::GetInitialNetPlayRTC -> NetPlayClient::m_initial_rtc -> SystemTimers::Init).
  return system.GetCoreTiming().GetFakeTBStartValue() + s_salt;
}

u64 Read(Core::System& system, u32 lr)
{
  size_t ord = 0;
  switch (Classify(lr, &ord))
  {
  case Site::Passthrough:
    return system.GetSystemTimers().GetFakeTimeBase();
  case Site::FrameExact:
    return FrameExactTimeBase(system);
  case Site::InitStamp:
    // Hardware follows this stamp with two VIWaitForRetrace before the first swap; give the
    // first measured frame the same 2-field delta instead of 0.
    return FrameExactTimeBase(system) - 2 * s_period;
  case Site::PreLoopStamp:
    // The main loop's pre-loop stamp: one period back so pass 1's delta is exactly one field
    // (16683 us), as on hardware, instead of 0.
    return FrameExactTimeBase(system) - s_period;
  case Site::Ordinal:
  {
    const u32 frame = GetFrame(system);
    Ordinal& o = s_ordinals[ord];
    if (o.frame != frame)
    {
      o.frame = frame;
      o.count = 0;
    }
    return Base(system) + s_period * frame + o.count++;
  }
  case Site::Default:
  default:
    // The per-call term only exists so that spin-waits (SI transfer polls, the OSInit DSP
    // spin, GX abort spins) make progress. Its exact value never reaches game state: every
    // game-visible stamp is FrameExact/Ordinal/Seed/Key above, and every Default consumer is
    // an elapsed-time comparison against a threshold of 100 ms or more, or a fixed-count spin.
    s_calls += DEFAULT_INCREMENT;
    return Base(system) + s_period * GetFrame(system) + s_calls;
  }
}

u32 KeyMaterial()
{
  // Six nibbles with exactly two bits each: popcount(low 24) = 12. The generator ORs in 0xDD
  // (6 bits) and gates popcount(key) to 10..24 (0x8002d47c..0x8002d534): 18 passes on the first
  // read on every machine, so the retry loop never re-reads.
  static constexpr u8 NIBBLES[4] = {0x3, 0x5, 0x6, 0x9};
  u32 k = 0;
  for (u32 i = 0; i < 6; ++i)
    k |= static_cast<u32>(NIBBLES[(s_salt >> (2 * i)) & 3]) << (4 * i);
  return k;
}
}  // namespace

bool IsInstalled()
{
  return s_installed;
}

u32 GetSalt()
{
  return s_salt;
}

u32 GetSeed()
{
  return s_seed;
}

u64 GetPeriod()
{
  return s_period;
}

u64 CallCount()
{
  return s_calls;
}

u64 DefaultIncrement()
{
  return DEFAULT_INCREMENT;
}

u32 GetFrame(Core::System& system)
{
  return system.GetMemory().Read_U32(ADDR_FRAME_COUNTER);
}

u64 FrameExactTimeBase(Core::System& system)
{
  return Base(system) + s_period * GetFrame(system);
}

void Install(Core::System& system)
{
  if (!Config::Get(Config::SESSION_XD_DETERMINISTIC_CLOCK))
    return;
  auto& memory = system.GetMemory();
  if (memory.Read_U32(0x80000000) != XD_DISC_ID ||
      memory.Read_U32(ADDR_OSGETTIME) != OP_MFTBU_R3 ||
      memory.Read_U32(ADDR_OSGETTICK) != OP_MFTB_R3)
  {
    ERROR_LOG_FMT(OSHLE, "XD clock requested but this is not the GXXE01 DOL; hooks NOT installed");
    return;
  }
  if (!s_installed)
  {
    // First install of this emulation session. HLE::Reload may run again later (symbol
    // loads); the counters must survive that.
    s_salt = Config::Get(Config::SESSION_XD_CLOCK_SALT);
    s_seed = Config::Get(Config::SESSION_XD_RNG_SEED);
    const u64 tb_hz = static_cast<u64>(system.GetSystemTimers().GetTicksPerSecond()) /
                      SystemTimers::TIMER_RATIO;
    s_period = tb_hz * 1001 / 60000;  // 40,500,000 * 1001 / 60000 = 675,675 = one NTSC field
    s_calls = 0;
    s_ordinals = {};
    s_installed = true;
  }
  HLE::Patch(system, ADDR_OSGETTICK, "XD_OSGetTick");
  HLE::Patch(system, ADDR_OSGETTIME, "XD_OSGetTime");
  INFO_LOG_FMT(OSHLE, "XD deterministic clock installed: salt={:08x} seed={:08x} period={} inc={}",
               s_salt, s_seed, s_period, DEFAULT_INCREMENT);
}

void Shutdown()
{
  s_installed = false;
  s_salt = 0;
  s_seed = 0;
  s_period = 0;
  s_calls = 0;
  s_ordinals = {};
}

void OSGetTick(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  const u32 lr = LR(ppc_state);
  const u64 value = (lr == LR_JOYBOOT_KEY) ? KeyMaterial() : Read(system, lr);
  ppc_state.gpr[3] = static_cast<u32>(value);
  ppc_state.npc = lr;  // Replace hooks return by writing npc (see HLE_Misc::UnimplementedFunction)
}

void OSGetTime(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  const u32 lr = LR(ppc_state);
  u64 value = Read(system, lr);
  if (lr == LR_RNG_SEEDER)
    value = (value & 0xFFFFFFFF00000000ull) | s_seed;  // stw r4 -> 0x804E8610 = battle RNG seed
  ppc_state.gpr[3] = static_cast<u32>(value >> 32);
  ppc_state.gpr[4] = static_cast<u32>(value);
  ppc_state.npc = lr;
}
}  // namespace HLE_XD
