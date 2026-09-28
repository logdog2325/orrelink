// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE_XD.h"

#include <algorithm>
#include <array>
#include <optional>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI_Device.h"
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
// v1.5.11 used that pass counter as the clock unit; XD runs one pass per TWO VI fields in
// battle (flip every 2nd retrace, 0x802aebd4(1)), so the game measured half the real frame
// delta and every delta-scaled system ran at half rate (field-confirmed). It is now only
// the Ordinal reset key and the pf= log field.
// v1.5.12 clock unit: the SDK's own VI retrace count (r13-0x51cc). ++ once per DI0/DI1
// interrupt in __VIRetraceHandler 0x800b8588 (stw 0x800b8688), zeroed once in VIInit
// (0x800b8b9c), read by the game only in VIWaitForRetrace 0x800b8fe0 / VIGetRetraceCount
// 0x800ba25c. DI1 = line 1, DI0 = line nhlines/2+1 (__VIInit 0x800b8a94 / 0x800b8ab8):
// 59.94 Hz in 480i and 480p, independent of what the game does per pass.
constexpr u32 ADDR_RETRACE_COUNT = 0x804EAC54;
// The ONLY site that samples it: the main loop's 'now' stamp. From the third pass on it runs
// thousands of cycles after the render task's end-of-frame wait was released from inside
// the retrace interrupt path, so the read is program-ordered after the handler on every
// machine and cannot straddle the block-split skew. Every other site gets the cached value.
constexpr u32 LR_MAIN_NOW_STAMP = 0x8005c6ac;
constexpr u32 CLOCK_MODEL = 2;

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
u32 s_fields = 0;        // clock unit: fields since the first locked 'now' stamp
u32 s_retrace_base = 0;  // retraceCount value that corresponds to s_fields == 2
u32 s_now_samples = 0;   // 'now' stamps seen this boot
u64 s_last_now_ticks = 0;
u64 s_last_now_delta = 0;
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

void SampleFields(Core::System& system)
{
  const u64 ticks = system.GetCoreTiming().GetTicks();
  s_last_now_delta = ticks - s_last_now_ticks;
  s_last_now_ticks = ticks;
  // An in-game soft reset reboots through the apploader: VIInit re-zeroes retraceCount and
  // the main loop re-enters with its pass counter back at 0 while these statics persist
  // (HLE::Reload keeps them). Re-run the boot anchoring instead of computing a ~2^32 jump.
  if (s_now_samples > 2 && GetFrame(system) == 0)
    s_now_samples = 0;
  ++s_now_samples;
  if (s_now_samples <= 2)
  {
    // Passes 1 and 2 can run before the render pipeline is phase-locked to the retrace
    // (init leaves a buffer free, so the first end-of-frame waits may not block): don't
    // read the counter there. Same values v1.5.11 produced on these passes (0, then 1).
    s_fields = s_now_samples - 1;
    return;
  }
  const u32 rc = system.GetMemory().Read_U32(ADDR_RETRACE_COUNT);
  if (s_now_samples == 3)
  {
    // First locked read (pass 3 waits for pass 2's copy, which is pending on a flip in
    // either boot ordering). Anchor here.
    s_retrace_base = rc - 2;
    s_fields = 2;
    return;
  }
  // Fields really elapsed, never fewer than one per pass: hardware never measures a
  // 0-field pass, and a loop that outruns the field rate (render task parked) then behaves
  // exactly like v1.5.11. Catches up after loads and hitches like hardware does.
  s_fields = std::max(rc - s_retrace_base, s_fields + 1);
}

