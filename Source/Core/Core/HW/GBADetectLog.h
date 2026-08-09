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
// `suppress_repeats` (pass true once the socket is battle-locked): an exact
// repeat of the previous line on the same (channel, direction) is counted
// instead of written -- measured 96.8% of battle-phase joy bytes were such
// repeats -- and the run length is emitted as rep=N on the next changed line.
void LogJoybus(int channel, u64 tick, char dir, const u8* bytes, int len, bool sent, u8 jsa = 0,
               bool suppress_repeats = false);

// Once-per-second telemetry mirror (caller throttles to <=1/sec). Fields mirror
// the on-screen readout (rom/gba/loc/lk/win/rst/prb/est/lck/cmd) plus the
// ungated data-command counters rd/wr (I4 ground truth).
void LogSummary(int channel, u64 tick, bool rom, bool gba, bool loc, bool lk, u32 win, u32 rst,
                u32 prb, bool est, bool lck, u8 cmd, u32 rd, u32 wr);

// One line of pre-boot context (Battle Style selections, forced flags...).
// These events happen BEFORE emulation starts, when no session file exists
// yet: the line is queued (bounded) and flushed right under the banner of the
// NEXT session's log, so the log that describes a battle also says what was
// configured into it. With a session already open it is written immediately.
void NoteBoot(const std::string& line);

// Append one line AFTER the session's log has already been closed.
//
// OnDeviceDestroyed() closes the file as soon as the last GBA device is torn
// down, but the end-of-session save cleanup deliberately runs later than that
// (it has to: the mGBA core rewrites the .sav from its in-memory copy during
// that very teardown). This reopens the most recent session's file in append
// mode, writes one line, and closes it again -- so the cleanup is visible in
// the same log a user already knows how to hand over. No-op before any
// session has run. Called from the emulation thread at shutdown or from the
// UI thread when a room closes with emulation already down; bounded to a
// handful of lines per session.
void LogPostSession(const std::string& detail);

// Shared last-GC-pad-press tick (A/B/Start rising edges), written by the GC
// controller tap and read by the GBA devices for the per-roll state lines.
// Diagnostic only; never feeds emulation.
void NotePadPress(u64 tick);
u64 LastPadPress();
}  // namespace GBADetectLog
