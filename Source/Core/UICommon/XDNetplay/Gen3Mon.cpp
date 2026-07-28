// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/Gen3Mon.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "UICommon/XDNetplay/Gen3Text.h"

namespace XDNetplay
{
namespace
{
using namespace Gen3Bytes;

// XOR each little-endian u32 of the 48-byte block with key (symmetric).
void XorBlockInPlace(u8* block48, u32 key)
{
  for (size_t i = 0; i < 48; i += 4)
    WriteU32(block48, i, ReadU32(block48, i) ^ key);
}

// u16 sum of the decrypted 48-byte block taken as 24 u16 words.
u32 DataChecksum(const u8* decrypted48)
{
  u32 total = 0;
  for (size_t i = 0; i < 48; i += 2)
    total = (total + ReadU16(decrypted48, i)) & 0xFFFF;
  return total;
}
}  // namespace

const std::array<const char*, 24> Gen3Mon::SUBSTRUCT_ORDER = {
    "GAEM", "GAME", "GEAM", "GEMA", "GMAE", "GMEA", "AGEM", "AGME", "AEGM", "AEMG", "AMGE", "AMEG",
    "EGAM", "EGMA", "EAGM", "EAMG", "EMGA", "EMAG", "MGAE", "MGEA", "MAGE", "MAEG", "MEGA", "MEAG",
};

const std::array<const char*, 25> Gen3Mon::NATURES = {
    "Hardy",  "Lonely", "Brave",   "Adamant", "Naughty", "Bold",   "Docile",  "Relaxed", "Impish",
    "Lax",    "Timid",  "Hasty",   "Serious", "Jolly",   "Naive",  "Modest",  "Mild",    "Quiet",
    "Bashful", "Rash",  "Calm",    "Gentle",  "Sassy",   "Careful", "Quirky",
};

std::string Gen3Mon::GetNickname() const
{
  return Gen3Text::Decode(nickname_raw.data(), nickname_raw.size());
}

std::string Gen3Mon::GetOtName() const
{
  return Gen3Text::Decode(ot_name_raw.data(), ot_name_raw.size());
}

const char* Gen3Mon::GetNatureName() const
{
  return NATURES[GetNature()];
}

std::array<u32, 6> Gen3Mon::GetIvs() const
{
  std::array<u32, 6> out{};
  for (size_t i = 0; i < 6; i++)
    out[i] = (iv_egg_ability >> (5 * i)) & 0x1F;
  return out;
}

void Gen3Mon::SetIvs(const std::array<u32, 6>& ivs)
{
  u32 v = iv_egg_ability & ~u32{0x3FFFFFFF};
  for (size_t i = 0; i < 6; i++)
    v |= (ivs[i] & 0x1F) << (5 * i);
  iv_egg_ability = v;
}

void Gen3Mon::SetAbilitySlot(u32 slot)
{
  iv_egg_ability = (iv_egg_ability & 0x7FFFFFFF) | ((slot & 1) << 31);
}

std::array<u8, Gen3Mon::SIZE> Gen3Mon::Encode() const
{
  std::array<u8, SIZE> buf{};
  WriteU32(buf.data(), 0x00, pid);
  WriteU32(buf.data(), 0x04, ot_id);
  std::memcpy(buf.data() + 0x08, nickname_raw.data(), nickname_raw.size());
  WriteU16(buf.data(), 0x12, language);
  std::memcpy(buf.data() + 0x14, ot_name_raw.data(), ot_name_raw.size());
  WriteU8(buf.data(), 0x1B, markings);
  WriteU16(buf.data(), 0x1E, unknown_1e);

  u8 g[12]{};
  WriteU16(g, 0, species);
  WriteU16(g, 2, held_item);
  WriteU32(g, 4, experience);
  WriteU8(g, 8, pp_bonuses);
  WriteU8(g, 9, friendship);
  WriteU16(g, 10, growth_unknown);

  u8 a[12]{};
  for (size_t i = 0; i < 4; i++)
  {
    WriteU16(a, 2 * i, moves[i]);
    WriteU8(a, 8 + i, pp[i]);
  }

  u8 e[12]{};
  for (size_t i = 0; i < 6; i++)
  {
    WriteU8(e, i, evs[i]);
    WriteU8(e, 6 + i, contest[i]);
  }

  u8 m[12]{};
  WriteU8(m, 0, pokerus);
  WriteU8(m, 1, met_location);
  WriteU16(m, 2, origins);
  WriteU32(m, 4, iv_egg_ability);
  WriteU32(m, 8, ribbons);

  const char* order = SUBSTRUCT_ORDER[pid % 24];
  u8 decrypted[48]{};
  for (size_t slot = 0; slot < 4; slot++)
  {
    const u8* sub = nullptr;
    switch (order[slot])
    {
    case 'G':
      sub = g;
      break;
    case 'A':
      sub = a;
      break;
    case 'E':
      sub = e;
      break;
    default:
      sub = m;
      break;
    }
    std::memcpy(decrypted + slot * 12, sub, 12);
  }

  WriteU16(buf.data(), 0x1C, DataChecksum(decrypted));
  XorBlockInPlace(decrypted, pid ^ ot_id);
  std::memcpy(buf.data() + 0x20, decrypted, 48);

  WriteU32(buf.data(), 0x50, status);
  WriteU8(buf.data(), 0x54, level);
  WriteU8(buf.data(), 0x55, pokerus_days);
  WriteU16(buf.data(), 0x56, current_hp);
  WriteU16(buf.data(), 0x58, max_hp);
  WriteU16(buf.data(), 0x5A, attack);
  WriteU16(buf.data(), 0x5C, defense);
  WriteU16(buf.data(), 0x5E, speed);
  WriteU16(buf.data(), 0x60, sp_attack);
  WriteU16(buf.data(), 0x62, sp_defense);
  return buf;
}

bool Gen3Mon::IsEmptySlotBytes(const u8* buf)
{
  return std::all_of(buf, buf + SIZE, [](u8 b) { return b == 0; });
}

std::optional<Gen3Mon> Gen3Mon::FromBytes(const u8* buf, std::string* error)
{
  Gen3Mon mon;
  mon.pid = ReadU32(buf, 0x00);
  mon.ot_id = ReadU32(buf, 0x04);
  std::memcpy(mon.nickname_raw.data(), buf + 0x08, mon.nickname_raw.size());
  mon.language = ReadU16(buf, 0x12);
  std::memcpy(mon.ot_name_raw.data(), buf + 0x14, mon.ot_name_raw.size());
  mon.markings = ReadU8(buf, 0x1B);
  const u32 stored_checksum = ReadU16(buf, 0x1C);
  mon.unknown_1e = ReadU16(buf, 0x1E);

  u8 decrypted[48];
  std::memcpy(decrypted, buf + 0x20, 48);
  XorBlockInPlace(decrypted, mon.pid ^ mon.ot_id);

  if (!IsEmptySlotBytes(buf))
  {
    const u32 calc = DataChecksum(decrypted);
    if (calc != stored_checksum)
    {
      if (error)
      {
        char msg[80];
        std::snprintf(msg, sizeof(msg),
                      "substructure checksum mismatch: stored=0x%04X calc=0x%04X", stored_checksum,
                      calc);
        *error = msg;
      }
      return std::nullopt;
    }
  }

  const char* order = SUBSTRUCT_ORDER[mon.pid % 24];
  const u8* g = nullptr;
  const u8* a = nullptr;
  const u8* e = nullptr;
  const u8* m = nullptr;
  for (size_t slot = 0; slot < 4; slot++)
  {
    const u8* sub = decrypted + slot * 12;
    switch (order[slot])
    {
    case 'G':
      g = sub;
      break;
    case 'A':
      a = sub;
      break;
    case 'E':
      e = sub;
      break;
    default:
      m = sub;
      break;
    }
  }

  mon.species = ReadU16(g, 0);
  mon.held_item = ReadU16(g, 2);
  mon.experience = ReadU32(g, 4);
  mon.pp_bonuses = ReadU8(g, 8);
  mon.friendship = ReadU8(g, 9);
  mon.growth_unknown = ReadU16(g, 10);

  for (size_t i = 0; i < 4; i++)
  {
    mon.moves[i] = ReadU16(a, 2 * i);
    mon.pp[i] = ReadU8(a, 8 + i);
  }

  for (size_t i = 0; i < 6; i++)
  {
    mon.evs[i] = ReadU8(e, i);
    mon.contest[i] = ReadU8(e, 6 + i);
  }

  mon.pokerus = ReadU8(m, 0);
  mon.met_location = ReadU8(m, 1);
  mon.origins = ReadU16(m, 2);
  mon.iv_egg_ability = ReadU32(m, 4);
  mon.ribbons = ReadU32(m, 8);

  mon.status = ReadU32(buf, 0x50);
  mon.level = ReadU8(buf, 0x54);
  mon.pokerus_days = ReadU8(buf, 0x55);
  mon.current_hp = ReadU16(buf, 0x56);
  mon.max_hp = ReadU16(buf, 0x58);
  mon.attack = ReadU16(buf, 0x5A);
  mon.defense = ReadU16(buf, 0x5C);
  mon.speed = ReadU16(buf, 0x5E);
  mon.sp_attack = ReadU16(buf, 0x60);
  mon.sp_defense = ReadU16(buf, 0x62);
  return mon;
}
}  // namespace XDNetplay
