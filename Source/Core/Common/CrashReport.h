// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace Common::CrashReport
{
// Installs last-chance handlers (Windows: an unhandled-exception filter; POSIX:
// fatal-signal handlers on an alternate stack) that write a plain-text report
// into `directory` before the process dies, then hand the crash back to the OS
// so its own behaviour (dialog, core dump, crash log) is unchanged.
//
// The report names every return address as module+offset, which resolves
// offline against the build's PDB/.map (Windows), dSYM (macOS) or unstripped
// binary (Linux) -- the CI keeps those next to each release. Desktop only;
// Android already has tombstones. Safe to call once, early, after the user
// directory is known; the directory is created if missing.
void Install(const std::string& directory);
}  // namespace Common::CrashReport
