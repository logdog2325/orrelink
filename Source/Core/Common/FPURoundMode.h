// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Common::FPU
{
enum RoundMode : u32
{
  ROUND_NEAR = 0,
  ROUND_CHOP = 1,
  ROUND_UP = 2,
  ROUND_DOWN = 3
};

// software_output_flush: the running core flushes denormal OUTPUTS itself
// (interpreter cores on an ARM64 host without FEAT_AFP), so the host FPU must
// not flush at all -- hardware FZ there would also flush INPUTS, which the
// Gekko never does and x86-64 (FTZ only) does not either. Keeps the Cached
// Interpreter bit-identical between x86-64 and Apple M1-M3 class hosts
// (residual: the FMA tie-correction intermediates in NI_madd_msub are raw host
// results and can differ for denormal intermediates -- contrived for GC titles).
void SetSIMDMode(RoundMode rounding_mode, bool non_ieee_mode, bool software_output_flush = false);

/*
 * There are two different flavors of float to int conversion:
 * _mm_cvtps_epi32() and _mm_cvttps_epi32().
 *
 * The first rounds according to the MXCSR rounding bits.
 * The second one always uses round towards zero.
 */
void SaveSIMDState();
void LoadSIMDState();
void LoadDefaultSIMDState();
}  // namespace Common::FPU
