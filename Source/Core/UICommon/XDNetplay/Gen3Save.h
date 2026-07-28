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
  static constexpr size_t TRAINER_GENDER_OFFSET = 0x0008;
  static constexpr size_t TRAINER_ID_OFFSET = 0x000A;  // u32: low public, high secret

  // Per-section valid data length used by the checksum (Emerald). Index =
  // logical section ID. Empirically confirmed: with these exact lengths every
  // section checksum in the test saves matches.
  static const std::array<u32, SECTIONS_PER_BLOCK> EMERALD_SECTION_DATA_LENGTHS;

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
  // mismatches (empty when the file is fully consistent).
  std::vector<ChecksumMismatch> VerifyAllChecksums() const;

  void UpdateSectionChecksum(size_t section_id);

  // -- trainer info (logical section 0) --------------------------------------

  std::string GetTrainerName() const;
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
