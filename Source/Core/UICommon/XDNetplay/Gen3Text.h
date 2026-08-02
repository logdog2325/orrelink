// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

// Gen 3 western (International) character set encode/decode.
//
// Ported 1:1 from the Android Kotlin port (Gen3Text.kt) of the empirically
// validated Python reference; all byte values were confirmed against real
// Emerald saves. Strings are fixed-width, 0xFF-terminated; a string that
// fills the whole field carries no terminator, exactly like the games.
//
// The public API uses UTF-8 std::strings (the charset contains non-ASCII
// glyphs such as the gender symbols and curly quotes); the codepoint helpers
// are exposed for callers that sanitize text before encoding.
namespace XDNetplay::Gen3Text
{
constexpr u8 TERMINATOR = 0xFF;

// True if the Unicode codepoint has a Gen 3 byte value in this charset.
bool CanEncode(char32_t c);

// Decode up to max_len bytes of Gen 3 text starting at buf. Unknown bytes
// decode as '?', mirroring the reference implementations.
std::string Decode(const u8* buf, size_t max_len);

// Encode UTF-8 text to exactly max_len bytes, 0xFF-terminated/padded. Text
// longer than max_len codepoints is truncated (as in the references).
// Returns std::nullopt (with *error set, if given) if a codepoint cannot be
// encoded.
std::optional<std::vector<u8>> Encode(const std::string& text, size_t max_len,
                                      std::string* error = nullptr);

// Make text Gen 3-encodable by force: straight ASCII quotes become the
// charset's curly forms, anything else unencodable is DROPPED, and the result
// is clamped to max_len codepoints (0 = no clamp). Use this for input that
// must degrade rather than fail (nicknames from a Showdown paste, a trainer
// name that arrived over the wire); use Encode's error path when a human is
// typing and deserves to be told which character is the problem.
std::string Sanitize(const std::string& text, size_t max_len = 0);

// Minimal UTF-8 <-> codepoint helpers shared with MonFactory's sanitizer.
std::u32string DecodeUtf8(const std::string& text);
std::string EncodeUtf8(const std::u32string& text);
}  // namespace XDNetplay::Gen3Text
