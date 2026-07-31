// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"

// Human-readable, self-contained, BOUNDED diagnostic log for the XD GBA-vs-GBA
// detection path. One fresh file per emulation session, written from the CPU/
// emulation thread only, through a single mutex-guarded sink.
//
// Purely observational: nothing here feeds back into emulation or netplay, no
// value returned is consumed by any control path, and no call performs file I/O
// on the joybus hot path (transition-triggered + <=1/sec summary + a bounded
// boot-time command dump only).
//
// File: <User>/GBA/gba_detect_YYYYMMDD_HHMMSS.log (File::GetUserPath(D_GBAUSER_IDX));
// one file per session, the two most recent previous sessions are retained.
namespace GBADetectLog
{
// Absolute path of the CURRENT session's log file (empty before any session).
std::string FilePath();

// CSIDevice_GBAEmu ctor/dtor (CPU/host thread). Reference-counted: the first
// live GBA device of a session truncates the file, writes the banner as line 1
// and shows the path on the OSD; when the last device is torn down the session
// is closed. Guarantees a fresh, small file every boot.
void OnDeviceCreated();
void OnDeviceDestroyed();

// H1 -- one line per GBA at boot (CPU/host thread, inside Core::Start).
void LogBios(int channel, bool official, const std::string& path, u64 size, bool netplay,
             bool load_ok);

// Discrete detection transitions / role line. Flushed by default (transitions
// are rare); pass flush=false from the GBA event thread (I1 jpost/jclear) so
// that thread never waits on a write syscall.
void LogEvent(int channel, u64 tick, const char* event, const std::string& detail,
              bool flush = true);

// One joybus command ('>') or response ('<') line. NOT flushed per call
// (bounded boot burst; a later event/summary flushes it). bytes[0..4] must be
// valid. `sent` is meaningful for '>' lines. `jsa` (responses only) is JOYSTAT
// as sampled on the event thread right after the command executed.
void LogJoybus(int channel, u64 tick, char dir, const u8* bytes, int len, bool sent, u8 jsa = 0);

// Once-per-second telemetry mirror (caller throttles to <=1/sec). Fields mirror
// the on-screen readout (rom/gba/loc/lk/win/rst/prb/est/lck/cmd) plus the
// ungated data-command counters rd/wr (I4 ground truth).
void LogSummary(int channel, u64 tick, bool rom, bool gba, bool loc, bool lk, u32 win, u32 rst,
                u32 prb, bool est, bool lck, u8 cmd, u32 rd, u32 wr);

// Shared last-GC-pad-press tick (A/B/Start rising edges), written by the GC
// controller tap and read by the GBA devices for the per-roll state lines.
// Diagnostic only; never feeds emulation.
void NotePadPress(u64 tick);
u64 LastPadPress();
}  // namespace GBADetectLog
