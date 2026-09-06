// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"

namespace Config
{
extern const Info<bool> SESSION_USE_FMA;
extern const Info<bool> SESSION_LOAD_IPL_DUMP;
extern const Info<bool> SESSION_GCI_FOLDER_CURRENT_GAME_ONLY;
extern const Info<bool> SESSION_CODE_SYNC_OVERRIDE;
extern const Info<bool> SESSION_SAVE_DATA_WRITABLE;
extern const Info<bool> SESSION_SHOULD_FAKE_ERROR_001;
// OrreLink v1.5.11: set only by the NetPlay config layer (NetPlayConfigLoader); read by
// HLE_XD::Install. Defaults mean "no hook" for solo play and same-architecture rooms.
extern const Info<bool> SESSION_XD_DETERMINISTIC_CLOCK;
extern const Info<u32> SESSION_XD_CLOCK_SALT;
extern const Info<u32> SESSION_XD_RNG_SEED;
}  // namespace Config
