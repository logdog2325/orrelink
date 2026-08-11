// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/Gen3Save.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "UICommon/XDNetplay/Gen3Text.h"

#ifndef XDNETPLAY_STANDALONE_NO_FILE_IO
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#endif

namespace XDNetplay
{
using namespace Gen3Bytes;

const std::array<u32, EmeraldSave::SECTIONS_PER_BLOCK> EmeraldSave::EMERALD_SECTION_DATA_LENGTHS = {
    3884,  // 0  Trainer info
    3968,  // 1  Team / items
    3968,  // 2  Game state
    3968,  // 3  Misc data
    3848,  // 4  Rival info
    3968,  // 5  PC buffer A
    3968,  // 6  PC buffer B
    3968,  // 7  PC buffer C
    3968,  // 8  PC buffer D
    3968,  // 9  PC buffer E
    3968,  // 10 PC buffer F
    3968,  // 11 PC buffer G
    3968,  // 12 PC buffer H
    2000,  // 13 PC buffer I
};

// Ruby/Sapphire and FireRed/LeafGreen checksum the same 14 sections over
// different lengths. Each length is the sizeof() of that game's own save
// struct chunk -- transcribed from the decomps' chunk tables
// (pret/pokeruby src/save.c sSaveBlockChunks, pret/pokefirered src/save.c
// sSaveSlotLayout: section 0 = SaveBlock2, 1..4 = SaveBlock1 split at 3968,
// 5..13 = PokemonStorage) with the struct sizes as documented in PKHeX's
// SAV3 buffers (0x890/0xC40 RS, 0xF24/0xEE8 FRLG, 0x83D0 storage for all).
// Bytes past a section's length are zeroed by the game's write buffer, which
// is why Emerald-length tools APPEAR to work on RS/FRLG saves; verifying
// against the game's own stored checksums still needs the real lengths.
const std::array<u32, EmeraldSave::SECTIONS_PER_BLOCK> EmeraldSave::RS_SECTION_DATA_LENGTHS = {
    2192,  // 0  Trainer info (sizeof(SaveBlock2) = 0x890)
    3968,  // 1  Team / items
    3968,  // 2  Game state
    3968,  // 3  Misc data
    3136,  // 4  Rival info (SaveBlock1 = 0x3AC0 leaves 0xC40)
    3968,  // 5  PC buffer A
    3968,  // 6  PC buffer B
    3968,  // 7  PC buffer C
    3968,  // 8  PC buffer D
    3968,  // 9  PC buffer E
    3968,  // 10 PC buffer F
    3968,  // 11 PC buffer G
    3968,  // 12 PC buffer H
    2000,  // 13 PC buffer I
};

const std::array<u32, EmeraldSave::SECTIONS_PER_BLOCK> EmeraldSave::FRLG_SECTION_DATA_LENGTHS = {
    3876,  // 0  Trainer info (sizeof(SaveBlock2) = 0xF24)
    3968,  // 1  Team / items
    3968,  // 2  Game state
    3968,  // 3  Misc data
    3816,  // 4  Rival info (SaveBlock1 = 0x3D68 leaves 0xEE8)
    3968,  // 5  PC buffer A
    3968,  // 6  PC buffer B
    3968,  // 7  PC buffer C
    3968,  // 8  PC buffer D
    3968,  // 9  PC buffer E
    3968,  // 10 PC buffer F
    3968,  // 11 PC buffer G
    3968,  // 12 PC buffer H
    2000,  // 13 PC buffer I
};

const std::array<u32, EmeraldSave::SECTIONS_PER_BLOCK>&
EmeraldSave::SectionDataLengths(Gen3Game game)
{
  switch (game)
  {
  case Gen3Game::RubySapphire:
    return RS_SECTION_DATA_LENGTHS;
  case Gen3Game::FireRedLeafGreen:
    return FRLG_SECTION_DATA_LENGTHS;
  case Gen3Game::Emerald:
  default:
    return EMERALD_SECTION_DATA_LENGTHS;
  }
}

const char* GameDisplayName(Gen3Game game)
{
  switch (game)
  {
  case Gen3Game::RubySapphire:
    return "Ruby/Sapphire";
  case Gen3Game::FireRedLeafGreen:
    return "FireRed/LeafGreen";
  case Gen3Game::Emerald:
  default:
    return "Emerald";
  }
}

Gen3Game EmeraldSave::DetectGame(const EmeraldSave& save)
{
  const std::vector<u8> sec0 = save.SectionBytes(0);
  const u32 code = ReadU32(sec0.data(), GAME_CODE_OFFSET);
  if (code == 0)
    return Gen3Game::RubySapphire;
  if (code == 1)
    return Gen3Game::FireRedLeafGreen;
  return Gen3Game::Emerald;
}

u32 EmeraldSave::SectionChecksum(const u8* buf, size_t base, u32 data_length)
{
  u32 total = 0;
  for (u32 i = 0; i < data_length; i += 4)
    total += ReadU32(buf, base + i);
  return ((total & 0xFFFF) + (total >> 16)) & 0xFFFF;
}

std::optional<EmeraldSave> EmeraldSave::Create(std::vector<u8> raw, std::string* error)
{
  if (raw.size() < 2 * BLOCK_SIZE)
  {
    if (error)
      *error = "file too small for an Emerald save";
    return std::nullopt;
  }

  EmeraldSave save;
  save.m_raw = std::move(raw);
  const std::optional<int> active = save.FindActiveBlock();
  if (!active)
  {
    if (error)
      *error = "no valid save block found";
    return std::nullopt;
  }
  save.m_active_block = *active;
  save.MapSections(save.m_active_block);
  return save;
}

std::pair<s64, bool> EmeraldSave::BlockIndexAndValid(int block) const
{
  s64 idx = -1;
  bool valid = true;
  std::array<bool, SECTIONS_PER_BLOCK> seen{};
  size_t seen_count = 0;
  for (size_t slot = 0; slot < SECTIONS_PER_BLOCK; slot++)
  {
    const size_t off = block * BLOCK_SIZE + slot * SECTION_SIZE;
    const u32 sid = ReadU16(m_raw.data(), off + SECTION_FOOTER_OFFSET);
    const u32 sig = ReadU32(m_raw.data(), off + SECTION_FOOTER_OFFSET + 4);
    const u32 sidx = ReadU32(m_raw.data(), off + SECTION_FOOTER_OFFSET + 8);
    if (sig != SAVE_SIGNATURE || sid >= SECTIONS_PER_BLOCK)
    {
      valid = false;
      continue;
    }
    if (!seen[sid])
    {
      seen[sid] = true;
      seen_count++;
    }
    if (idx == -1)
      idx = sidx;
    else if (static_cast<s64>(sidx) != idx)
      valid = false;
  }
  if (seen_count != SECTIONS_PER_BLOCK)
    valid = false;
  return {idx, valid};
}

std::optional<int> EmeraldSave::FindActiveBlock() const
{
  const auto [idx0, ok0] = BlockIndexAndValid(0);
  const auto [idx1, ok1] = BlockIndexAndValid(1);
  if (ok0 && ok1)
    return idx1 > idx0 ? 1 : 0;
  if (ok1)
    return 1;
  if (ok0)
    return 0;
  return std::nullopt;
}

void EmeraldSave::MapSections(int block)
{
  for (size_t slot = 0; slot < SECTIONS_PER_BLOCK; slot++)
  {
    const size_t off = block * BLOCK_SIZE + slot * SECTION_SIZE;
    const u32 sid = ReadU16(m_raw.data(), off + SECTION_FOOTER_OFFSET);
    if (sid < SECTIONS_PER_BLOCK)
      m_section_offsets[sid] = off;
  }
}

std::vector<u8> EmeraldSave::GetExtraBytes() const
{
  return std::vector<u8>(m_raw.begin() + 2 * BLOCK_SIZE, m_raw.end());
}

std::vector<u8> EmeraldSave::SectionBytes(size_t section_id) const
{
  const size_t off = m_section_offsets[section_id];
  return std::vector<u8>(m_raw.begin() + off, m_raw.begin() + off + SECTION_SIZE);
}

std::vector<ChecksumMismatch> EmeraldSave::VerifyAllChecksums(Gen3Game game) const
{
  const std::array<u32, SECTIONS_PER_BLOCK>& lengths = SectionDataLengths(game);
  std::vector<ChecksumMismatch> bad;
  for (int block = 0; block <= 1; block++)
  {
    for (size_t slot = 0; slot < SECTIONS_PER_BLOCK; slot++)
    {
      const size_t off = block * BLOCK_SIZE + slot * SECTION_SIZE;
      const u32 sid = ReadU16(m_raw.data(), off + SECTION_FOOTER_OFFSET);
      const u32 stored = ReadU16(m_raw.data(), off + SECTION_FOOTER_OFFSET + 2);
      if (sid >= SECTIONS_PER_BLOCK)
        continue;
      const u32 calc = SectionChecksum(m_raw.data(), off, lengths[sid]);
      if (calc != stored)
      {
        bad.push_back({block, static_cast<int>(slot), static_cast<int>(sid), stored, calc});
      }
    }
  }
  return bad;
}

void EmeraldSave::UpdateSectionChecksum(size_t section_id, Gen3Game game)
{
  const size_t off = m_section_offsets[section_id];
  const u32 calc = SectionChecksum(m_raw.data(), off, SectionDataLengths(game)[section_id]);
  WriteU16(m_raw.data(), off + SECTION_FOOTER_OFFSET + 2, calc);
}

std::string EmeraldSave::GetTrainerName() const
{
  return Gen3Text::Decode(m_raw.data() + m_section_offsets[0] + TRAINER_NAME_OFFSET,
                          TRAINER_NAME_LEN);
}

bool EmeraldSave::SetTrainerName(const std::string& name, std::string* error)
{
  if (name.empty())
  {
    if (error)
      *error = "trainer name cannot be empty";
    return false;
  }
  // Length is counted in CODEPOINTS, not bytes: '♀' is three UTF-8 bytes and
  // one Gen 3 byte. Encode() would silently truncate an over-long name, which
  // is the wrong answer for a name a human just typed -- say so instead.
  if (Gen3Text::DecodeUtf8(name).size() > TRAINER_NAME_LEN)
  {
    if (error)
      *error = "trainer name is longer than " + std::to_string(TRAINER_NAME_LEN) + " characters";
    return false;
  }
  // Encode first: it fails (leaving the save untouched) on any character the
  // Gen 3 charset has no byte for, rather than writing a '?' placeholder.
  const auto encoded = Gen3Text::Encode(name, TRAINER_NAME_LEN, error);
  if (!encoded)
    return false;

  const size_t off = m_section_offsets[0] + TRAINER_NAME_OFFSET;
  std::memcpy(m_raw.data() + off, encoded->data(), encoded->size());
  // Encode() already 0xFF-pads the 7 usable bytes; byte 7 is the field's hard
  // terminator, which the game keeps at 0xFF even for a 7-character name.
  m_raw[off + TRAINER_NAME_LEN] = Gen3Text::TERMINATOR;
  static_assert(TRAINER_NAME_OFFSET + TRAINER_NAME_FIELD_SIZE == TRAINER_GENDER_OFFSET,
                "trainer name field must stop before the gender byte");
  UpdateSectionChecksum(0);
  return true;
}

std::string EmeraldSave::SanitizeTrainerName(const std::string& name)
{
  // Trim first: a leading space is an encodable character (0x00) and would
  // otherwise eat one of the seven slots and render as a blank in-game.
  const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  size_t begin = 0;
  size_t end = name.size();
  while (begin < end && is_space(name[begin]))
    ++begin;
  while (end > begin && is_space(name[end - 1]))
    --end;

  // Sanitize (drop unencodable) THEN trim again: dropping an unencodable
  // character can expose new surrounding whitespace, e.g. "Lo 🐬 gan".
  std::string trimmed = Gen3Text::Sanitize(name.substr(begin, end - begin), TRAINER_NAME_LEN);
  while (!trimmed.empty() && is_space(trimmed.back()))
    trimmed.pop_back();
  size_t lead = 0;
  while (lead < trimmed.size() && is_space(trimmed[lead]))
    ++lead;
  return trimmed.substr(lead);
}

u32 EmeraldSave::GetTrainerId() const
{
  return ReadU32(m_raw.data(), m_section_offsets[0] + TRAINER_ID_OFFSET);
}

u32 EmeraldSave::GetTrainerGender() const
{
  return ReadU8(m_raw.data(), m_section_offsets[0] + TRAINER_GENDER_OFFSET);
}

size_t EmeraldSave::GetPartyCount() const
{
  const u32 count = ReadU32(m_raw.data(), m_section_offsets[1] + PARTY_COUNT_OFFSET);
  return std::min<size_t>(count, PARTY_MAX);
}

std::optional<std::vector<Gen3Mon>> EmeraldSave::ReadParty(std::string* error) const
{
  const size_t off = m_section_offsets[1];
  std::vector<Gen3Mon> mons;
  const size_t count = GetPartyCount();
  for (size_t i = 0; i < count; i++)
  {
    auto mon = Gen3Mon::FromBytes(m_raw.data() + off + PARTY_DATA_OFFSET + i * Gen3Mon::SIZE, error);
    if (!mon)
      return std::nullopt;
    mons.push_back(std::move(*mon));
  }
  return mons;
}

bool EmeraldSave::WriteParty(const std::vector<Gen3Mon>& mons)
{
  if (mons.size() > PARTY_MAX)
    return false;
  const size_t off = m_section_offsets[1];
  WriteU32(m_raw.data(), off + PARTY_COUNT_OFFSET, static_cast<u32>(mons.size()));
  for (size_t i = 0; i < PARTY_MAX; i++)
  {
    u8* slot = m_raw.data() + off + PARTY_DATA_OFFSET + i * Gen3Mon::SIZE;
    if (i < mons.size())
    {
      const std::array<u8, Gen3Mon::SIZE> bytes = mons[i].Encode();
      std::memcpy(slot, bytes.data(), bytes.size());
    }
    else
    {
      std::memset(slot, 0, Gen3Mon::SIZE);
    }
  }
  UpdateSectionChecksum(1);
  return true;
}

bool EmeraldSave::RewritePartyInPlace(std::string* error)
{
  const auto party = ReadParty(error);
  if (!party)
    return false;
  return WriteParty(*party);
}

#ifndef XDNETPLAY_STANDALONE_NO_FILE_IO
std::optional<EmeraldSave> LoadSaveFile(const std::string& path, std::string* error)
{
  File::IOFile file(path, "rb");
  if (!file)
  {
    if (error)
      *error = "could not open save file: " + path;
    return std::nullopt;
  }
  std::vector<u8> data(file.GetSize());
  if (!file.ReadBytes(data.data(), data.size()))
  {
    if (error)
      *error = "could not read save file: " + path;
    return std::nullopt;
  }
  return EmeraldSave::Create(std::move(data), error);
}

bool VerifiedWriteSaveFile(const std::string& path, const EmeraldSave& save, std::string* error)
{
  const std::vector<u8>& bytes = save.ToBytes();

  // Re-parse the edited image and verify it before touching the disk.
  std::string parse_error;
  const auto reparsed = EmeraldSave::Create(bytes, &parse_error);
  if (!reparsed)
  {
    if (error)
      *error = "edited save does not re-parse: " + parse_error;
    return false;
  }
  if (!reparsed->VerifyAllChecksums().empty())
  {
    if (error)
      *error = "edited save has checksum mismatches";
    return false;
  }
  if (!reparsed->ReadParty(&parse_error))
  {
    if (error)
      *error = "edited party does not decode: " + parse_error;
    return false;
  }

  // Write to a tmp file and read it back byte-identical.
  const std::string tmp_path = path + ".tmp";
  {
    File::IOFile tmp(tmp_path, "wb");
    if (!tmp || !tmp.WriteBytes(bytes.data(), bytes.size()) || !tmp.Flush())
    {
      if (error)
        *error = "could not write " + tmp_path;
      return false;
    }
  }
  {
    File::IOFile tmp(tmp_path, "rb");
    std::vector<u8> readback(bytes.size());
    if (!tmp || tmp.GetSize() != bytes.size() ||
        !tmp.ReadBytes(readback.data(), readback.size()) || readback != bytes)
    {
      if (error)
        *error = "tmp readback mismatch for " + tmp_path;
      File::Delete(tmp_path);
      return false;
    }
  }

  // Back up the existing save, then rename the verified tmp into place.
  if (File::Exists(path) && !File::CopyRegularFile(path, path + ".bak"))
  {
    if (error)
      *error = "could not back up " + path;
    File::Delete(tmp_path);
    return false;
  }
  if (!File::Rename(tmp_path, path))
  {
    if (error)
      *error = "could not rename " + tmp_path + " into place";
    return false;
  }
  return true;
}
#endif  // XDNETPLAY_STANDALONE_NO_FILE_IO
}  // namespace XDNetplay
