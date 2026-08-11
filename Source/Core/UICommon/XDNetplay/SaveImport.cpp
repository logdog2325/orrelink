// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/SaveImport.h"

#include <array>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif

namespace XDNetplay::SaveImport
{
namespace
{
// The full 128 KiB flash image every Gen 3 cartridge writes. EmeraldSave only
// needs the two game-save blocks (114688 bytes), but an import always stores
// the whole flash so the Hall of Fame / e-Reader sectors and any emulator
// footer ride along verbatim.
constexpr size_t FULL_FLASH_SIZE = 131072;
// A 64 KiB dump holds a single bank: block 0 plus the start of block 1.
constexpr size_t SINGLE_BANK_SIZE = 65536;
// Largest emulator footer accepted on a single-bank dump. mGBA's RTC tail is
// 16 bytes; 256 leaves headroom for other emulators without letting a
// truncated dual-bank file masquerade as bank-plus-footer.
constexpr size_t MAX_FOOTER_SIZE = 256;

#ifdef HAS_LIBMGBA
// TeamInjector's session bookkeeping files. MUST stay in step with the
// suffix constants in TeamInjector.cpp: their presence next to a socket save
// means a room's injection round trip owns that file right now.
constexpr const char* HOST_STASH_SUFFIX = ".hostteam";
constexpr const char* GUEST_MARK_SUFFIX = ".guestteam";
// Same ROM resolution as TeamInjector and the team editors: the socket's own
// ROM, falling back to the other socket's (a launcher-made setup points both
// at one dump). Empty when the setup is not complete enough to have one.
std::string ResolveRomForDevice(int device)
{
  std::string rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device]);
  if (rom.empty() || !File::Exists(rom))
    rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device == 1 ? 2 : 1]);
  if (rom.empty() || !File::Exists(rom))
    return {};
  return rom;
}

std::string SavePathForDevice(int device)
{
  const std::string rom = ResolveRomForDevice(device);
  if (rom.empty())
    return {};
  return HW::GBA::Core::GetSavePath(rom, device);
}

const Config::Info<std::string>& ImportedSaveKey(int device)
{
  return device == 1 ? Config::MAIN_XD_IMPORTED_SAVE_2 : Config::MAIN_XD_IMPORTED_SAVE_3;
}

std::string BaseName(const std::string& path)
{
  return path.substr(path.find_last_of("/\\") + 1);
}

bool ReadFileBytes(const std::string& path, std::vector<u8>* out)
{
  File::IOFile file(path, "rb");
  if (!file)
    return false;
  out->resize(file.GetSize());
  return out->empty() || file.ReadBytes(out->data(), out->size());
}

// The tmp/readback/rename discipline of VerifiedWriteSaveFile (Gen3Save.cpp),
// minus its Emerald-only re-parse -- an already-validated RS/FRLG image must
// pass. The destination is never touched until the tmp has been read back
// byte-identical, and an existing destination is kept as one-level .bak undo.
bool WriteFileVerified(const std::string& path, const std::vector<u8>& bytes, std::string* error)
{
  File::CreateFullPath(path);
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

// Refusals shared by import and restore: never touch a socket save while the
// emulated GBAs could rewrite it from memory, and never touch the guest slot
// while a room's injection bookkeeping owns it (the purge would later copy
// the stash straight over whatever was written).
bool RefuseIfSaveIsLive(int device, const std::string& save_path, std::string* error)
{
  if (!Core::IsUninitialized(Core::System::GetInstance()))
  {
    if (error)
    {
      *error = "A game is running — the emulated GBA owns its save file and would overwrite the "
               "change. Stop emulation first.";
    }
    return true;
  }
  if (device == 2 &&
      (NetPlay::IsNetPlayRunning() || File::Exists(save_path + GUEST_MARK_SUFFIX) ||
       File::Exists(save_path + HOST_STASH_SUFFIX)))
  {
    if (error)
    {
      *error = "You are hosting a room right now — the guest slot holds your opponent's team. "
               "Close the room, then import.";
    }
    return true;
  }
  return false;
}

// Requirement 5, appended to every success status: an import must never leave
// a joiner believing their file reaches a host's room.
constexpr const char* HOST_SAVES_NOTE =
    "Netplay rooms always use the HOST's saves. An imported save applies when you play solo or "
    "host; when you join someone's room, use Submit Team instead.";

constexpr const char* RS_REVISION_NOTE =
    "Note: Ruby/Sapphire revisions (1.0/1.1/1.2) also differ — use a save from your exact ROM "
    "revision.";
#endif  // HAS_LIBMGBA
}  // namespace

