// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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
void OnField(Core::System& system);
// The game is stopping: a battle still open is written as far as it got.
void Finish();
}  // namespace XDBattleRecorder
