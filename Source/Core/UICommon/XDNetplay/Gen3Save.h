// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

#include "UICommon/XDNetplay/Gen3Mon.h"

namespace XDNetplay
{
// One section whose stored footer checksum does not match the computed one.
struct ChecksumMismatch
{
  int block = 0;
  int slot = 0;
  int section_id = 0;
  u32 stored = 0;
  u32 calculated = 0;
};

// Which Gen 3 game wrote a save image. The block/section/footer layout is
// identical across all five cartridges; what differs is the per-section
// checksum length table and some section-interior offsets, so game-aware
// callers pick behavior off this enum. EmeraldSave::DetectGame is the single
// authoritative detector on every platform (mirrored in Gen3Save.kt).
enum class Gen3Game
{
  RubySapphire,
  FireRedLeafGreen,
  Emerald,
};

// "Ruby/Sapphire" / "FireRed/LeafGreen" / "Emerald", for user-facing messages.
const char* GameDisplayName(Gen3Game game);

// Pokemon Emerald (Gen 3) save file: 131072 bytes of flash plus any trailing
// bytes (e.g. the 16-byte mGBA RTC footer), which are preserved verbatim.
//
// Ported 1:1 from the Android Kotlin port (Gen3Save.kt) of the empirically
// validated Python reference:
//  - Flash holds two 57344-byte game-save blocks of 14 rotating 4096-byte
//    sections each; the physical slot of logical section N shifts every save.
//  - Section footer at +0x0FF4: u16 section id, u16 checksum,
//    u32 signature (0x08012025), u32 save index (shared by the block).
//  - Active block = the fully valid block with the higher save index.
//  - Section checksum: sum of the section's data bytes as little-endian u32s
//    over that section's data length, folded to u16 as
//    ((sum & 0xFFFF) + (sum >> 16)) & 0xFFFF.
class EmeraldSave
{
public:
  static constexpr size_t SECTION_SIZE = 4096;
  static constexpr size_t SECTION_FOOTER_OFFSET = 0x0FF4;
  static constexpr size_t SECTIONS_PER_BLOCK = 14;
  static constexpr size_t BLOCK_SIZE = SECTION_SIZE * SECTIONS_PER_BLOCK;  // 57344
  static constexpr u32 SAVE_SIGNATURE = 0x08012025;

  // Section 1 (Team/Items) offsets - Emerald.
  static constexpr size_t PARTY_COUNT_OFFSET = 0x0234;  // u32
  static constexpr size_t PARTY_DATA_OFFSET = 0x0238;   // 6 * 100 bytes
  static constexpr size_t PARTY_MAX = 6;

  // Section 0 (Trainer Info) offsets - Emerald.
  static constexpr size_t TRAINER_NAME_OFFSET = 0x0000;  // 7 bytes + 0xFF
  static constexpr size_t TRAINER_NAME_LEN = 7;
  // The field the game reserves is 8 bytes (pokeemerald: playerName is
  // PLAYER_NAME_LENGTH + 1): 7 usable characters plus a byte that is ALWAYS the
  // 0xFF terminator, even for a full-length name. Byte 8 is the gender.
  static constexpr size_t TRAINER_NAME_FIELD_SIZE = TRAINER_NAME_LEN + 1;
  static constexpr size_t TRAINER_GENDER_OFFSET = 0x0008;
  static constexpr size_t TRAINER_ID_OFFSET = 0x000A;  // u32: low public, high secret
  // u32 read by DetectGame: 0 in Ruby/Sapphire (the field is unused there),
  // the constant 1 in FireRed/LeafGreen, and Emerald's security key -- an
  // effectively random nonzero value -- otherwise. Same heuristic every Gen 3
  // save tool uses; the bundled team saves carry 0xAA2F3199 / 0xCF0579AC.
  static constexpr size_t GAME_CODE_OFFSET = 0x00AC;

  // Per-section valid data length used by the checksum (Emerald). Index =
  // logical section ID. Empirically confirmed: with these exact lengths every
  // section checksum in the test saves matches.
  static const std::array<u32, SECTIONS_PER_BLOCK> EMERALD_SECTION_DATA_LENGTHS;
  // Ruby/Sapphire and FireRed/LeafGreen counterparts; see the transcription
  // note in Gen3Save.cpp before trusting either table.
  static const std::array<u32, SECTIONS_PER_BLOCK> RS_SECTION_DATA_LENGTHS;
  static const std::array<u32, SECTIONS_PER_BLOCK> FRLG_SECTION_DATA_LENGTHS;
  static const std::array<u32, SECTIONS_PER_BLOCK>& SectionDataLengths(Gen3Game game);

  // THE authoritative game detector (see Gen3Game). The save must have been
  // parsed by Create so section 0 is mapped.
  static Gen3Game DetectGame(const EmeraldSave& save);