std::optional<SaveValidation> ValidateGen3SaveImage(std::vector<u8>* bytes, std::string* error)
{
  const auto fail = [error](std::string message) {
    if (error)
      *error = std::move(message);
    return std::nullopt;
  };

  SaveValidation result;
  result.original_size = bytes->size();

  if (bytes->size() == SINGLE_BANK_SIZE)
  {
    // 0xFF is flash erase state: the padded second bank scans as invalid and
    // block 0 becomes the active block, exactly like a once-saved cartridge.
    bytes->resize(FULL_FLASH_SIZE, 0xFF);
    result.padded_from_64k = true;
  }
  else if (bytes->size() > SINGLE_BANK_SIZE && bytes->size() < FULL_FLASH_SIZE &&
           bytes->size() - SINGLE_BANK_SIZE <= MAX_FOOTER_SIZE)
  {
    // A 64 KiB dump that carries an emulator footer (mGBA's RTC tail is 16
    // bytes): pad the flash bank to full size and re-attach the footer after
    // it, where the rest of the pipeline expects footers to live.
    std::vector<u8> footer(bytes->begin() + SINGLE_BANK_SIZE, bytes->end());
    bytes->resize(SINGLE_BANK_SIZE);
    bytes->resize(FULL_FLASH_SIZE, 0xFF);
    bytes->insert(bytes->end(), footer.begin(), footer.end());
    result.padded_from_64k = true;
  }
  else if (bytes->size() < FULL_FLASH_SIZE)
  {
    return fail(fmt::format(
        "That file is not a Gen 3 save: expected 131,072 bytes of save data (128 KiB, optionally "
        "with an emulator footer), got {} bytes.",
        result.original_size));
  }

  std::string parse_error;
  const auto save = EmeraldSave::Create(*bytes, &parse_error);
  if (!save)
  {
    return fail("No valid save block found — the file may be corrupt, or from a different "
                "game/emulator format.");
  }

  result.game = EmeraldSave::DetectGame(*save);
  const auto bad = save->VerifyAllChecksums(result.game);
  if (!bad.empty())
  {
    return fail(fmt::format(
        "{} section checksum(s) are invalid — the save looks corrupt, so it was not imported.",
        bad.size()));
  }
  return result;
}

std::optional<Gen3Game> DetectRomGame(const std::string& rom_path, std::string* error)
{
  const auto fail = [error](std::string message) {
    if (error)
      *error = std::move(message);
    return std::nullopt;
  };

  std::array<char, 4> code{};
  {
    File::IOFile file(rom_path, "rb");
    if (!file || file.GetSize() < 0xC0 || !file.Seek(0xAC, File::SeekOrigin::Begin) ||
        !file.ReadBytes(code.data(), code.size()))
    {
      return fail("could not read the ROM header of " + rom_path);
    }
  }

  const std::string_view id(code.data(), code.size());
  const std::string_view family = id.substr(0, 3);  // fourth letter = language
  if (family == "AXV" || family == "AXP")
    return Gen3Game::RubySapphire;
  if (family == "BPR" || family == "BPG")
    return Gen3Game::FireRedLeafGreen;
  if (family == "BPE")
    return Gen3Game::Emerald;

  std::string printable;
  for (const char c : id)
    printable.push_back(c >= 0x20 && c < 0x7F ? c : '?');
  return fail(fmt::format(
      "this ROM is not a Gen 3 Pokemon game (game code \"{}\"), so there is nothing to match the "
      "save against",
      printable));
}