u64 Read(Core::System& system, u32 lr)
{
  size_t ord = 0;
  switch (Classify(lr, &ord))
  {
  case Site::Passthrough:
    return system.GetSystemTimers().GetFakeTimeBase();
  case Site::FrameExact:
    if (lr == LR_MAIN_NOW_STAMP)
      SampleFields(system);
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
    const u32 frame = GetFrame(system);  // reset key: still the pass counter
    Ordinal& o = s_ordinals[ord];
    if (o.frame != frame)
    {
      o.frame = frame;
      o.count = 0;
    }
    return FrameExactTimeBase(system) + o.count++;
  }
  case Site::Default:
  default:
    // The per-call term only exists so that spin-waits (SI transfer polls, the OSInit DSP
    // spin, GX abort spins) make progress. Its exact value never reaches game state: every
    // game-visible stamp is FrameExact/Ordinal/Seed/Key above, and every Default consumer is
    // an elapsed-time comparison against a threshold of 100 ms or more, or a fixed-count spin.
    s_calls += DEFAULT_INCREMENT;
    return FrameExactTimeBase(system) + s_calls;
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

u32 GetFields()
{
  return s_fields;
}

u64 LastNowTicks()
{
  return s_last_now_ticks;
}

u64 LastNowDelta()
{
  return s_last_now_delta;
}

u32 ClockModel()
{
  return CLOCK_MODEL;
}

u64 FrameExactTimeBase(Core::System& system)
{
  return Base(system) + s_period * s_fields;
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
    s_fields = 0;
    s_retrace_base = 0;
    s_now_samples = 0;
    s_last_now_ticks = 0;
    s_last_now_delta = 0;
    s_installed = true;
  }
  HLE::Patch(system, ADDR_OSGETTICK, "XD_OSGetTick");
  HLE::Patch(system, ADDR_OSGETTIME, "XD_OSGetTime");
  INFO_LOG_FMT(OSHLE, "XD deterministic clock installed: model={} salt={:08x} seed={:08x} "
               "period={} inc={}",
               CLOCK_MODEL, s_salt, s_seed, s_period, DEFAULT_INCREMENT);
}

namespace
{
// The socket-1 controller wait (see HLE_XD.h). Its first instruction, stwu r1,-0x20(r1), is the
// install check.
constexpr u32 ADDR_SOCKET1_WAIT = 0x801044D4;
constexpr u32 OP_SOCKET1_WAIT_PROLOGUE = 0x9421FFE0;
// The callers whose r3 == 1 call is "XD waits for the socket-1 controller": the VS loop and its
// helper, the menu error paths and the connect flow's exits.
constexpr std::array<u32, 8> SOCKET1_WAIT_LRS = {0x80045F2C, 0x80046C6C, 0x80081700, 0x8008178C,
                                                 0x800817D8, 0x80081820, 0x8004F418, 0x8004F66C};
Socket1Wait s_socket1;
}  // namespace

void InstallMultiHooks(Core::System& system)
{
  auto& memory = system.GetMemory();
  if (memory.Read_U32(0x80000000) != XD_DISC_ID ||
      memory.Read_U32(ADDR_SOCKET1_WAIT) != OP_SOCKET1_WAIT_PROLOGUE ||
      Config::Get(Config::GetInfoForSIDevice(0)) != SerialInterface::SIDEVICE_GC_GBA_XDMULTI)
  {
    return;
  }
  // HLE::Reload can run again (symbol loads): the counters survive it.
  if (!s_socket1.installed)
    s_socket1 = Socket1Wait{.installed = true};
  HLE::Patch(system, ADDR_SOCKET1_WAIT, "XD_Socket1Wait");
  INFO_LOG_FMT(OSHLE, "XD multi socket-1 wait hook installed");
}

void Socket1WaitHook(const Core::CPUThreadGuard& guard)
{
  // A Start hook: the function runs on unchanged afterwards. Read only.
  auto& system = guard.GetSystem();
  const auto& ppc_state = system.GetPPCState();
  if (ppc_state.gpr[3] != 1)
    return;
  const u32 lr = LR(ppc_state);
  if (std::find(SOCKET1_WAIT_LRS.begin(), SOCKET1_WAIT_LRS.end(), lr) == SOCKET1_WAIT_LRS.end())
    return;
  ++s_socket1.calls;
  s_socket1.tick = system.GetCoreTiming().GetTicks();
  s_socket1.lr = lr;
}

