// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"

namespace XDNetplay
{
// Little-endian byte helpers shared by the Gen 3 save/mon code. All reads
// mask to the unsigned width; u32 values are raw bit patterns.
namespace Gen3Bytes
{
inline u32 ReadU8(const u8* buf, size_t off)
{
  return buf[off];
}

inline u32 ReadU16(const u8* buf, size_t off)
{
  return static_cast<u32>(buf[off]) | (static_cast<u32>(buf[off + 1]) << 8);
}

inline u32 ReadU32(const u8* buf, size_t off)
{
  return static_cast<u32>(buf[off]) | (static_cast<u32>(buf[off + 1]) << 8) |
         (static_cast<u32>(buf[off + 2]) << 16) | (static_cast<u32>(buf[off + 3]) << 24);
}

inline void WriteU8(u8* buf, size_t off, u32 value)
{
  buf[off] = static_cast<u8>(value & 0xFF);
}

inline void WriteU16(u8* buf, size_t off, u32 value)
{
  buf[off] = static_cast<u8>(value & 0xFF);
  buf[off + 1] = static_cast<u8>((value >> 8) & 0xFF);
}

inline void WriteU32(u8* buf, size_t off, u32 value)
{
  buf[off] = static_cast<u8>(value & 0xFF);
  buf[off + 1] = static_cast<u8>((value >> 8) & 0xFF);
  buf[off + 2] = static_cast<u8>((value >> 16) & 0xFF);
  buf[off + 3] = static_cast<u8>((value >> 24) & 0xFF);
}
}  // namespace Gen3Bytes

// Fully decoded Gen 3 party Pokemon (100-byte struct).
//
// Ported 1:1 from the Android Kotlin port (Gen3Mon.kt) of the empirically
// validated Python reference. FromBytes()/Encode() round-trip
// byte-identically.
//
// Box portion (80 bytes):
//   0x00 u32  personality (PID)
//   0x04 u32  OT ID (low u16 public, high u16 secret)
//   0x08 10B  nickname (Gen 3 charset)
//   0x12 u16  language (0x0202 = English)
//   0x14 7B   OT name
//   0x1B u8   markings
//   0x1C u16  checksum (u16-sum of DECRYPTED 48-byte data block)
//   0x1E u16  unknown/padding (preserved verbatim)
//   0x20 48B  encrypted data: four 12-byte substructures, order = PID % 24,
//             each u32 XORed with key = PID ^ OTID
// Party-only portion (20 bytes):
//   0x50 u32 status, 0x54 u8 level, 0x55 u8 pokerus days, 0x56 u16 curHP,
//   0x58 u16 maxHP, 0x5A atk, 0x5C def, 0x5E spe, 0x60 spa, 0x62 spd
struct Gen3Mon
{
  // Size of a party Pokemon struct in bytes.
  static constexpr size_t SIZE = 100;

  // Box header
  u32 pid = 0;
  u32 ot_id = 0;
  std::array<u8, 10> nickname_raw{};  // raw bytes (kept for exact round-trip)
  u32 language = 0x0202;
  std::array<u8, 7> ot_name_raw{};
  u32 markings = 0;
  u32 unknown_1e = 0;

  // Growth substructure
  u32 species = 0;
  u32 held_item = 0;
  u32 experience = 0;
  u32 pp_bonuses = 0;
  u32 friendship = 255;  // game max; MonFactory/Decode overwrite
  u32 growth_unknown = 0;

  // Attacks substructure
  std::array<u32, 4> moves{};
  std::array<u32, 4> pp{};

  // EVs substructure (+ contest stats)
  std::array<u32, 6> evs{};      // hp, atk, def, speed, spatk, spdef
  std::array<u32, 6> contest{};  // cool, beauty, cute, smart, tough, feel

  // Misc substructure
  u32 pokerus = 0;
  u32 met_location = 0;
  u32 origins = 0;         // bits: 0-6 level met, 7-10 game, 11-14 ball, 15 OT gender
  u32 iv_egg_ability = 0;  // bits: 6x5-bit IVs, 30 is_egg, 31 ability slot
  u32 ribbons = 0;

  // Party-only portion
  u32 status = 0;
  u32 level = 0;
  u32 pokerus_days = 0;
  u32 current_hp = 0;
  u32 max_hp = 0;
  u32 attack = 0;
  u32 defense = 0;
  u32 speed = 0;
  u32 sp_attack = 0;
  u32 sp_defense = 0;

  // -- derived accessors -----------------------------------------------------

  std::string GetNickname() const;
  std::string GetOtName() const;

  // Nature index 0..24 = PID % 25.
  u32 GetNature() const { return pid % 25; }
  const char* GetNatureName() const;

  // IVs as (hp, atk, def, speed, spatk, spdef), 5 bits each.
  std::array<u32, 6> GetIvs() const;
  void SetIvs(const std::array<u32, 6>& ivs);

  u32 IsEgg() const { return (iv_egg_ability >> 30) & 1; }
  u32 GetAbilitySlot() const { return (iv_egg_ability >> 31) & 1; }
  void SetAbilitySlot(u32 slot);

  bool IsEmpty() const { return pid == 0 && species == 0; }

  // Rebuild the 100-byte struct from decoded fields (re-encrypts and
  // recomputes the substructure checksum).
  std::array<u8, SIZE> Encode() const;

  // Substructure order by PID % 24. G=Growth A=Attacks E=EVs M=Misc.
  static const std::array<const char*, 24> SUBSTRUCT_ORDER;
  static const std::array<const char*, 25> NATURES;

  // An unused party slot is all zeroes (PID 0, OTID 0, checksum 0).
  static bool IsEmptySlotBytes(const u8* buf);

  // Decode a 100-byte party struct at buf. Verifies the substructure checksum
  // for non-empty slots; returns std::nullopt (with *error set, if given) on
  // mismatch.
  static std::optional<Gen3Mon> FromBytes(const u8* buf, std::string* error = nullptr);
};
}  // namespace XDNetplay
