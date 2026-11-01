// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/XDBattleRecorder.h"

#include <array>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Core/ConfigManager.h"
#include "Core/HW/GBAMoveNames.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace XDBattleRecorder
{
namespace
{
// XD (GXXE01) addresses and offsets. Battle floor: the current attacker, target and switch-in.
constexpr u32 FLOOR = 0x804A1730;
constexpr u32 FLOOR_TURN = FLOOR + 0x10;   // u16, the current turn
constexpr u32 FLOOR_PHASE = FLOOR + 0x12;  // u16, 0 outside a battle
constexpr u32 FLOOR_ATTACKER = FLOOR + 0xDDF4;
constexpr u32 FLOOR_TARGET = FLOOR + 0xDDF8;
constexpr u32 FLOOR_SWITCH = FLOOR + 0xDE00;
// The four battle positions: GBA 1's two, then GBA 2's. The second of each is unused in singles.
constexpr std::array<u32, 4> SLOTS{0x804A3324, 0x804A3BB8, 0x804AA214, 0x804AAAA8};
constexpr std::array<const char*, 4> SLOT_NAMES{"p1a", "p1b", "p2a", "p2b"};
constexpr u32 SLOT_POKEMON = 4;      // -> battle Pokemon; its Pokemon data starts 4 bytes in
constexpr u32 SLOT_MOVE = 0x648;     // the move in progress
constexpr u32 SLOT_STAGES = 0x7B0;   // atk, def, spa, spd, spe, accuracy, evasion; 6 is neutral
constexpr u32 MOVE_ID = 4;           // u16
constexpr u32 MOVE_RESULT = 0x69;    // 0x45 once it missed or was blocked
constexpr u32 MOVE_CRIT = 0x98;      // 2 on a critical hit
constexpr u32 MON_SPECIES = 0;       // u16
constexpr u32 MON_HP = 4;            // u16
constexpr u32 MON_LEVEL = 0x11;
constexpr u32 MON_STATUS = 0x16;
constexpr u32 MON_NAME = 0x4E;       // UTF-16, 10 characters
constexpr u32 MON_PP = 0x82;         // u8, every 4 bytes for the four moves

constexpr u8 RESULT_MISSED = 0x45;
constexpr std::array<const char*, 7> STAGE_NAMES{"atk", "def",     "spa",    "spd",
                                                 "spe", "accuracy", "evasion"};
constexpr std::array<const char*, 7> STAGE_TEXT{"Attack",  "Defense",  "Sp. Atk", "Sp. Def",
                                                "Speed",   "accuracy", "evasion"};

struct SlotState
{
  u32 pokemon = 0;
  u16 species = 0;
  int hp = 0;
  u8 level = 0;
  u8 status = 0;
  int pp = 0;
  std::array<u8, 7> stages{};
  std::string name;
};

struct Battle
{
  std::array<SlotState, 4> slots;
  std::map<u32, int> max_hp;  // by battle Pokemon: its HP when first seen
  std::vector<std::string> log;
  std::vector<std::string> text;
  std::array<u16, 4> move_this_turn{};
  int turn = 0;
  int acting = -1;
  bool crit_shown = false;
  bool result_shown = false;
};

std::optional<Battle> s_battle;
std::string s_session_stamp;
int s_battle_count = 0;

u32 Read32(const Memory::MemoryManager& mem, u32 address)
{
  return mem.Read_U32(address & 0x01FFFFFF);
}
u16 Read16(const Memory::MemoryManager& mem, u32 address)
{
  return mem.Read_U16(address & 0x01FFFFFF);
}
u8 Read8(const Memory::MemoryManager& mem, u32 address)
{
  return mem.Read_U8(address & 0x01FFFFFF);
}

bool IsRamPointer(u32 address)
{
  return address >= 0x80000000 && address < 0x81800000;
}

std::optional<int> SlotOf(u32 pointer)
{
  for (size_t i = 0; i < SLOTS.size(); ++i)
  {
    if (SLOTS[i] == pointer)
      return static_cast<int>(i);
  }
  return std::nullopt;
}

SlotState ReadSlot(const Memory::MemoryManager& mem, int index)
{
  SlotState state;
  const u32 slot = SLOTS[index];
  state.pokemon = Read32(mem, slot + SLOT_POKEMON);
  if (!IsRamPointer(state.pokemon))
  {
    state.pokemon = 0;
    return state;
  }
  const u32 mon = state.pokemon + 4;
  state.species = Read16(mem, mon + MON_SPECIES);
  state.hp = Read16(mem, mon + MON_HP);
  state.level = Read8(mem, mon + MON_LEVEL);
  state.status = Read8(mem, mon + MON_STATUS);
  for (u32 i = 0; i < 4; ++i)
    state.pp += Read8(mem, mon + MON_PP + i * 4);
  for (u32 i = 0; i < state.stages.size(); ++i)
    state.stages[i] = Read8(mem, slot + SLOT_STAGES + i);
  for (u32 i = 0; i < 10; ++i)
  {
    const u16 c = Read16(mem, mon + MON_NAME + i * 2);
    if (c == 0)
      break;
    state.name.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  if (state.species == 0 || state.level == 0)
    state.pokemon = 0;
  return state;
}

std::string SpeciesKey(u16 species)
{
  return species < std::size(GBADetectLog::GEN3_SPECIES_IDS) ?
             std::string(GBADetectLog::GEN3_SPECIES_IDS[species]) :
             fmt::format("pokemon{}", species);
}

std::string MoveName(u16 id)
{
  return id < std::size(GBADetectLog::GEN3_MOVE_NAMES) && id != 0 ?
             std::string(GBADetectLog::GEN3_MOVE_NAMES[id]) :
             fmt::format("Move {}", id);
}

std::string Who(const Battle& battle, int slot)
{
  return fmt::format("{}: {}", SLOT_NAMES[slot], battle.slots[slot].name);
}

std::string TextWho(const Battle& battle, int slot)
{
  return fmt::format("GBA {} {}", slot / 2 + 1, battle.slots[slot].name);
}

const char* StatusId(u8 status)
{
  switch (status)
  {
  case 3:
    return "psn";
  case 4:
    return "tox";
  case 5:
    return "par";
  case 6:
    return "brn";
  case 7:
    return "frz";
  case 8:
    return "slp";
  default:
    return nullptr;
  }
}

void AddSwitch(Battle& battle, int slot)
{
  const SlotState& state = battle.slots[slot];
  const int max = battle.max_hp.try_emplace(state.pokemon, state.hp).first->second;
  battle.log.push_back(fmt::format("|switch|{}|{}, L{}|{}/{}", Who(battle, slot),
                                   SpeciesKey(state.species), state.level, state.hp, max));
  battle.text.push_back(fmt::format("{} comes in ({}/{})", TextWho(battle, slot), state.hp, max));
}

void Step(const Memory::MemoryManager& mem, Battle& battle)
{
  const u32 attacker = Read32(mem, FLOOR_ATTACKER);
  const u32 target = Read32(mem, FLOOR_TARGET);

  const int turn = Read16(mem, FLOOR_TURN);
  if (turn != battle.turn && turn != 0)
  {
    battle.turn = turn;
    battle.move_this_turn = {};
    battle.log.push_back("|");
    battle.log.push_back(fmt::format("|turn|{}", turn));
    battle.text.push_back(fmt::format("Turn {}", turn));
  }

  for (int i = 0; i < 4; ++i)
  {
    const SlotState now = ReadSlot(mem, i);
    SlotState& was = battle.slots[i];
    if (now.pokemon == 0)
      continue;

    if (now.pokemon != was.pokemon)
    {
      was = now;
      AddSwitch(battle, i);
      continue;
    }

    // Move used: its PP went down (flinched, asleep or fainted Pokemon spend none).
    if (now.pp < was.pp && attacker == SLOTS[i])
    {
      const u16 move = Read16(mem, SLOTS[i] + SLOT_MOVE + MOVE_ID);
      const std::optional<int> aim = SlotOf(target);
      battle.log.push_back("|");
      battle.log.push_back(fmt::format("|move|{}|{}|{}", Who(battle, i), MoveName(move),
                                       aim ? Who(battle, *aim) : std::string{}));
      battle.text.push_back(fmt::format("  {}: {}{}", TextWho(battle, i), MoveName(move),
                                        aim ? " on " + TextWho(battle, *aim) : std::string{}));
      battle.move_this_turn[i] = move;
      battle.acting = i;
      battle.crit_shown = false;
      battle.result_shown = false;
    }
    was.pp = now.pp;

    if (now.hp != was.hp)
    {
      const int max = battle.max_hp.try_emplace(now.pokemon, was.hp).first->second;
      const std::string who = Who(battle, i);
      if (now.hp < was.hp)
      {
        if (battle.acting >= 0 && !battle.crit_shown && target == SLOTS[i] &&
            Read8(mem, SLOTS[battle.acting] + SLOT_MOVE + MOVE_CRIT) == 2)
        {
          battle.log.push_back(fmt::format("|-crit|{}", who));
          battle.text.push_back("    A critical hit!");
          battle.crit_shown = true;
        }
        battle.log.push_back(now.hp == 0 ? fmt::format("|-damage|{}|0 fnt", who) :
                                           fmt::format("|-damage|{}|{}/{}", who, now.hp, max));
      }
      else
      {
        battle.log.push_back(fmt::format("|-heal|{}|{}/{}", who, now.hp, max));
      }
      battle.text.push_back(fmt::format("    {} {} -> {}", TextWho(battle, i), was.hp, now.hp));
      if (now.hp == 0)
      {
        battle.log.push_back(fmt::format("|faint|{}", who));
        battle.text.push_back(fmt::format("    {} fainted", TextWho(battle, i)));
      }
      was.hp = now.hp;
    }

    if (now.status != was.status)
    {
      const std::string who = Who(battle, i);
      if (const char* id = StatusId(now.status))
      {
        battle.log.push_back(fmt::format("|-status|{}|{}", who, id));
        battle.text.push_back(fmt::format("    {} is now {}", TextWho(battle, i), id));
      }
      else if (const char* old_id = StatusId(was.status))
      {
        battle.log.push_back(fmt::format("|-curestatus|{}|{}", who, old_id));
        battle.text.push_back(fmt::format("    {} is cured of {}", TextWho(battle, i), old_id));
      }
      was.status = now.status;
    }

    for (size_t s = 0; s < now.stages.size(); ++s)
    {
      const int change = now.stages[s] - was.stages[s];
      if (change == 0)
        continue;
      battle.log.push_back(fmt::format("|{}|{}|{}|{}", change > 0 ? "-boost" : "-unboost",
                                       Who(battle, i), STAGE_NAMES[s], std::abs(change)));
      battle.text.push_back(fmt::format("    {} {} {:+}", TextWho(battle, i), STAGE_TEXT[s],
                                        change));
    }
    was.stages = now.stages;
  }

  // Missed or blocked. A Protect or Detect by the target this turn says which.
  if (battle.acting >= 0 && !battle.result_shown &&
      Read8(mem, SLOTS[battle.acting] + SLOT_MOVE + MOVE_RESULT) == RESULT_MISSED)
  {
    battle.result_shown = true;
    const std::optional<int> aim = SlotOf(target);
    if (aim && *aim != battle.acting)
    {
      const u16 move = battle.move_this_turn[*aim];
      if (move == 182 || move == 197)
      {
        battle.log.push_back(fmt::format("|-activate|{}|Protect", Who(battle, *aim)));
        battle.text.push_back(fmt::format("    {} protected itself", TextWho(battle, *aim)));
      }
      else
      {
        battle.log.push_back(fmt::format("|-miss|{}|{}", Who(battle, battle.acting),
                                         Who(battle, *aim)));
        battle.text.push_back("    It missed or failed");
      }
    }
  }
}

std::string Stamp()
{
  const std::time_t now = std::time(nullptr);
  std::array<char, 32> buffer{};
  std::strftime(buffer.data(), buffer.size(), "%Y%m%d_%H%M%S", std::localtime(&now));
  return buffer.data();
}

void WriteBattle(Battle& battle)
{
  if (battle.text.size() <= 1)
    return;

  std::array<bool, 2> alive{};
  for (size_t i = 0; i < battle.slots.size(); ++i)
    alive[i / 2] = alive[i / 2] || (battle.slots[i].pokemon != 0 && battle.slots[i].hp > 0);
  // Only the Pokemon on the field are known here, so a side with one on the bench can look beaten.
  if (alive[0] != alive[1])
  {
    battle.log.push_back("|");
    battle.log.push_back(fmt::format("|win|GBA {}", alive[0] ? 1 : 2));
    battle.text.push_back(fmt::format("GBA {} wins.", alive[0] ? 1 : 2));
  }
  else
  {
    battle.text.push_back("Battle over.");
  }

  std::string log = "|gametype|doubles\n|player|p1|GBA 1||\n|player|p2|GBA 2||\n|gen|3\n"
                    "|tier|[Gen 3] OrreLink\n|start\n";
  for (const std::string& line : battle.log)
    log += line + '\n';
  std::string text = "OrreLink battle record\n\n";
  for (const std::string& line : battle.text)
    text += line + '\n';

  if (s_session_stamp.empty())
    s_session_stamp = Stamp();
  ++s_battle_count;
  const std::string base = fmt::format("{}Battle Records/battle_{}_{}",
                                       File::GetUserPath(D_GBAUSER_IDX), s_session_stamp,
                                       s_battle_count);
  File::CreateFullPath(base);
  std::ofstream(base + ".txt", std::ios::trunc) << text;
  std::ofstream(base + ".html", std::ios::trunc)
      << "<!DOCTYPE html>\n<meta charset=\"utf-8\" />\n<!-- version 1 -->\n"
         "<title>OrreLink replay</title>\n"
         "<div class=\"wrapper replay-wrapper\" style=\"max-width:1180px;margin:0 auto\">\n"
         "<input type=\"hidden\" name=\"replayid\" value=\"orrelink\" />\n"
         "<div class=\"battle\"></div><div class=\"battle-log\"></div>"
         "<div class=\"replay-controls\"></div><div class=\"replay-controls-2\"></div>\n"
         "<script type=\"text/plain\" class=\"battle-log-data\">\n"
      << log
      << "</script>\n</div>\n"
         "<script src=\"https://play.pokemonshowdown.com/js/replay-embed.js\"></script>\n";
}
}  // namespace

void OnField(Core::System& system)
{
  if (SConfig::GetInstance().GetGameID() != "GXXE01")
    return;
  Memory::MemoryManager& mem = system.GetMemory();
  if (mem.GetRAM() == nullptr)
    return;

  const bool in_battle = Read16(mem, FLOOR_PHASE) != 0;
  if (!in_battle)
  {
    if (s_battle)
      WriteBattle(*s_battle);
    s_battle.reset();
    return;
  }

  if (!s_battle)
  {
    // Wait for the first action: the field is not set up before it.
    if (!SlotOf(Read32(mem, FLOOR_ATTACKER)) && !SlotOf(Read32(mem, FLOOR_SWITCH)))
      return;
    s_battle.emplace();
    for (int i = 0; i < 4; ++i)
    {
      s_battle->slots[i] = ReadSlot(mem, i);
      if (s_battle->slots[i].pokemon != 0)
        AddSwitch(*s_battle, i);
    }
  }
  Step(mem, *s_battle);
}

void Finish()
{
  if (s_battle)
    WriteBattle(*s_battle);
  s_battle.reset();
  s_session_stamp.clear();
  s_battle_count = 0;
}
}  // namespace XDBattleRecorder
