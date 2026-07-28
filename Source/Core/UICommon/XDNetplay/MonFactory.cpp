// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/MonFactory.h"

#include <algorithm>
#include <array>
#include <cctype>

#include "UICommon/XDNetplay/Gen3Text.h"

namespace XDNetplay::MonFactory
{
namespace
{
constexpr u32 LANGUAGE_ENGLISH = 0x0202;
constexpr u32 DEFAULT_FRIENDSHIP = 70;
constexpr u32 DEFAULT_PP = 35;  // mirrors the Python injector
constexpr u32 GAME_EMERALD = 3;
constexpr u32 BALL_POKE = 4;  // the Python injector's default ball
constexpr u32 OT_GENDER_MALE = 0;
constexpr u32 SHINY_THRESHOLD = 8;

// Gender ratio sentinel values (never PID-dependent).
constexpr int RATIO_ALWAYS_MALE = 0;
constexpr int RATIO_ALWAYS_FEMALE = 254;
constexpr int RATIO_GENDERLESS = 255;

// Remap hp/atk/def/spa/spd/spe (Showdown order) to
// hp/atk/def/speed/spatk/spdef (Gen 3 save order).
std::array<int, 6> ShowdownToGen3Order(const std::array<int, 6>& sd)
{
  return {sd[0], sd[1], sd[2], sd[5], sd[3], sd[4]};
}

// Move name -> id. "Hidden Power [Ice]" / "Hidden Power Ice" resolve to plain
// Hidden Power (Gen 3 stores no HP type; it derives from IVs).
std::optional<int> ResolveMoveId(const std::string& name, const Gen3Data& data)
{
  if (const auto id = data.MoveId(name))
    return id;
  if (Gen3Data::NormalizeName(name).rfind("hiddenpower", 0) == 0)
  {
    if (const auto id = data.MoveId("hiddenpower"))
      return id;
  }
  return std::nullopt;
}

// Make text Gen 3-encodable: straight ASCII quotes become the charset's curly
// forms, anything else unencodable is dropped.
std::string SanitizeText(const std::string& text)
{
  std::u32string out;
  for (const char32_t c : Gen3Text::DecodeUtf8(text))
  {
    char32_t mapped = c;
    if (c == U'\'')
      mapped = U'’';
    else if (c == U'"')
      mapped = U'”';
    if (Gen3Text::CanEncode(mapped))
      out.push_back(mapped);
  }
  return Gen3Text::EncodeUtf8(out);
}

// Gen 3 stat formula, integer division exactly as the game:
//   HP     = (2*base + IV + EV/4) * level / 100 + level + 10   (Shedinja: 1)
//   others = ((2*base + IV + EV/4) * level / 100 + 5) * nature
// where the nature multiplier is *110/100 or *90/100 (integer division), with
// nature plus/minus indices 0=atk 1=def 2=speed 3=spatk 4=spdef. Arrays are
// in Gen 3 order: hp, atk, def, speed, spatk, spdef.
std::array<int, 6> ComputeStats(const Gen3Data::Species& species, int level,
                                const std::array<int, 6>& ivs, const std::array<int, 6>& evs,
                                const Gen3Data::Nature& nature)
{
  std::array<int, 6> out{};
  if (species.name == "shedinja")
  {
    out[0] = 1;
  }
  else
  {
    out[0] = (2 * species.base_stats[0] + ivs[0] + evs[0] / 4) * level / 100 + level + 10;
  }
  for (int i = 1; i <= 5; i++)
  {
    int v = (2 * species.base_stats[i] + ivs[i] + evs[i] / 4) * level / 100 + 5;
    const int stat_index = i - 1;  // nature indices skip HP
    if (nature.plus == stat_index && nature.minus != stat_index)
      v = v * 110 / 100;
    else if (nature.minus == stat_index && nature.plus != stat_index)
      v = v * 90 / 100;
    out[i] = v;
  }
  return out;
}

bool IsShinyPid(u32 pid, u32 trainer_id)
{
  const u32 tid = trainer_id & 0xFFFF;
  const u32 sid = (trainer_id >> 16) & 0xFFFF;
  const u32 pid_low = pid & 0xFFFF;
  const u32 pid_high = (pid >> 16) & 0xFFFF;
  return (tid ^ sid ^ pid_high ^ pid_low) < SHINY_THRESHOLD;
}

bool GenderOk(u32 pid_low_byte, int gender_ratio, char wanted_gender)
{
  if (gender_ratio == RATIO_GENDERLESS || gender_ratio == RATIO_ALWAYS_FEMALE ||
      gender_ratio == RATIO_ALWAYS_MALE)
  {
    // Gender is fixed by the species regardless of PID.
    return true;
  }
  switch (wanted_gender)
  {
  case 'F':
    return static_cast<int>(pid_low_byte) < gender_ratio;  // female iff (PID & 0xFF) < ratio
  case 'M':
    return static_cast<int>(pid_low_byte) >= gender_ratio;
  default:
    return true;
  }
}

// Deterministic PID search. For shiny requests: enumerate the low u16
// (parity = ability slot, low byte satisfying gender), derive the eight shiny
// high u16 values, and take the first PID with the right nature. Non-shiny
// (or shiny-unsatisfiable, relaxed): start from the Python reference's
// nature-preserving base (0x12345678 rounded to the nature) and step by 25,
// which cycles the low u16 through every value, until parity + gender match
// and the PID is not accidentally shiny.
std::optional<u32> ChoosePid(u32 nature_id, u32 ability_slot, int gender_ratio, char wanted_gender,
                             bool shiny, u32 trainer_id)
{
  const u32 tid = trainer_id & 0xFFFF;
  const u32 sid = (trainer_id >> 16) & 0xFFFF;

  if (shiny)
  {
    for (u32 low = 0; low <= 0xFFFF; low++)
    {
      if ((low & 1) != ability_slot)
        continue;
      if (!GenderOk(low & 0xFF, gender_ratio, wanted_gender))
        continue;
      for (u32 r = 0; r < SHINY_THRESHOLD; r++)
      {
        const u32 high = (tid ^ sid ^ low ^ r) & 0xFFFF;
        const u32 pid = (high << 16) | low;
        if (pid % 25 == nature_id)
          return pid;
      }
    }
    // No shiny PID satisfies nature+ability+gender: relax shiny.
  }

  // Nature-preserving scan (mirrors find_pid_for_nature's base). No overflow:
  // base + 25 * 0x20000 stays far below 2^31.
  constexpr u32 base = 0x12345678;
  u32 pid = base - (base % 25) + nature_id;
  for (u32 step = 0; step < 0x20000; step++)
  {
    if ((pid & 1) == ability_slot && GenderOk(pid & 0xFF, gender_ratio, wanted_gender) &&
        !IsShinyPid(pid, trainer_id))
    {
      return pid;
    }
    pid += 25;
  }
  // Unreachable: the low u16 cycles through all values (gcd(25, 2^16)=1), so
  // parity+gender always hit, and shininess excludes at most 8/65536.
  return std::nullopt;
}

std::string Upper(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return text;
}
}  // namespace

std::optional<Gen3Mon> Build(const ShowdownSet& set, const Gen3Data& data,
                             const std::string& trainer_name, u32 trainer_id, std::string* error)
{
  const auto fail = [error](const std::string& message) -> std::optional<Gen3Mon> {
    if (error)
      *error = message;
    return std::nullopt;
  };

  const Gen3Data::Species* species = data.FindSpecies(set.species);
  if (!species)
    return fail("unknown species: " + set.species);

  const int level = std::clamp(set.level, 1, 100);

  // Nature: default Hardy (neutral) when the set omits it, so the stats we
  // store match the nature the PID encodes.
  Gen3Data::Nature nature{"hardy", 0, -1, -1};
  if (set.nature)
  {
    const Gen3Data::Nature* found = data.FindNature(*set.nature);
    if (!found)
      return fail("unknown nature: " + *set.nature);
    nature = *found;
  }
  else if (const Gen3Data::Nature* hardy = data.FindNature("hardy"))
  {
    nature = *hardy;
  }

  // Ability slot: 1 only when the requested ability is the species' distinct
  // second ability; otherwise 0.
  u32 ability_slot = 0;
  if (set.ability)
  {
    const std::optional<int> ability_id = data.AbilityId(*set.ability);
    if (!ability_id)
      return fail("unknown ability: " + *set.ability);
    if (*ability_id == species->abilities[1] && species->abilities[1] != 0 &&
        species->abilities[1] != species->abilities[0])
    {
      ability_slot = 1;
    }
  }

  const std::optional<u32> pid =
      ChoosePid(static_cast<u32>(nature.id), ability_slot, species->gender_ratio, set.gender,
                set.shiny, trainer_id);
  if (!pid)
    return fail("no PID satisfies the requested constraints");

  // Remap hp/atk/def/spa/spd/spe (Showdown order) to Gen 3 save order.
  std::array<int, 6> evs = ShowdownToGen3Order(set.evs);
  std::array<int, 6> ivs = ShowdownToGen3Order(set.ivs);
  for (int i = 0; i < 6; i++)
  {
    ivs[i] = std::clamp(ivs[i], 0, 31);
    evs[i] = std::clamp(evs[i], 0, 255);
  }

  std::array<u32, 4> move_ids{};
  for (size_t i = 0; i < std::min<size_t>(set.moves.size(), 4); i++)
  {
    const std::optional<int> id = ResolveMoveId(set.moves[i], data);
    if (!id)
      return fail("unknown move: " + set.moves[i]);
    move_ids[i] = static_cast<u32>(*id);
  }

  u32 held_item = 0;
  if (set.item)
  {
    const std::optional<int> item_id = data.ItemId(*set.item);
    if (!item_id)
      return fail("unknown item: " + *set.item);
    held_item = static_cast<u32>(*item_id);
  }

  const std::optional<int> experience = data.ExpForLevel(species->exp_group, level);
  if (!experience)
    return fail("unknown experience group: " + species->exp_group);

  const std::array<int, 6> stats = ComputeStats(*species, level, ivs, evs, nature);

  const std::string nickname = SanitizeText(set.nickname ? *set.nickname : Upper(set.species));
  const auto nickname_bytes = Gen3Text::Encode(nickname, 10, error);
  if (!nickname_bytes)
    return std::nullopt;
  const auto ot_name_bytes = Gen3Text::Encode(SanitizeText(trainer_name), 7, error);
  if (!ot_name_bytes)
    return std::nullopt;

  Gen3Mon mon;
  mon.pid = *pid;
  mon.ot_id = trainer_id;
  std::copy(nickname_bytes->begin(), nickname_bytes->end(), mon.nickname_raw.begin());
  mon.language = LANGUAGE_ENGLISH;
  std::copy(ot_name_bytes->begin(), ot_name_bytes->end(), mon.ot_name_raw.begin());
  mon.species = static_cast<u32>(species->id);
  mon.held_item = held_item;
  mon.experience = static_cast<u32>(*experience);
  mon.pp_bonuses = 0;
  mon.friendship = DEFAULT_FRIENDSHIP;
  mon.moves = move_ids;
  for (size_t i = 0; i < 4; i++)
    mon.pp[i] = move_ids[i] != 0 ? DEFAULT_PP : 0;
  for (size_t i = 0; i < 6; i++)
    mon.evs[i] = static_cast<u32>(evs[i]);
  mon.met_location = 0;
  mon.origins = (static_cast<u32>(level) & 0x7F) | ((GAME_EMERALD & 0xF) << 7) |
                ((BALL_POKE & 0xF) << 11) | ((OT_GENDER_MALE & 1) << 15);
  std::array<u32, 6> iv_array{};
  for (size_t i = 0; i < 6; i++)
    iv_array[i] = static_cast<u32>(ivs[i]);
  mon.SetIvs(iv_array);
  mon.SetAbilitySlot(ability_slot);
  mon.level = static_cast<u32>(level);
  mon.max_hp = static_cast<u32>(stats[0]);
  mon.current_hp = static_cast<u32>(stats[0]);
  mon.attack = static_cast<u32>(stats[1]);
  mon.defense = static_cast<u32>(stats[2]);
  mon.speed = static_cast<u32>(stats[3]);
  mon.sp_attack = static_cast<u32>(stats[4]);
  mon.sp_defense = static_cast<u32>(stats[5]);
  return mon;
}
}  // namespace XDNetplay::MonFactory