bool ImportUserSave(const std::string& source_path, int device, std::string* status,
                    std::string* error)
{
  const auto fail = [error](std::string message) {
    if (error)
      *error = std::move(message);
    return false;
  };

#ifndef HAS_LIBMGBA
  return fail("this build has no GBA support");
#else
  if (device != 1 && device != 2)
    return fail(fmt::format("no such GBA socket ({})", device));

  const std::string rom = ResolveRomForDevice(device);
  if (rom.empty())
    return fail("no GBA ROM configured for that port — run the launcher checklist first");
  const std::string save_path = HW::GBA::Core::GetSavePath(rom, device);

  if (RefuseIfSaveIsLive(device, save_path, error))
    return false;

  // The chosen file is read once, fully, and never opened for writing.
  std::vector<u8> bytes;
  if (!ReadFileBytes(source_path, &bytes))
    return fail("could not read " + source_path);

  const auto validation = ValidateGen3SaveImage(&bytes, error);
  if (!validation)
    return false;

  const auto rom_game = DetectRomGame(rom, error);
  if (!rom_game)
    return false;
  if (validation->game != *rom_game)
  {
    std::string message = fmt::format(
        "That save is from {}, but this GBA port runs {}. The game itself would refuse a "
        "mismatched save, so it was not imported.",
        GameDisplayName(validation->game), GameDisplayName(*rom_game));
    if (validation->game == Gen3Game::RubySapphire)
      message += std::string(" ") + RS_REVISION_NOTE;
    return fail(message);
  }

  // First backup wins, forever: a re-import must never overwrite the record
  // of what the socket held before the FIRST import.
  const std::string backup_path = save_path + PREIMPORT_SUFFIX;
  const bool backup_existed = File::Exists(backup_path);
  const bool had_previous = File::Exists(save_path);
  if (had_previous && !backup_existed && !File::CopyRegularFile(save_path, backup_path))
    return fail("could not back up the current save to " + backup_path + " — nothing was changed");

  if (!WriteFileVerified(save_path, bytes, error))
    return false;

  // Display-only record of what was imported; HasImportBackup stays the
  // authoritative signal.
  Config::SetBase(ImportedSaveKey(device), BaseName(source_path));
  Config::Save();

  if (status)
  {
    const auto parsed = EmeraldSave::Create(bytes);
    // The trainer-name field lives at the same section-0 offset in all five
    // cartridges, so it is worth showing whatever the game is.
    const std::string trainer = parsed ? parsed->GetTrainerName() : "?";
    *status = fmt::format("Imported {} ({}, trainer {}).", BaseName(source_path),
                          GameDisplayName(validation->game), trainer);
    if (validation->padded_from_64k)
      *status += " The 64 KiB file was padded to the full 128 KiB flash size.";
    if (backup_existed)
    {
      *status += fmt::format(" Your original save from before the first import is kept as {}.",
                             BaseName(backup_path));
    }
    else if (had_previous)
    {
      *status += fmt::format(" Your previous save is kept as {}.", BaseName(backup_path));
    }
    if (validation->game == Gen3Game::RubySapphire)
      *status += std::string(" ") + RS_REVISION_NOTE;
    *status += std::string("\n\n") + HOST_SAVES_NOTE;
  }
  return true;
#endif
}

bool RestoreDefaultSave(int device, std::string* error)
{
  const auto fail = [error](std::string message) {
    if (error)
      *error = std::move(message);
    return false;
  };

#ifndef HAS_LIBMGBA
  return fail("this build has no GBA support");
#else
  if (device != 1 && device != 2)
    return fail(fmt::format("no such GBA socket ({})", device));

  const std::string save_path = SavePathForDevice(device);
  if (save_path.empty())
    return fail("no GBA ROM configured for that port — run the launcher checklist first");

  if (RefuseIfSaveIsLive(device, save_path, error))
    return false;

  const std::string template_path =
      File::GetSysDirectory() +
      (device == 1 ? "XDNetplay/EMERALD-2.sav" : "XDNetplay/EMERALD-3.sav");
  std::vector<u8> bytes;
  if (!ReadFileBytes(template_path, &bytes))
    return fail("bundled template missing: " + template_path);

  // The replaced import survives as .bak (WriteFileVerified's one-level
  // undo); .preimport is deliberately left alone -- it belongs to the
  // pre-import world and nothing may delete it.
  if (!WriteFileVerified(save_path, bytes, error))
    return false;

  Config::SetBase(ImportedSaveKey(device), std::string{});
  Config::Save();
  return true;
#endif
}

bool HasImportBackup(int device)
{
#ifndef HAS_LIBMGBA
  return false;
#else
  const std::string save_path = SavePathForDevice(device);
  return !save_path.empty() && File::Exists(save_path + PREIMPORT_SUFFIX);
#endif
}
}  // namespace XDNetplay::SaveImport
