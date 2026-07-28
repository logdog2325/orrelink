// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/XDNetplay/PbrWiimmfi.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <variant>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"

#include "Core/Boot/Boot.h"
#include "Core/XDNetplay/PbrWiimmfiData.h"

#include "DiscIO/DiscUtils.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"

namespace XDNetplay::PbrWiimmfi
{
namespace
{
constexpr char PATCH_ID[] = "xd-pbr-wiimmfi";

// A DOL header is 7 text sections followed by 11 data sections; the three tables (file offset,
// virtual address, size) are each 18 contiguous big-endian u32s.
constexpr size_t DOL_SECTION_COUNT = 18;
constexpr u32 DOL_OFFSET_TABLE = 0x00;
constexpr u32 DOL_ADDRESS_TABLE = 0x48;
constexpr u32 DOL_SIZE_TABLE = 0x90;
constexpr u32 DOL_HEADER_SIZE = 0x100;

std::span<const MemoryWrite> GetWriteTable(std::string_view game_id)
{
  if (game_id == GAME_ID_USA)
    return {RPBE01_WRITES};
  if (game_id == GAME_ID_PAL)
    return {RPBP01_WRITES};
  return {};
}

u32 ReadBE32(const u8* header, u32 table, size_t index)
{
  u32 value;
  std::memcpy(&value, header + table + index * sizeof(u32), sizeof(u32));
  return Common::FromBigEndian(value);
}

// Translates a PowerPC virtual address into an offset inside the main.dol file, making sure the
// whole [address, address + length) range stays inside one section.
bool DolAddressToOffset(const u8* header, u32 address, u32 length, u64* out_offset)
{
  for (size_t i = 0; i < DOL_SECTION_COUNT; ++i)
  {
    const u32 section_offset = ReadBE32(header, DOL_OFFSET_TABLE, i);
    const u32 section_address = ReadBE32(header, DOL_ADDRESS_TABLE, i);
    const u32 section_size = ReadBE32(header, DOL_SIZE_TABLE, i);
    if (section_size == 0 || section_address == 0)
      continue;
    if (address < section_address || address - section_address >= section_size)
      continue;

    const u32 delta = address - section_address;
    if (section_size - delta < length)
      return false;
    *out_offset = u64{section_offset} + delta;
    return true;
  }
  return false;
}
}  // namespace

bool IsPbrGameID(std::string_view game_id)
{
  return game_id == GAME_ID_USA || game_id == GAME_ID_PAL || game_id == GAME_ID_JPN;
}

bool IsSupportedGameID(std::string_view game_id)
{
  return !GetWriteTable(game_id).empty();
}

std::vector<DiscIO::Riivolution::Patch> GetPatches(std::string_view game_id)
{
  const std::span<const MemoryWrite> writes = GetWriteTable(game_id);
  if (writes.empty())
    return {};

  // Built by hand rather than by round-tripping an XML string through
  // DiscIO::Riivolution::ParseString(). The parser only ever does
  //   memory.m_offset = attribute("offset").as_uint(0);
  //   memory.m_value  = ReadHexString(attribute("value"));
  // for a <memory> element (DiscIO/RiivolutionParser.cpp), so constructing the structs directly
  // is exactly equivalent to parsing the reference XML, minus a hex encode/decode round trip and
  // a parse that could silently fail at runtime.
  //
  // The addresses below are absolute PowerPC virtual addresses. That is what a Riivolution
  // "offset" attribute means too: ApplyMemoryPatch() writes to (m_offset | 0x80000000), so an
  // address that already has the 0x8 nibble set is passed through unchanged
  // (DiscIO/RiivolutionPatcher.cpp).
  std::vector<DiscIO::Riivolution::Patch> patches;
  DiscIO::Riivolution::Patch& patch = patches.emplace_back();
  patch.m_id = PATCH_ID;
  patch.m_memory_patches.reserve(writes.size());
  for (const MemoryWrite& write : writes)
  {
    DiscIO::Riivolution::Memory& memory = patch.m_memory_patches.emplace_back();
    memory.m_offset = write.address;
    memory.m_value.assign(write.bytes, write.bytes + write.length);
    // Deliberately no m_original: leaving it empty means the write is unconditional. The payload
    // is idempotent (see below), so an unconditional write cannot corrupt a pre-patched disc,
    // whereas an m_original guard would silently skip the patch on any disc revision whose stock
    // bytes we did not anticipate.
  }
  return patches;
}

DiscStatus ExamineDisc(const DiscIO::VolumeDisc& volume)
{
  const std::string game_id = volume.GetGameID();
  if (!IsPbrGameID(game_id))
    return DiscStatus::NotPbr;

  const std::span<const MemoryWrite> writes = GetWriteTable(game_id);
  if (writes.empty())
    return DiscStatus::UnsupportedRegion;

  const DiscIO::Partition partition = volume.GetGamePartition();
  const std::optional<u64> dol_offset = DiscIO::GetBootDOLOffset(volume, partition);
  if (!dol_offset)
    return DiscStatus::Unknown;

  std::array<u8, DOL_HEADER_SIZE> header{};
  if (!volume.Read(*dol_offset, header.size(), header.data(), partition))
    return DiscStatus::Unknown;

  // Only the fields the payload touches are read back, so this stays around a kilobyte of I/O
  // even though main.dol itself is several megabytes.
  std::vector<u8> buffer;
  bool all_patched = true;
  for (const MemoryWrite& write : writes)
  {
    u64 file_offset = 0;
    if (!DolAddressToOffset(header.data(), write.address, write.length, &file_offset))
      return DiscStatus::Unknown;

    buffer.resize(write.length);
    if (!volume.Read(*dol_offset + file_offset, buffer.size(), buffer.data(), partition))
      return DiscStatus::Unknown;

    if (!std::equal(buffer.begin(), buffer.end(), write.bytes))
    {
      all_patched = false;
      break;
    }
  }

  return all_patched ? DiscStatus::AlreadyPatched : DiscStatus::Clean;
}

bool ApplyToBootParameters(BootParameters* boot_params)
{
  if (!boot_params)
    return false;
  if (!std::holds_alternative<BootParameters::Disc>(boot_params->parameters))
    return false;

  const BootParameters::Disc& disc = std::get<BootParameters::Disc>(boot_params->parameters);
  if (!disc.volume)
    return false;

  const std::string game_id = disc.volume->GetGameID();
  if (!IsPbrGameID(game_id))
    return false;

  if (!IsSupportedGameID(game_id))
  {
    WARN_LOG_FMT(BOOT, "Pokemon Battle Revolution {} has no Wiimmfi payload; online play will "
                       "not work with this disc.",
                 game_id);
    return false;
  }

  // Don't stack a second copy on top of a Riivolution patch set that already includes ours.
  const auto already_attached =
      std::ranges::any_of(boot_params->riivolution_patches,
                          [](const DiscIO::Riivolution::Patch& p) { return p.m_id == PATCH_ID; });
  if (already_attached)
    return false;

  switch (ExamineDisc(*disc.volume))
  {
  case DiscStatus::AlreadyPatched:
    INFO_LOG_FMT(BOOT, "{} already carries the Wiimmfi payload; not patching again.", game_id);
    return false;
  case DiscStatus::Clean:
    INFO_LOG_FMT(BOOT, "Clean {} dump detected; applying the built-in Wiimmfi patch.", game_id);
    break;
  default:
    // Couldn't read main.dol (odd container, damaged FST, ...). Patch anyway: every replacement
    // is the new string NUL-padded to the length of the string it replaces, so applying it to an
    // image that already contains it writes back byte-identical data.
    WARN_LOG_FMT(BOOT, "Could not inspect {}'s main.dol; applying the Wiimmfi patch regardless.",
                 game_id);
    break;
  }

  std::vector<DiscIO::Riivolution::Patch> patches = GetPatches(game_id);
  if (patches.empty())
    return false;

  // Note this appends to riivolution_patches instead of going through AddRiivolutionPatches().
  // That helper additionally wraps the volume in a DirectoryBlobReader so that <file> patches can
  // rewrite the on-disc FST. We have no file patches -- only <memory> ones, which CBoot::BootUp()
  // applies straight from boot_params->riivolution_patches after the DOL is in RAM -- so the
  // wrapper would buy us nothing and cost a full FST rebuild plus on-the-fly Wii partition
  // re-encryption on every boot.
  boot_params->riivolution_patches.insert(boot_params->riivolution_patches.end(),
                                          std::make_move_iterator(patches.begin()),
                                          std::make_move_iterator(patches.end()));
  return true;
}
}  // namespace XDNetplay::PbrWiimmfi