Socket1Wait GetSocket1Wait()
{
  return s_socket1;
}

void Shutdown()
{
  s_socket1 = {};
  s_installed = false;
  s_salt = 0;
  s_seed = 0;
  s_period = 0;
  s_calls = 0;
  s_ordinals = {};
  s_fields = 0;
  s_retrace_base = 0;
  s_now_samples = 0;
  s_last_now_ticks = 0;
  s_last_now_delta = 0;
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

namespace
{
// Pokemon XD (GXXE01 USA, main.dol sha256 c8659341...78f15) battle state that decides a link
// battle, reverse-engineered from the binary and cross-checked adversarially. Every fixed address
// is a .bss/.sbss/.sdata location in MEM1.
struct XdRange
{
  u32 addr;
  u32 len;
};
// Main PRNG (the game's own LCG): seed = seed * 0x343FD + 0x269EC3, stepped by every rand_u16
// (407 callers) / rand_float (238) call; written once at boot from OSGetTime's low word
// (0x800efa58). The pointer cell at +4 always reads 0x804E8610 (its only setter has no callers).
constexpr u32 XD_RNG_SEED = 0x804E8610;
// SDK CARD unlock LCG state (ANSI LCG, 0x804e81e0 = r13-31808), seeded from OSGetTick by CARD
// DummyLen 0x800c0a50 / __CARDUnlock 0x800c0b14 (all 12 references) -- NOT JoyBoot: it only moves
// on memory-card mount/unlock. Logged raw as jb= (name kept for log compatibility); under the
// v1.5.11 clock those three reads are per-site ordinals, so it stays identical.
constexpr u32 XD_JOYBOOT_LCG = 0x804E81E0;
constexpr u32 XD_BATTLE_FLAGS = 0x804EB938;  // u32 bit flags, 263 refs in the engine
// crcA: battle-engine scalars (.sbss: flags, command-ring indices, turn counters, scratch-block
// pointer) + the .sdata battle event struct.
constexpr XdRange XD_CRC_A[] = {{0x804EB8D0, 0xB0}, {0x804E85C0, 0x20}};
// crcB: party state of the LIVE battle context. There are two 0x6ef0-byte context objects
// (ctx[0]=0x804a1744, ctx[1]=0x804a8634); trainer = ctx+0x64+t*0x3744; party entry =
// trainer+0x97c+i*0x300 (record at +4: species u16, +4 current HP u16, +0x11 level, +0x28
// status, +0x80 moves/PP, +0x90 max HP); active slot = trainer+0x1b7c+s*0x894.
// The cell at 0x804af528 (logged as ctx=) does NOT hold the context base: in the field it held
// 804a3324, 804a3bb8, 804aa214 and 804aaaa8, which are ctx+0x64+0x1b7c(+0x894), a trainer's
// active slot. Up to v1.7.4 crcB compared the cell against the two bases, never matched, and was
// 00000000 on every line. The base is now recovered from whichever slot the cell names.
constexpr u32 XD_CTX_PTR = 0x804AF528;
constexpr u32 XD_CTX_A = 0x804A1744;
constexpr u32 XD_CTX_B = 0x804A8634;
constexpr u32 XD_TRAINER_OFF = 0x64;
constexpr u32 XD_TRAINER_STRIDE = 0x3744;
constexpr u32 XD_PARTY_OFF = 0x97C;
constexpr u32 XD_PARTY_STRIDE = 0x300;
constexpr u32 XD_SLOT_OFF = 0x1B7C;
constexpr u32 XD_SLOT_STRIDE = 0x894;
// crcC: GBA driver per-channel state, .sbss link state, battle mode word, and the ruleset table
// entry the format pin writes (0x804334C0 = BattleCustomizer's ORRE_RULESET_BASE).
constexpr XdRange XD_CRC_C[] = {
    {0x80428338, 0x20}, {0x804EA768, 0x28}, {0x80429A00, 0x04}, {0x804334C0, 0x90}};
constexpr u32 XD_DISC_ID_GXXE = 0x47585845;  // "GXXE" at 0x80000000

// The context base the cell names: the base itself, or one of its trainers' two active slots.
// The two objects are 0x6ef0 apart and the largest offset is 0x5bb8 (0x64 + 0x3744 + 0x1b7c +
// 0x894), so no value is ambiguous.
std::optional<u32> ContextBaseFromCell(u32 cell)
{
  for (const u32 base : {XD_CTX_A, XD_CTX_B})
  {
    if (cell == base)
      return base;
    for (u32 t = 0; t < 2; ++t)
    {
      for (u32 sl = 0; sl < 2; ++sl)
      {
        const u32 slot =
            base + XD_TRAINER_OFF + t * XD_TRAINER_STRIDE + XD_SLOT_OFF + sl * XD_SLOT_STRIDE;
        if (cell == slot)
          return base;
      }
    }
  }
  return std::nullopt;
}

size_t AppendEmu(const Memory::MemoryManager& memory, u8* dst, size_t at, u32 addr, u32 len)
{
  memory.CopyFromEmu(dst + at, addr, len);
  return at + len;
}

// FNV-1a over a u32, byte by byte in a fixed order (no type punning).
u32 MixWord(u32 h, u32 v)
{
  for (int shift = 0; shift < 32; shift += 8)
  {
    h ^= (v >> shift) & 0xFF;
    h *= 16777619u;
  }
  return h;
}
}  // namespace

XdDigest ComputeXdDigest(const Memory::MemoryManager& memory)
{
  XdDigest d;
  if (memory.Read_U32(0x80000000) != XD_DISC_ID_GXXE)
    return d;
  d.is_xd = true;
  d.seed = memory.Read_U32(XD_RNG_SEED);
  d.jb = memory.Read_U32(XD_JOYBOOT_LCG);
  d.flags = memory.Read_U32(XD_BATTLE_FLAGS);
  d.ctx = memory.Read_U32(XD_CTX_PTR);

  // 2 x (6 x 0x50 + 2 x 0x40) = 1216 bytes is the largest range set.
  std::array<u8, 2048> buf{};
  size_t n = 0;
  for (const XdRange& r : XD_CRC_A)
    n = AppendEmu(memory, buf.data(), n, r.addr, r.len);
  d.crc_a = Common::ComputeCRC32(buf.data(), n);

  if (const std::optional<u32> base = ContextBaseFromCell(d.ctx))
  {
    n = 0;
    for (u32 t = 0; t < 2; t++)
    {
      const u32 trainer = *base + XD_TRAINER_OFF + t * XD_TRAINER_STRIDE;
      for (u32 i = 0; i < 6; i++)
      {
        const u32 record = trainer + XD_PARTY_OFF + i * XD_PARTY_STRIDE + 4;
        n = AppendEmu(memory, buf.data(), n, record, 0x30);
        n = AppendEmu(memory, buf.data(), n, record + 0x80, 0x20);
      }
      for (u32 sl = 0; sl < 2; sl++)
        n = AppendEmu(memory, buf.data(), n, trainer + XD_SLOT_OFF + sl * XD_SLOT_STRIDE, 0x40);
    }
    d.crc_b = Common::ComputeCRC32(buf.data(), n);
  }

  n = 0;
  for (const XdRange& r : XD_CRC_C)
    n = AppendEmu(memory, buf.data(), n, r.addr, r.len);
  d.crc_c = Common::ComputeCRC32(buf.data(), n);
  return d;
}

u32 XdDigestHash(const XdDigest& digest)
{
  u32 h = 2166136261u;
  for (const u32 v : {digest.is_xd ? 1u : 0u, digest.seed, digest.jb, digest.flags, digest.ctx,
                      digest.crc_a, digest.crc_b, digest.crc_c})
  {
    h = MixWord(h, v);
  }
  return h;
}
}  // namespace HLE_XD
