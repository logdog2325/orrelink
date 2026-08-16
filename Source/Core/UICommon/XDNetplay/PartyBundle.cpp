// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/PartyBundle.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "UICommon/XDNetplay/Gen3Text.h"

namespace XDNetplay::PartyBundle
{
using namespace Gen3Bytes;

namespace
{
constexpr char BASE64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 0..63 for alphabet bytes, -1 otherwise ('=' is handled by the caller).
int Base64Value(char c)
{
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

// Copy the 8-byte raw trainer-name field with its padding normalized: the
// terminator byte at [7] is forced (the games always keep it 0xFF, even for a
// full-length name) and everything past the FIRST terminator becomes 0xFF too,
// so stray bytes beyond the visible name can never ride a bundle onto another
// machine. The visible bytes themselves are preserved verbatim -- they must
// stay byte-identical to the OT-name copies inside the mons, or the game would
// treat the whole party as traded outsiders (see the header).
std::array<u8, EmeraldSave::TRAINER_NAME_FIELD_SIZE> NormalizeNameField(const u8* raw)
{
  std::array<u8, EmeraldSave::TRAINER_NAME_FIELD_SIZE> name{};
  bool terminated = false;
  for (size_t i = 0; i < EmeraldSave::TRAINER_NAME_LEN; i++)
  {
    terminated |= raw[i] == Gen3Text::TERMINATOR;
    name[i] = terminated ? Gen3Text::TERMINATOR : raw[i];
  }
  name[EmeraldSave::TRAINER_NAME_LEN] = Gen3Text::TERMINATOR;
  return name;
}
}  // namespace

std::optional<std::vector<u8>> Extract(const EmeraldSave& save, std::string* error)
{
  const auto fail = [error](std::string message) {
    if (error)
      *error = std::move(message);
    return std::nullopt;
  };

  // Emerald and Ruby/Sapphire share every offset this reads (section-0 trainer
  // block and section-1 party area, verified); FireRed/LeafGreen does not --
  // its party lives elsewhere in section 1, and because the section checksum
  // lengths coincide, Emerald-offset reads from an FRLG save return plausible
  // garbage instead of failing. Refuse loudly, same reasoning as the host-side
  // FRLG refusal in TeamInjector.
  if (EmeraldSave::DetectGame(save) == Gen3Game::FireRedLeafGreen)
  {
    return fail("this is a FireRed/LeafGreen save, which stores its party at different offsets -- "
                "only Emerald and Ruby/Sapphire saves can be submitted directly");
  }

  // ReadParty already reads the newest valid slot (EmeraldSave::Create picked
  // the active block) and verifies each non-empty slot's substructure
  // checksum -- the same reader every other consumer of party data uses.
  std::string party_error;
  const auto party = save.ReadParty(&party_error);
  if (!party)
    return fail("party unreadable (" + party_error + ")");
  if (party->empty())
    return fail("the save's party is empty -- catch at least one Pokemon first");

  const std::vector<u8> section0 = save.SectionBytes(0);
  const auto name =
      NormalizeNameField(section0.data() + EmeraldSave::TRAINER_NAME_OFFSET);
  if (Gen3Text::Decode(name.data(), EmeraldSave::TRAINER_NAME_LEN).empty())
    return fail("the save's trainer name is empty");

  const u32 gender = save.GetTrainerGender();
  if (gender > 1)
    return fail("the save's trainer gender byte is corrupt");

  std::vector<u8> bundle(BUNDLE_SIZE, 0);
  WriteU32(bundle.data(), COUNT_OFFSET, static_cast<u32>(party->size()));
  for (size_t i = 0; i < party->size(); i++)
  {
    // Encode() round-trips FromBytes() byte-identically, so the wire carries
    // the mon's REAL bytes -- IVs, ribbons, origin data and all -- not a
    // rebuilt approximation. Slots past the count stay all-zero.
    const std::array<u8, Gen3Mon::SIZE> mon = (*party)[i].Encode();
    std::memcpy(bundle.data() + PARTY_OFFSET + i * Gen3Mon::SIZE, mon.data(), mon.size());
  }
  std::memcpy(bundle.data() + NAME_OFFSET, name.data(), name.size());
  WriteU8(bundle.data(), GENDER_OFFSET, gender);
  WriteU32(bundle.data(), TRAINER_ID_OFFSET, save.GetTrainerId());
  return bundle;
}

std::optional<Decoded> Validate(const std::vector<u8>& bundle, std::string* error)
{
  const auto fail = [error](std::string message) {
    if (error)
      *error = std::move(message);
    return std::nullopt;
  };

  // Length EXACT: the bundle is a fixed shape, and both ends of a session run
  // the same build (netplay refuses mismatched versions), so any other size is
  // a malformed or hand-crafted payload, never a compatibility case.
  if (bundle.size() != BUNDLE_SIZE)
  {
    return fail("wrong size: expected " + std::to_string(BUNDLE_SIZE) + " bytes, got " +
                std::to_string(bundle.size()));
  }

  const u32 count = ReadU32(bundle.data(), COUNT_OFFSET);
  if (count < 1 || count > MON_SLOTS)
    return fail("party count " + std::to_string(count) + " is outside 1..6");

  Decoded out;
  for (size_t i = 0; i < MON_SLOTS; i++)
  {
    const u8* slot = bundle.data() + PARTY_OFFSET + i * Gen3Mon::SIZE;
    if (i < count)
    {
      // The existing mon reader does the heavy lifting: substructure
      // decryption and checksum verification. An all-zero slot decodes
      // "successfully" as empty (FromBytes only checksums non-empty slots),
      // so emptiness is rejected explicitly -- a counted slot must hold a
      // real Pokemon.
      std::string mon_error;
      auto mon = Gen3Mon::FromBytes(slot, &mon_error);
      if (!mon)
        return fail("party slot " + std::to_string(i + 1) + " invalid (" + mon_error + ")");
      if (mon->IsEmpty())
        return fail("party slot " + std::to_string(i + 1) + " is empty but counted");
      out.mons.push_back(std::move(*mon));
    }
    else
    {
      // Slots past the count must be ALL zero -- extraction writes them that
      // way, so any other content is smuggled data.
      const bool all_zero =
          std::all_of(slot, slot + Gen3Mon::SIZE, [](u8 b) { return b == 0; });
      if (!all_zero)
        return fail("party slot " + std::to_string(i + 1) + " has data beyond the party count");
    }
  }

  const u8* name_raw = bundle.data() + NAME_OFFSET;
  // Extraction normalizes the field, so a legitimate bundle always passes:
  // hard terminator at [7], only terminator padding after the visible name.
  const auto normalized = NormalizeNameField(name_raw);
  if (!std::equal(normalized.begin(), normalized.end(), name_raw))
    return fail("trainer name field is not terminator-normalized");
  out.name_raw = normalized;
  out.trainer_name = Gen3Text::Decode(out.name_raw.data(), EmeraldSave::TRAINER_NAME_LEN);
  if (out.trainer_name.empty())
    return fail("trainer name is empty");

  const u32 gender = ReadU8(bundle.data(), GENDER_OFFSET);
  if (gender > 1)
    return fail("trainer gender byte is out of range");
  out.gender = static_cast<u8>(gender);
  out.trainer_id = ReadU32(bundle.data(), TRAINER_ID_OFFSET);
  return out;
}

std::optional<EmeraldSave> BuildSave(std::vector<u8> template_bytes,
                                     const std::vector<u8>& bundle, std::string* error)
{
  const auto fail = [error](std::string message) {
    if (error)
      *error = std::move(message);
    return std::nullopt;
  };

  auto decoded = Validate(bundle, error);
  if (!decoded)
    return std::nullopt;

  std::string parse_error;
  auto save = EmeraldSave::Create(std::move(template_bytes), &parse_error);
  if (!save)
    return fail("bundled template unreadable (" + parse_error + ")");
  // The disposable save is always built from the bundled EMERALD template;
  // anything else here means the template file was tampered with or swapped.
  if (EmeraldSave::DetectGame(*save) != Gen3Game::Emerald)
    return fail("bundled template is not an Emerald save");

  // Trainer identity goes in RAW: the name bytes must stay byte-identical to
  // the OT-name copies inside the party mons (they came from the same save),
  // and the save's TID must equal the mons' OT ID, or the game would rule the
  // party traded outsiders. This is the whole reason bundles do NOT re-stamp
  // OTs the way MonFactory does for Showdown teams -- with identity and party
  // both copied from the guest's save, obedience is intrinsically correct.
  save->SetTrainerIdentityRaw(decoded->name_raw, decoded->gender, decoded->trainer_id);

  // The party bytes are the guest's own, already validated; WriteParty
  // re-encodes them byte-identically, zeroes the unused slots and fixes
  // section 1's checksum.
  if (!save->WriteParty(decoded->mons))
    return fail("party too large");  // unreachable: Validate capped it at 6

  // Reconstruct BOTH rotating save slots: copy the edited active block over
  // the inactive one, save index and all. Identical images mean whichever
  // slot the game (or any tool) treats as newest holds the bundle's party and
  // trainer, and the game's next in-game save rotates to the other slot with
  // index+1 exactly as normal. Section checksums ride along with the copy, so
  // the whole file verifies. Any bytes past the two blocks (emulator footer)
  // are preserved untouched.
  std::vector<u8> bytes = save->ToBytes();
  const size_t active_off = static_cast<size_t>(save->GetActiveBlock()) * EmeraldSave::BLOCK_SIZE;
  const size_t other_off = (save->GetActiveBlock() == 0 ? 1 : 0) * EmeraldSave::BLOCK_SIZE;
  std::memcpy(bytes.data() + other_off, bytes.data() + active_off, EmeraldSave::BLOCK_SIZE);

  auto rebuilt = EmeraldSave::Create(std::move(bytes), &parse_error);
  if (!rebuilt)
    return fail("rebuilt save does not re-parse (" + parse_error + ")");
  return rebuilt;
}

std::string Base64Encode(const std::vector<u8>& data)
{
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 3 <= data.size(); i += 3)
  {
    const u32 n = (static_cast<u32>(data[i]) << 16) | (static_cast<u32>(data[i + 1]) << 8) |
                  static_cast<u32>(data[i + 2]);
    out.push_back(BASE64_ALPHABET[(n >> 18) & 0x3F]);
    out.push_back(BASE64_ALPHABET[(n >> 12) & 0x3F]);
    out.push_back(BASE64_ALPHABET[(n >> 6) & 0x3F]);
    out.push_back(BASE64_ALPHABET[n & 0x3F]);
  }
  const size_t rest = data.size() - i;
  if (rest == 1)
  {
    const u32 n = static_cast<u32>(data[i]) << 16;
    out.push_back(BASE64_ALPHABET[(n >> 18) & 0x3F]);
    out.push_back(BASE64_ALPHABET[(n >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  }
  else if (rest == 2)
  {
    const u32 n = (static_cast<u32>(data[i]) << 16) | (static_cast<u32>(data[i + 1]) << 8);
    out.push_back(BASE64_ALPHABET[(n >> 18) & 0x3F]);
    out.push_back(BASE64_ALPHABET[(n >> 12) & 0x3F]);
    out.push_back(BASE64_ALPHABET[(n >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

std::optional<std::vector<u8>> Base64Decode(std::string_view text)
{
  // Strict: a positive multiple of 4 characters, '=' only as the final one or
  // two, everything else from the alphabet. No whitespace, no line wrapping --
  // the encoder never emits any, and this only ever decodes our own format.
  if (text.empty() || text.size() % 4 != 0)
    return std::nullopt;

  std::vector<u8> out;
  out.reserve((text.size() / 4) * 3);
  for (size_t i = 0; i < text.size(); i += 4)
  {
    u32 n = 0;
    size_t chars = 0;
    for (size_t j = 0; j < 4; j++)
    {
      const char c = text[i + j];
      if (c == '=')
      {
        // Padding is only legal in the final quantum, in the last two spots,
        // and never followed by a non-'=' character.
        if (i + 4 != text.size() || j < 2 || (j == 2 && text[i + 3] != '='))
          return std::nullopt;
        break;
      }
      const int value = Base64Value(c);
      if (value < 0)
        return std::nullopt;
      n = (n << 6) | static_cast<u32>(value);
      chars++;
    }
    if (chars == 4)
    {
      out.push_back(static_cast<u8>((n >> 16) & 0xFF));
      out.push_back(static_cast<u8>((n >> 8) & 0xFF));
      out.push_back(static_cast<u8>(n & 0xFF));
    }
    else if (chars == 3)
    {
      n <<= 6;
      out.push_back(static_cast<u8>((n >> 16) & 0xFF));
      out.push_back(static_cast<u8>((n >> 8) & 0xFF));
    }
    else if (chars == 2)
    {
      n <<= 12;
      out.push_back(static_cast<u8>((n >> 16) & 0xFF));
    }
    else
    {
      return std::nullopt;  // "====" or "x===": not a valid final quantum
    }
  }
  return out;
}
}  // namespace XDNetplay::PartyBundle
