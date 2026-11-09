// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

// Pokemon XD battle recorder. Once per field it reads XD's battle state and notes what changed:
// moves, HP, faints, switches, status and stat stages. Each finished battle becomes
// GBA/Battle Records/battle_<stamp>_<n>.html (a Showdown-format replay) and .txt (a readable log).
// It only reads emulated memory, so it cannot affect netplay.
namespace XDBattleRecorder
{
struct TeamMember
{
  u16 species = 0;
  u8 level = 0;
  u16 max_hp = 0;
  std::string name;
};

// A 1v1 start's two parties, from the GBA saves it boots (side 0 is GBA 1). Each battle record
// lists them at the top, so the whole team is on record even if some never leave the bench.
void SetTeams(std::vector<TeamMember> side1, std::vector<TeamMember> side2);
void ClearTeams();

void OnField(Core::System& system);
// The game is stopping: a battle still open is written as far as it got.
void Finish();
}  // namespace XDBattleRecorder
