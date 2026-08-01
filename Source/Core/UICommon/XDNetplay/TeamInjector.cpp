// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/TeamInjector.h"

#include <optional>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"

#include "Core/Config/MainSettings.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif

#include "UICommon/XDNetplay/Gen3Data.h"
#include "UICommon/XDNetplay/Gen3Mon.h"
#include "UICommon/XDNetplay/Gen3Save.h"
#include "UICommon/XDNetplay/MonFactory.h"
#include "UICommon/XDNetplay/ShowdownParser.h"

namespace XDNetplay
{
#ifdef HAS_LIBMGBA
namespace
{
bool ReadFileBytes(const std::string& path, std::vector<u8>* out)
{
  File::IOFile file(path, "rb");
  if (!file)
    return false;
  out->resize(file.GetSize());
  return out->empty() || file.ReadBytes(out->data(), out->size());
}
}  // namespace
#endif

bool InjectGuestTeam(const std::string& showdown_text, int device, std::string* status)
{
  const auto fail = [status](std::string message) {
    if (status)
      *status = std::move(message);
    return false;
  };

#ifndef HAS_LIBMGBA
  return fail("this build has no GBA support");
#else
  std::string error;
  const auto data = Gen3Data::LoadBundled(&error);
  if (!data)
    return fail(fmt::format("game data unavailable ({})", error));

  // Same ROM resolution the team editor uses: the socket's own ROM, falling
  // back to the other socket's (a launcher-made setup points both at one dump).
  std::string rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device]);
  if (rom.empty() || !File::Exists(rom))
    rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device == 1 ? 2 : 1]);
  if (rom.empty() || !File::Exists(rom))
    return fail("no Emerald ROM configured on the host");

  // GetSavePath is exactly what SyncSaveData reads at start, so writing here
  // is guaranteed to be the file the guest receives.
  const std::string save_path = HW::GBA::Core::GetSavePath(rom, device);

  std::vector<u8> bytes;
  if (!(File::Exists(save_path) && ReadFileBytes(save_path, &bytes)))
  {
    const std::string template_path =
        File::GetSysDirectory() +
        (device == 1 ? "XDNetplay/EMERALD-2.sav" : "XDNetplay/EMERALD-3.sav");
    if (!ReadFileBytes(template_path, &bytes))
      return fail("host has no save for that socket and no bundled template");
  }

  auto save = EmeraldSave::Create(std::move(bytes), &error);
  if (!save)
    return fail(fmt::format("host save unreadable ({})", error));

  const std::vector<ShowdownSet> sets = ShowdownParser::ParseTeam(showdown_text);
  if (sets.empty())
    return fail("nothing recognizable in that paste");

  // Any party size is accepted: players run whatever format they like, and the
  // game itself enforces its own rules at battle setup.
  std::vector<Gen3Mon> built;
  size_t skipped = 0;
  for (const ShowdownSet& set : sets)
  {
    if (built.size() == EmeraldSave::PARTY_MAX)
    {
      ++skipped;
      continue;
    }
    std::string mon_error;
    auto mon = MonFactory::Build(set, *data, save->GetTrainerName(), save->GetTrainerId(),
                                 &mon_error);
    if (mon)
      built.push_back(std::move(*mon));
    else
      ++skipped;
  }
  if (built.empty())
    return fail("no usable Pokemon in that team");

  const size_t count = built.size();
  if (!save->WriteParty(built))
    return fail("party too large");
  if (!VerifiedWriteSaveFile(save_path, *save, &error))
    return fail(fmt::format("save NOT written ({})", error));

  if (status)
  {
    *status = skipped == 0 ?
                  fmt::format("{} Pokemon written to the guest save", count) :
                  fmt::format("{} Pokemon written to the guest save ({} entries skipped)", count,
                              skipped);
  }
  return true;
#endif
}
}  // namespace XDNetplay