  // Parse a save image. Returns std::nullopt (with *error set, if given) when
  // the file is too small or no fully valid block exists.
  static std::optional<EmeraldSave> Create(std::vector<u8> raw, std::string* error = nullptr);

  // Sum data_length bytes starting at base as little-endian u32s (mod 2^32),
  // then fold to u16: ((sum & 0xFFFF) + (sum >> 16)) & 0xFFFF.
  static u32 SectionChecksum(const u8* buf, size_t base, u32 data_length);

  int GetActiveBlock() const { return m_active_block; }

  // Bytes after the two game-save blocks (Hall of Fame area, emulator RTC
  // footer, ...). Preserved verbatim; exposed for inspection only.
  std::vector<u8> GetExtraBytes() const;

  // The full file image (flash + any trailing bytes), reflecting all edits.
  const std::vector<u8>& ToBytes() const { return m_raw; }

  // Copy of the 4096-byte physical section holding logical section_id.
  std::vector<u8> SectionBytes(size_t section_id) const;

  // Check every section footer checksum in BOTH blocks. Returns the list of
  // mismatches (empty when the file is fully consistent). Game-aware callers
  // pass DetectGame's answer; the Emerald default keeps the bundled-save
  // paths (and every pre-import caller) byte-identical in behavior.
  std::vector<ChecksumMismatch> VerifyAllChecksums(Gen3Game game = Gen3Game::Emerald) const;

  void UpdateSectionChecksum(size_t section_id, Gen3Game game = Gen3Game::Emerald);

  // -- trainer info (logical section 0) --------------------------------------

  std::string GetTrainerName() const;

  // Overwrite the trainer name in the trainer block and fix section 0's
  // checksum. name is UTF-8 and must be 1..TRAINER_NAME_LEN (7) codepoints,
  // every one of them encodable in the Gen 3 charset -- an unencodable
  // character fails the call (with *error naming it) and leaves the save
  // completely untouched, rather than writing a '?' or a truncated field.
  //
  // NOTE for callers that also rebuild the party: every Pokemon carries its own
  // OT name copy, so renaming the trainer does NOT rename the mons already in
  // the party. Set the name FIRST, then build/re-stamp the party from it.
  bool SetTrainerName(const std::string& name, std::string* error = nullptr);

  // Coerce arbitrary (possibly untrusted, possibly over-long) text into
  // something SetTrainerName will accept: trim surrounding whitespace, map
  // straight quotes to the charset's curly forms, DROP anything still
  // unencodable, clamp to TRAINER_NAME_LEN codepoints. Returns "" when nothing
  // survives -- callers decide whether that means "keep the existing name" or
  // "reject the request".
  static std::string SanitizeTrainerName(const std::string& name);

  // Raw u32: low u16 = public (visible) trainer ID, high u16 = secret ID.
  u32 GetTrainerId() const;
  u32 GetTrainerPublicId() const { return GetTrainerId() & 0xFFFF; }
  u32 GetTrainerSecretId() const { return (GetTrainerId() >> 16) & 0xFFFF; }
  u32 GetTrainerGender() const;

  // -- party (logical section 1) ---------------------------------------------

  // Stored party count, clamped to 0..6 so a corrupt count cannot drive reads
  // past the party area.
  size_t GetPartyCount() const;

  // Decode the party. Returns std::nullopt (with *error) if a non-empty slot
  // fails its substructure checksum.
  std::optional<std::vector<Gen3Mon>> ReadParty(std::string* error = nullptr) const;

  // Serialize mons into section 1, zero unused slots, fix the count and the
  // section checksum. Fails (false) if mons.size() > PARTY_MAX.
  bool WriteParty(const std::vector<Gen3Mon>& mons);

  // Re-encode the current party over itself (round-trip identity helper).
  bool RewritePartyInPlace(std::string* error = nullptr);

private:
  EmeraldSave() = default;

  // Returns (save index, all valid) for a block; the index is taken from any
  // valid section (all 14 share it). Index is -1 if no section was valid.
  std::pair<s64, bool> BlockIndexAndValid(int block) const;
  std::optional<int> FindActiveBlock() const;
  void MapSections(int block);

  std::vector<u8> m_raw;
  int m_active_block = 0;
  std::array<size_t, SECTIONS_PER_BLOCK> m_section_offsets{};
};

#ifndef XDNETPLAY_STANDALONE_NO_FILE_IO
// Verified-write discipline (mirrors the validated Android/Python injectors):
// re-parse the edited image and check every checksum, write it to path.tmp,
// read the tmp back and compare byte-identical, back up an existing file to
// path.bak, then rename the tmp into place. Returns false with *error set on
// any failure; the original file is never touched until the tmp has been
// verified.
bool VerifiedWriteSaveFile(const std::string& path, const EmeraldSave& save,
                           std::string* error = nullptr);

// Read + parse a save file from disk.
std::optional<EmeraldSave> LoadSaveFile(const std::string& path, std::string* error = nullptr);
#endif
}  // namespace XDNetplay
