// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace XDNetplay
{
// The XD Netplay release this build belongs to.
//
// Common::GetScmDescStr() describes the upstream Dolphin commit CI happened to build, not this
// fork's release, so it cannot answer "which XD Netplay am I?" -- this constant is the only thing
// that can. It MUST be bumped in the same patch that a release is tagged from: ship a tag ahead of
// this string and every user of that release is told they are out of date forever.
constexpr char VERSION[] = "1.4.2";
}  // namespace XDNetplay
