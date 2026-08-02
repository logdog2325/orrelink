// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/Gen3Text.h"

#include <algorithm>
#include <map>

namespace XDNetplay::Gen3Text
{
namespace
{
struct Maps
{
  std::map<u8, char32_t> byte_to_char;
  std::map<char32_t, u8> char_to_byte;

  void Map(u8 b, char32_t c)
  {
    byte_to_char[b] = c;
    char_to_byte[c] = b;
  }

  Maps()
  {
    Map(0x00, U' ');
    // Digits 0-9: 0xA1..0xAA
    for (int i = 0; i <= 9; i++)
      Map(static_cast<u8>(0xA1 + i), U'0' + i);
    Map(0xAB, U'!');
    Map(0xAC, U'?');
    Map(0xAD, U'.');
    Map(0xAE, U'-');
    Map(0xAF, U'・');  // KATAKANA MIDDLE DOT
    Map(0xB0, U'…');  // HORIZONTAL ELLIPSIS
    Map(0xB1, U'“');  // LEFT DOUBLE QUOTATION MARK
    Map(0xB2, U'”');  // RIGHT DOUBLE QUOTATION MARK
    Map(0xB3, U'‘');  // LEFT SINGLE QUOTATION MARK
    Map(0xB4, U'’');  // RIGHT SINGLE QUOTATION MARK
    Map(0xB5, U'♂');  // MALE SIGN
    Map(0xB6, U'♀');  // FEMALE SIGN
    Map(0xB7, U'$');
    Map(0xB8, U',');
    Map(0xB9, U'×');  // MULTIPLICATION SIGN
    Map(0xBA, U'/');
    // A-Z: 0xBB..0xD4, a-z: 0xD5..0xEE
    for (int i = 0; i <= 25; i++)
    {
      Map(static_cast<u8>(0xBB + i), U'A' + i);
      Map(static_cast<u8>(0xD5 + i), U'a' + i);
    }
  }
};

const Maps& GetMaps()
{
  static const Maps maps;
  return maps;
}
}  // namespace

std::u32string DecodeUtf8(const std::string& text)
{
  std::u32string out;
  size_t i = 0;
  const size_t n = text.size();
  while (i < n)
  {
    const u8 b0 = static_cast<u8>(text[i]);
    char32_t cp = U'?';
    size_t len = 1;
    if (b0 < 0x80)
    {
      cp = b0;
    }
    else if ((b0 & 0xE0) == 0xC0 && i + 1 < n)
    {
      cp = ((b0 & 0x1F) << 6) | (static_cast<u8>(text[i + 1]) & 0x3F);
      len = 2;
    }
    else if ((b0 & 0xF0) == 0xE0 && i + 2 < n)
    {
      cp = ((b0 & 0x0F) << 12) | ((static_cast<u8>(text[i + 1]) & 0x3F) << 6) |
           (static_cast<u8>(text[i + 2]) & 0x3F);
      len = 3;
    }
    else if ((b0 & 0xF8) == 0xF0 && i + 3 < n)
    {
      cp = ((b0 & 0x07) << 18) | ((static_cast<u8>(text[i + 1]) & 0x3F) << 12) |
           ((static_cast<u8>(text[i + 2]) & 0x3F) << 6) | (static_cast<u8>(text[i + 3]) & 0x3F);
      len = 4;
    }
    out.push_back(cp);
    i += len;
  }
  return out;
}

std::string EncodeUtf8(const std::u32string& text)
{
  std::string out;
  for (const char32_t cp : text)
  {
    if (cp < 0x80)
    {
      out.push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800)
    {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

bool CanEncode(char32_t c)
{
  return GetMaps().char_to_byte.count(c) != 0;
}

std::string Decode(const u8* buf, size_t max_len)
{
  const auto& maps = GetMaps();
  std::u32string out;
  for (size_t i = 0; i < max_len; i++)
  {
    const u8 b = buf[i];
    if (b == TERMINATOR)
      break;
    const auto it = maps.byte_to_char.find(b);
    out.push_back(it != maps.byte_to_char.end() ? it->second : U'?');
  }
  return EncodeUtf8(out);
}

std::optional<std::vector<u8>> Encode(const std::string& text, size_t max_len, std::string* error)
{
  const auto& maps = GetMaps();
  std::vector<u8> out(max_len, TERMINATOR);
  const std::u32string codepoints = DecodeUtf8(text);
  const size_t n = std::min(codepoints.size(), max_len);
  for (size_t i = 0; i < n; i++)
  {
    const auto it = maps.char_to_byte.find(codepoints[i]);
    if (it == maps.char_to_byte.end())
    {
      if (error)
        *error = "character '" + EncodeUtf8(std::u32string(1, codepoints[i])) +
                 "' not encodable in Gen 3 charset";
      return std::nullopt;
    }
    out[i] = it->second;
  }
  return out;
}

std::string Sanitize(const std::string& text, size_t max_len)
{
  std::u32string out;
  for (const char32_t c : DecodeUtf8(text))
  {
    if (max_len != 0 && out.size() == max_len)
      break;
    char32_t mapped = c;
    if (c == U'\'')
      mapped = U'’';
    else if (c == U'"')
      mapped = U'”';
    if (CanEncode(mapped))
      out.push_back(mapped);
  }
  return EncodeUtf8(out);
}
}  // namespace XDNetplay::Gen3Text
