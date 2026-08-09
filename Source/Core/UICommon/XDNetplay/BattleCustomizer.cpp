// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/XDNetplay/BattleCustomizer.h"

#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"

#ifdef HAS_LIBMGBA
#include "Core/HW/GBADetectLog.h"
#endif

namespace XDNetplay::BattleCustomizer
{
namespace
{
// The one code this module owns in User/GameSettings/GXXE01.ini. The leading
// '$' is how ActionReplay::LoadCodes and ReadEnabledOrDisabled spell a code
// name inside the three sections.
constexpr std::string_view STYLE_CODE_LINE = "$OrreLink Battle Style";
// The Sys-bundled code (Data/Sys/GameSettings/GXXE01.ini) that must be locally
// disabled while cheats are forced on for a cosmetics-only session.
constexpr std::string_view OU_FIXES_LINE = "$XD OU Fixes";
// The feature is pinned to Pokemon XD (USA). Every address below is NTSC-U;
// writing only this file is what keeps a future PAL/JP experiment from
// inheriting wrong patches.
constexpr std::string_view GAME_INI_NAME = "GXXE01.ini";

// ---------------------------------------------------------------------------
// AR line constants (all NTSC-U GXXE01; MEM1 addresses in the comments)
// ---------------------------------------------------------------------------

// Trainer model modifier. Source: Ralf's community-circulated XD trainer
// modifier codes, gc-forever.com thread t=2150 page 3 (post p=55782; AR twin
// thread t=2069) -- reconstructed from search-engine snippets, consistent
// across three retrievals; eyeball the thread once before shipping. The model
// id indexes the 0x44-entry trainer_pkx_data table at 0x8040CE88.
//
// Player side: two NOPs take the game's own model selection out, then three
// "li r3, <id>" sites pin the id at every fetch.
constexpr u32 MODEL_P1_LINES[] = {
    0x041FFEF4,  // 32-bit write: nop @ 0x801FFEF4
    0x041FFEFC,  // 32-bit write: nop @ 0x801FFEFC
    0x041FFF00,  // 32-bit write: li r3, id @ 0x801FFF00
    0x041FD4FC,  // 32-bit write: li r3, id @ 0x801FD4FC
    0x042262CC,  // 32-bit write: li r3, id @ 0x802262CC
};
// Opponent side surfaced as a single li site in the thread. If testing shows
// the opponent model flickering or reverting mid-battle, the thread may hold
// adjacent opponent-side NOPs the snippets omitted -- check near 0x801FFF84.
constexpr u32 MODEL_P2_LINE = 0x041FFF84;  // 32-bit write: li r3, id @ 0x801FFF84
constexpr u32 PPC_NOP = 0x60000000;        // nop
constexpr u32 PPC_LI_R3 = 0x38600000;      // li r3, 0 -- the id occupies the low byte

// VS-mode battle music: pin the static staBGM_tunaide table in start.dol data
// (three identical consecutive slots). VERIFIED against a clean GXXE01
// main.dol: the DOL data section loading at 0x802EA020 holds, at file offset
// 0x2E7F0C = 0x802EAF0C, the u32 BE sequence 000004F3 000004F3 000004F3 --
// three identical stride-4 slots, vanilla value 0x4F3 = 1267 =
// stm_bgm_tool_minnade.fsys, the multiplayer-mode BGM family. That confirms
// the address, the u32 encoding below, and that the third write ends before
// the neighbouring data at +0x0C. (The old prediction of 0x5DB for the
// vanilla value was wrong; irrelevant, since "game default" never writes it.)
// Story battles carry their own BGM id in their common.rel record and never
// read this table, so the pick cannot bleed into a story save.
constexpr u32 MUSIC_LINES[] = {
    0x042EAF0C,  // 32-bit write: staBGM_tunaide[0] @ 0x802EAF0C
    0x042EAF10,  // 32-bit write: staBGM_tunaide[1] @ 0x802EAF10
    0x042EAF14,  // 32-bit write: staBGM_tunaide[2] @ 0x802EAF14
};

// Battle location: the four GBA-link battle records in the resident common_rel
// image (base 0x80B18DC0; record i = 0x80B1CDE0 + i*0x3C, battlefield index at
// +0x06, u16). Records 5-8 are only consumed by the type-11 GBA-link flow --
// story mode never reads them -- and the AR engine re-applies these every
// frame, which kills any ordering race against the game's own one-shot writes.
// All four are written so the venue holds for every battle style. Derived in
// this repo (scratchpad/dump_battles.py); engine path GetBattleRecord
// (0x801F19CC) -> lhz+6 -> GetBattleField (0x801F5E64).
// The record's own BGM field lives at +0x10 -- deliberately untouched here, so
// venue and music choices stay independent.
constexpr u32 VENUE_LINES[] = {
    0x02B1CF12,  // 16-bit write: link_battle record 5 (+0x06) -- single 6v6, the OU record
    0x02B1CF4E,  // 16-bit write: link_battle record 6 (+0x06) -- double, 2 mons
    0x02B1CF8A,  // 16-bit write: link_battle record 7 (+0x06) -- double 6v6
    0x02B1CFC6,  // 16-bit write: link_battle record 8 (+0x06) -- tag 2v2
};

// ---------------------------------------------------------------------------
// ID tables (see the tier note in BattleCustomizer.h)
// ---------------------------------------------------------------------------

// Trainer models: every valid id in 0x01..0x43 except 0x0A. 0x00 ("none") and
// 0x0A ("noTrainer") have no model file (expect invisible trainer or hang) and
// ids >= 0x44 read past the model table -- all excluded by omission.
// tested-safe = vanilla battling trainer, covered by Ralf's field-used
// modifier codes; experimental = valid entry but a non-battling NPC (or
// abnormal scale) in vanilla, battle animation set unverified.
constexpr StyleOption MODELS[] = {
    {0x01, "Michael (variant 1)", Tier::TestedSafe},
    {0x02, "Michael (variant 2, Snag Machine)", Tier::TestedSafe},
    {0x03, "Michael (variant 2)", Tier::TestedSafe},
    {0x04, "May (Emerald)", Tier::TestedSafe},
    {0x05, "Brendan (Emerald)", Tier::TestedSafe},
    {0x06, "Leaf (FireRed/LeafGreen)", Tier::TestedSafe},
    {0x07, "Red (FireRed/LeafGreen)", Tier::TestedSafe},
    {0x08, "May (Ruby/Sapphire)", Tier::TestedSafe},
    {0x09, "Brendan (Ruby/Sapphire)", Tier::TestedSafe},
    {0x0B, "Cipher Peon (female)", Tier::TestedSafe},
    {0x0C, "Cipher Peon (male A)", Tier::TestedSafe},
    {0x0D, "Cipher Peon (male B)", Tier::TestedSafe},
    {0x0E, "Cipher Peon (male C)", Tier::TestedSafe},
    {0x0F, "Resix (Hexagon Bros, red)", Tier::TestedSafe},
    {0x10, "Blusix (Hexagon Bros, blue)", Tier::TestedSafe},
    {0x11, "Browsix (Hexagon Bros, brown)", Tier::TestedSafe},
    {0x12, "Yellosix (Hexagon Bros, yellow)", Tier::TestedSafe},
    {0x13, "Purpsix (Hexagon Bros, purple)", Tier::TestedSafe},
    {0x14, "Greesix (Hexagon Bros, green)", Tier::TestedSafe},
    {0x15, "Cipher Admin Lovrina", Tier::TestedSafe},
    {0x16, "Sailor", Tier::TestedSafe},
    {0x17, "Thug Zook", Tier::TestedSafe},
    {0x18, "Cipher Admin Ardos", Tier::TestedSafe},
    {0x19, "Matron", Tier::TestedSafe},
    {0x1A, "Grand Master Greevil", Tier::TestedSafe},
    {0x1C, "Cipher Admin Eldes", Tier::TestedSafe},
    {0x1D, "Cipher Admin Gorigan", Tier::TestedSafe},
    {0x1E, "Snagem Head Gonzap", Tier::TestedSafe},
    {0x1F, "Super Trainer (female A)", Tier::TestedSafe},
    {0x20, "Super Trainer (female B)", Tier::TestedSafe},
    {0x21, "Vander", Tier::TestedSafe},
    {0x22, "Super Trainer (male B)", Tier::TestedSafe},
    {0x23, "Super Trainer (male C)", Tier::TestedSafe},
    {0x24, "Hunter", Tier::TestedSafe},
    {0x25, "Beauty", Tier::TestedSafe},
    {0x26, "Casual Dude", Tier::TestedSafe},
    {0x27, "Fun Old Man", Tier::TestedSafe},
    {0x28, "Curmudgeon", Tier::TestedSafe},
    {0x2B, "Miror B.", Tier::TestedSafe},
    {0x2C, "Bodybuilder (female)", Tier::TestedSafe},
    {0x2D, "Bodybuilder (male)", Tier::TestedSafe},
    {0x2E, "Mt. Battle Master Battlus", Tier::TestedSafe},
    {0x2F, "Casual Guy", Tier::TestedSafe},
    {0x30, "Researcher", Tier::TestedSafe},
    {0x31, "Rider", Tier::TestedSafe},
    {0x32, "Navigator", Tier::TestedSafe},
    {0x33, "Pre Gym Leader Justy", Tier::TestedSafe},
    {0x34, "Team Snagem Grunt (A)", Tier::TestedSafe},
    {0x35, "Team Snagem Grunt (B)", Tier::TestedSafe},
    {0x36, "Chobin", Tier::TestedSafe},
    {0x37, "Chaser (female A)", Tier::TestedSafe},
    {0x38, "Chaser (female B)", Tier::TestedSafe},
    {0x39, "Chaser (male)", Tier::TestedSafe},
    {0x3A, "Cail", Tier::TestedSafe},
    {0x3B, "Cooltrainer (female)", Tier::TestedSafe},
    {0x3C, "Cooltrainer (male)", Tier::TestedSafe},
    {0x3D, "Cipher Admin Snattle", Tier::TestedSafe},
    {0x3E, "Willie", Tier::TestedSafe},
    {0x3F, "Worker", Tier::TestedSafe},
    {0x42, "Michael (variant 3, Snag Machine)", Tier::TestedSafe},
    {0x43, "Michael (variant 3)", Tier::TestedSafe},
    // Non-battling NPCs in vanilla XD (battle animation set unverified), plus
    // the giant-mech-scale Robo Groudon and the robed Greevil variant.
    {0x1B, "Newscaster", Tier::Experimental},
    {0x29, "Eagun", Tier::Experimental},
    {0x2A, "Robo Groudon (Chobin)", Tier::Experimental},
    {0x40, "Professor Krane", Tier::Experimental},
    {0x41, "Grand Master Greevil (robed)", Tier::Experimental},
};

// Battle music. tested-safe = looping battle-context streams the game itself
// plays in battles; experimental = looping non-battle BGM, should loop fine in
// battle but unverified in the VS context. Non-looping jingles/stingers, cry
// SFX and archive containers are excluded by omission -- a non-looping pick
// would leave the battle permanently silent. 1499 (stm_bgm_tool_battle, the
// predicted stock VS theme) is deliberately NOT listed: it is what "Game
// default" already plays, and the vanilla value must never be written.
// Display names are best-effort stem translations; finalize EVERY label with a
// listening pass through the selector before release.
constexpr StyleOption MUSICS[] = {
    {1313, "XD Battle 1 (wild battle)", Tier::TestedSafe},
    {1314, "XD Battle 1b (wild variant)", Tier::TestedSafe},
    {1315, "XD Battle 6 (trainer)", Tier::TestedSafe},
    {1316, "XD Battle 8 (Cipher)", Tier::TestedSafe},
    {1413, "XD Battle 0", Tier::TestedSafe},
    {1127, "XD Battle 01", Tier::TestedSafe},
    {1128, "XD Battle 02", Tier::TestedSafe},
    {1407, "XD Battle 03", Tier::TestedSafe},
    {1318, "Colosseum Round 1", Tier::TestedSafe},
    {1319, "Colosseum Round 2", Tier::TestedSafe},
    {1320, "Colosseum Round 3", Tier::TestedSafe},
    {1321, "Colosseum Round 4", Tier::TestedSafe},
    {1240, "Orre Colosseum Battle", Tier::TestedSafe},
    {1241, "Pyrite Battle", Tier::TestedSafe},
    {1242, "Realgam Battle", Tier::TestedSafe},
    {1243, "Citadark Battle", Tier::TestedSafe},
    {1390, "Miror B. Battle", Tier::TestedSafe},
    {1251, "Final Battle (Greevil)", Tier::TestedSafe},
    {1252, "Shadow Lugia Battle", Tier::TestedSafe},
    {1264, "Battle Bingo", Tier::TestedSafe},
    {1265, "Battle Sim", Tier::TestedSafe},
    {1293, "Pokespot Battle", Tier::TestedSafe},
    {1410, "Willie Battle", Tier::TestedSafe},
    // Looping non-battle BGM (locations, themes, menus).
    {1068, "Mt. Battle", Tier::Experimental},
    {1075, "Pokemon Center", Tier::Experimental},
    {1070, "Gateon Port", Tier::Experimental},
    {1370, "Gateon Port 2", Tier::Experimental},
    {1124, "HQ Lab", Tier::Experimental},
    {1292, "S.S. Libra", Tier::Experimental},
    {1317, "Snagem Hideout", Tier::Experimental},
    {1341, "Citadark Isle", Tier::Experimental},
    {1409, "Cipher Lab", Tier::Experimental},
    {1129, "Purify Chamber", Tier::Experimental},
    {1468, "Orre Colosseum Lobby", Tier::Experimental},
    {1088, "Miror B. Theme", Tier::Experimental},
    {1471, "Greevil Theme", Tier::Experimental},
    {1486, "Hexagon Bros", Tier::Experimental},
    {1362, "Lugia Theme", Tier::Experimental},
    {1419, "Title Theme", Tier::Experimental},
    {1266, "Battle Now Menu", Tier::Experimental},
    {1267, "Group Battle Menu", Tier::Experimental},
    {1485, "Battle Now 2", Tier::Experimental},
    {1133, "World Map", Tier::Experimental},
    {1193, "Mt. Battle Rest Area", Tier::Experimental},
    {1194, "Mt. Battle Reception", Tier::Experimental},
    {1126, "Krabby Club", Tier::Experimental},
    {1343, "Bingo Lobby", Tier::Experimental},
    {1342, "Pokespot", Tier::Experimental},
    {1291, "Kaminko's Manor 2", Tier::Experimental},
    {4, "Kaminko's Manor", Tier::Experimental},
    {1132, "Mecha Theme 1", Tier::Experimental},
    {1412, "Mecha Theme 2", Tier::Experimental},
    {1371, "Chiriru Theme", Tier::Experimental},
    {1383, "Music Player 1", Tier::Experimental},
    {1384, "Music Player 2", Tier::Experimental},
    {1066, "Life Theme", Tier::Experimental},
    {1067, "Darkside 4", Tier::Experimental},
    {1074, "Darkside", Tier::Experimental},
    {1071, "Stand Theme", Tier::Experimental},
    {1072, "Saint Theme", Tier::Experimental},
    // Looping event (ev_*) streams; stems only, names pending the listening
    // pass like everything above.
    {1130, "Event Theme 1130", Tier::Experimental},
    {1131, "Event Theme 1131", Tier::Experimental},
    {1297, "Event Theme 1297", Tier::Experimental},
    {1361, "Event Theme 1361", Tier::Experimental},
    {1363, "Event Theme 1363", Tier::Experimental},
    {1391, "Event Theme 1391", Tier::Experimental},
    {1408, "Event Theme 1408", Tier::Experimental},
    {1411, "Event Theme 1411", Tier::Experimental},
    {1448, "Event Theme 1448", Tier::Experimental},
    {1449, "Event Theme 1449", Tier::Experimental},
    {1450, "Event Theme 1450", Tier::Experimental},
    {1469, "Event Theme 1469", Tier::Experimental},
    {1470, "Event Theme 1470", Tier::Experimental},
    // The low-id block (unused/leftover streams).
    {1, "Snowfreak (unused)", Tier::Experimental},
    {2, "Shinpi (unused)", Tier::Experimental},
    {3, "Tretre (unused)", Tier::Experimental},
    {5, "Latin (unused)", Tier::Experimental},
    {6, "Keio Azuchi (unused)", Tier::Experimental},
    {7, "Shadow Event (unused)", Tier::Experimental},
};

// Battle locations (battlefield-table indices). tested-safe = the retail VS
// colosseum pool; experimental = colosseum rooms the retail pool never offers
// plus story battle maps (outdoor/cave/water rooms alter Nature Power /
// Camouflage / Secret Power terrain -- test once if strict OU parity
// matters). Index 0 (null staging entry), the story/staging duplicates and
// anything >= 66 (GetBattleField returns NULL -> venue 0 -> hang risk) are
// excluded by omission. "Game default" (absent) is stock Pyrite Colosseum.
constexpr StyleOption VENUES[] = {
    {36, "Pyrite Colosseum", Tier::TestedSafe},
    {37, "Mt. Battle Colosseum", Tier::TestedSafe},
    {38, "Realgam Colosseum", Tier::TestedSafe},
    // Colosseums outside the retail VS pool.
    {35, "Phenac Colosseum", Tier::Experimental},
    {39, "Orre Colosseum", Tier::Experimental},
    // Story battle maps; 54 is the best non-colosseum bet (the game's own demo
    // battles stage there).
    {54, "Gateon Port docks", Tier::Experimental},
    {5, "Outskirt Stand", Tier::Experimental},
    {7, "Phenac City streets", Tier::Experimental},
    {9, "Phenac Pre Gym", Tier::Experimental},
    {8, "Phenac house interior", Tier::Experimental},
    {10, "Pyrite Town streets", Tier::Experimental},
    {11, "Pyrite building", Tier::Experimental},
    {12, "Pyrite cave (Miror B. hideout)", Tier::Experimental},
    {14, "Agate Village", Tier::Experimental},
    {15, "Agate cave", Tier::Experimental},
    {16, "Relic Shrine", Tier::Experimental},
    {17, "Cipher Lab grounds", Tier::Experimental},
    {18, "Cipher Lab B1", Tier::Experimental},
    {41, "Cipher Lab garage", Tier::Experimental},
    {19, "Mt. Battle entrance", Tier::Experimental},
    {20, "Mt. Battle lobby", Tier::Experimental},
    {21, "Mt. Battle valley", Tier::Experimental},
    {22, "Mt. Battle magma zone", Tier::Experimental},
    {23, "Mt. Battle clouds", Tier::Experimental},
    {42, "S.S. Libra deck", Tier::Experimental},
    {24, "Realgam dome", Tier::Experimental},
    {25, "Realgam Tower hall", Tier::Experimental},
    {64, "Battle Bingo hall", Tier::Experimental},
    {43, "Cipher Key Lair 1F", Tier::Experimental},
    {44, "Cipher Key Lair roof", Tier::Experimental},
    {63, "Cipher Key Lair dome", Tier::Experimental},
    {45, "Cipher Key Lair grounds", Tier::Experimental},
    {46, "Citadark dome 2F", Tier::Experimental},
    {47, "Citadark dome B1", Tier::Experimental},
    {48, "Citadark fort", Tier::Experimental},
    {49, "Citadark outer dome", Tier::Experimental},
    {50, "Citadark shore", Tier::Experimental},
    {52, "Pokemon HQ Lab", Tier::Experimental},
    {53, "HQ Lab grounds", Tier::Experimental},
    {55, "Gateon lighthouse top", Tier::Experimental},
    {56, "Kaminko's manor grounds", Tier::Experimental},
    {34, "Snagem Hideout", Tier::Experimental},
    {57, "Rock Pokespot", Tier::Experimental},
    {58, "Oasis Pokespot", Tier::Experimental},
    {59, "Cave Pokespot", Tier::Experimental},
};

const StyleOption* FindOption(std::span<const StyleOption> table, int id)
{
  for (const StyleOption& option : table)
  {
    if (option.id == id)
      return &option;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------

std::mutex s_mutex;
// The guest's submitted model, already validated. Overwritten (or reset) by
// every TeamData arrival; wins over the host's fallback dropdown.
std::optional<int> s_guest_model;
// True after PrepareForStart turned MAIN_ENABLE_CHEATS on for a cosmetics-only
// session; tells RegenerateFromConfig the user's real OU choice is still OFF
// and tells EndSession to give the flag back.
bool s_cheats_forced = false;
// Whether the most recent RegenerateIni left a non-empty block in the file.
bool s_block_active = false;
// True between BeginSession and EndSession -- i.e. a netplay room owns the
// lifecycle. A solo boot has no room, so its cleanup rides the emulation
// state hook instead, and that hook must stand down while a room is open
// (in netplay the game stopping does not mean the session is over).
bool s_netplay_session = false;

// Into the per-session GBA detect log -- the file testers already know how to
// hand over. GBADetectLog only exists in mGBA builds (same guard TeamInjector's
// cleanup line uses; an unguarded call here is a link error with USE_MGBA=OFF).
void LogNote(const std::string& line)
{
#ifdef HAS_LIBMGBA
  GBADetectLog::NoteBoot(line);
#else
  (void)line;
#endif
}

std::string LocalIniPath()
{
  return File::GetUserPath(D_GAMESETTINGS_IDX) + std::string(GAME_INI_NAME);
}

void AppendLine(std::string* block, u32 addr_word, u32 value_word)
{
  if (!block->empty())
    block->push_back('\n');
  *block += fmt::format("{:08X} {:08X}", addr_word, value_word);
}

// Remove every line of ours from a section's line list. For [ActionReplay]
// that means the "$OrreLink Battle Style" name line plus everything up to the
// next "$" name (or the section end); for the enabled/disabled sections it is
// a single exact name line. Comparison ignores surrounding whitespace only --
// user comments and codes are preserved byte for byte.
std::vector<std::string> WithoutStyleBlock(const std::vector<std::string>& lines)
{
  std::vector<std::string> kept;
  kept.reserve(lines.size());
  bool in_ours = false;
  for (const std::string& line : lines)
  {
    const std::string_view trimmed = StripWhitespace(line);
    if (trimmed == STYLE_CODE_LINE)
    {
      in_ours = true;
      continue;
    }
    if (in_ours && !trimmed.empty() && trimmed[0] == '$')
      in_ours = false;
    if (!in_ours)
      kept.push_back(line);
  }
  return kept;
}

std::vector<std::string> WithoutExactLine(const std::vector<std::string>& lines,
                                          std::string_view target)
{
  std::vector<std::string> kept;
  kept.reserve(lines.size());
  for (const std::string& line : lines)
  {
    if (StripWhitespace(line) != target)
      kept.push_back(line);
  }
  return kept;
}

// SetLines, or drop the section entirely when nothing of substance remains --
// so an all-default regenerate leaves the user's file as clean as it found it.
void StoreSection(Common::IniFile* ini, std::string_view section, std::vector<std::string> lines)
{
  bool only_blank = true;
  for (const std::string& line : lines)
  {
    if (!StripWhitespace(line).empty())
    {
      only_blank = false;
      break;
    }
  }
  if (only_blank)
    ini->DeleteSection(section);
  else
    ini->SetLines(section, std::move(lines));
}
}  // namespace

std::span<const StyleOption> ModelTable()
{
  return MODELS;
}

std::span<const StyleOption> MusicTable()
{
  return MUSICS;
}

std::span<const StyleOption> VenueTable()
{
  return VENUES;
}

bool IsValidModelId(int id)
{
  return FindOption(MODELS, id) != nullptr;
}

bool IsValidMusicId(int id)
{
  return FindOption(MUSICS, id) != nullptr;
}

bool IsValidVenueId(int id)
{
  return FindOption(VENUES, id) != nullptr;
}

std::string GenerateCodeBlock(std::optional<int> p1_model, std::optional<int> p2_model,
                              std::optional<int> bgm, std::optional<int> venue)
{
  // Each field independently: an id that fails validation is treated as absent
  // (fall back, never clamp -- an out-of-table model id dereferences garbage
  // past trainer_pkx_data, an out-of-table venue can hang the battle load).
  const bool p1 = p1_model && IsValidModelId(*p1_model);
  const bool p2 = p2_model && IsValidModelId(*p2_model);
  const bool music = bgm && IsValidMusicId(*bgm);
  const bool location = venue && IsValidVenueId(*venue);

  std::string block;
  if (!p1 && !p2 && !music && !location)
    return block;  // genuine game default: no code at all

  // Guard line first: "00000000 40000000" is the AR full terminator. On the
  // host -- where this runs as its own fresh code -- it is a no-op (Dolphin
  // logs it as an unsupported zero code and moves on), but a GUEST executes
  // every synced code merged into ONE "Synced Codes" stream, where an
  // all-lines-until conditional leaked by an earlier code would otherwise
  // swallow our writes. The terminator closes any such scope. Our own lines
  // are unconditional 04/02 writes, so nothing here can leak forward either.
  AppendLine(&block, 0x00000000, 0x40000000);

  if (p1)
  {
    // nops first, then the pinned "li r3, id" sites (see MODEL_P1_LINES).
    AppendLine(&block, MODEL_P1_LINES[0], PPC_NOP);
    AppendLine(&block, MODEL_P1_LINES[1], PPC_NOP);
    const u32 li = PPC_LI_R3 | (static_cast<u32>(*p1_model) & 0xFF);
    AppendLine(&block, MODEL_P1_LINES[2], li);
    AppendLine(&block, MODEL_P1_LINES[3], li);
    AppendLine(&block, MODEL_P1_LINES[4], li);
  }
  if (p2)
  {
    AppendLine(&block, MODEL_P2_LINE, PPC_LI_R3 | (static_cast<u32>(*p2_model) & 0xFF));
  }
  if (music)
  {
    // Three identical u32 slots (primary encoding; see MUSIC_LINES).
    const u32 value = static_cast<u32>(*bgm) & 0xFFFF;
    for (const u32 addr : MUSIC_LINES)
      AppendLine(&block, addr, value);
  }
  if (location)
  {
    // All four link-battle records, 16-bit writes (value's upper half = fill
    // count 0, i.e. one write each; see VENUE_LINES).
    const u32 value = static_cast<u32>(*venue) & 0xFFFF;
    for (const u32 addr : VENUE_LINES)
      AppendLine(&block, addr, value);
  }
  return block;
}

Selection ConfigSelection()
{
  Selection sel;
  sel.host_model = Config::Get(Config::MAIN_XD_STYLE_HOST_MODEL);
  sel.guest_model_fallback = Config::Get(Config::MAIN_XD_STYLE_GUEST_MODEL);
  sel.music = Config::Get(Config::MAIN_XD_STYLE_MUSIC);
  sel.venue = Config::Get(Config::MAIN_XD_STYLE_VENUE);
  return sel;
}

void BeginSession()
{
  {
    std::lock_guard lock(s_mutex);
    s_guest_model.reset();
    s_cheats_forced = false;
    s_block_active = false;
    s_netplay_session = true;
  }
  // All-default = pure removal of anything a crashed session left behind.
  RegenerateIni(Selection{}, /*ou_enabled=*/true, nullptr);
}

void SetGuestModel(std::optional<int> id)
{
  {
    std::lock_guard lock(s_mutex);
    if (id && IsValidModelId(*id))
      s_guest_model = *id;
    else
      s_guest_model.reset();  // no/invalid preference: the host fallback wins
  }
  LogNote(id && IsValidModelId(*id) ?
              fmt::format("battlestyle guest submitted model={:#x}", *id) :
              std::string{"battlestyle guest submitted no model preference"});
}

bool RegenerateIni(const Selection& sel, bool ou_enabled, std::string* status)
{
  // Guest stash (already validated) wins over the host's fallback dropdown --
  // the same precedence the socket-3 team fallback uses.
  std::optional<int> guest_model;
  {
    std::lock_guard lock(s_mutex);
    guest_model = s_guest_model;
  }
  const bool guest_submitted = guest_model.has_value();
  if (!guest_model)
    guest_model = sel.guest_model_fallback > 0 ? std::optional<int>(sel.guest_model_fallback) :
                                                 std::nullopt;

  // Side mapping (host -> "Player" block, guest -> "Opponent" line) is the
  // expected orientation; if the one-time emulator test shows it reversed,
  // swap the first two arguments HERE only.
  const std::string block = GenerateCodeBlock(
      sel.host_model > 0 ? std::optional<int>(sel.host_model) : std::nullopt, guest_model,
      sel.music > 0 ? std::optional<int>(sel.music) : std::nullopt,
      sel.venue > 0 ? std::optional<int>(sel.venue) : std::nullopt);
  const bool active = !block.empty();

  // One line per regeneration so a tester's log says exactly what was picked
  // and what it became -- "picked a venue, battle looked stock" is unanswerable
  // without this. The AR lines themselves follow when a block exists.
  {
    const auto id_or = [](std::optional<int> v) { return v ? fmt::format("{:#x}", *v) : std::string{"default"}; };
    LogNote(fmt::format(
        "battlestyle sel host_model={} guest_model={}{} music={} venue={} -> block={} ou_disable={}",
        id_or(sel.host_model > 0 ? std::optional<int>(sel.host_model) : std::nullopt),
        id_or(guest_model), guest_submitted ? " (guest pick)" : "",
        sel.music > 0 ? fmt::format("{}", sel.music) : std::string{"default"},
        sel.venue > 0 ? fmt::format("{}", sel.venue) : std::string{"default"},
        active ? "active" : "empty", active && !ou_enabled ? 1 : 0));
    if (active)
      for (const std::string& op : SplitString(block, '\n'))
        LogNote(fmt::format("battlestyle ar {}", op));
  }

  const std::string path = LocalIniPath();
  Common::IniFile ini;
  ini.Load(path);

  // IniFile's Load->Save round trip is lossy (blank lines and comments are
  // normalized away), and this function runs on EVERY room close, for any
  // game. So the file is only written when this call actually changes it:
  // something of ours removed, or something new to add. A user's hand-kept
  // GXXE01.ini survives a hundred Melee rooms untouched, and no empty file is
  // ever created just to hold nothing.
  bool removed_any = false;

  // [ActionReplay]: strip any previous block of ours, then append the fresh
  // one at the end -- SyncCodes ships codes in file order, so the generated
  // block always lands after the Sys-bundled codes AND after the user's own.
  {
    std::vector<std::string> lines;
    ini.GetLines("ActionReplay", &lines, false);
    const std::size_t before = lines.size();
    lines = WithoutStyleBlock(lines);
    removed_any |= lines.size() != before;
    if (active)
    {
      lines.emplace_back(STYLE_CODE_LINE);
      for (const std::string& op : SplitString(block, '\n'))
        lines.push_back(op);
    }
    StoreSection(&ini, "ActionReplay", std::move(lines));
  }

  // [ActionReplay_Enabled]: our "$" line, exactly how Sys enables OU Fixes.
  {
    std::vector<std::string> lines;
    ini.GetLines("ActionReplay_Enabled", &lines, false);
    const std::size_t before = lines.size();
    lines = WithoutExactLine(lines, STYLE_CODE_LINE);
    removed_any |= lines.size() != before;
    if (active)
      lines.emplace_back(STYLE_CODE_LINE);
    StoreSection(&ini, "ActionReplay_Enabled", std::move(lines));
  }

  // [ActionReplay_Disabled]: only while cosmetics force cheats on for a host
  // whose OU toggle is off does the Sys-enabled "$XD OU Fixes" need a local
  // disable. Note this also removes an identical line the user might have
  // written by hand -- acceptable, because the launcher's OU toggle (not a
  // hand edit) is the supported control for that code.
  {
    std::vector<std::string> lines;
    ini.GetLines("ActionReplay_Disabled", &lines, false);
    const std::size_t before = lines.size();
    lines = WithoutExactLine(lines, OU_FIXES_LINE);
    removed_any |= lines.size() != before;
    if (active && !ou_enabled)
      lines.emplace_back(OU_FIXES_LINE);
    StoreSection(&ini, "ActionReplay_Disabled", std::move(lines));
  }

  if (!active && !removed_any)
  {
    // Nothing of ours in the file and nothing to add: leave the disk alone.
    std::lock_guard lock(s_mutex);
    s_block_active = false;
    if (status)
      *status = "battle style: game default";
    return true;
  }

  File::CreateFullPath(path);
  const bool saved = ini.Save(path);

  {
    std::lock_guard lock(s_mutex);
    s_block_active = saved && active;
  }

  if (status)
  {
    if (!saved)
    {
      *status = "battle style: could not write " + std::string(GAME_INI_NAME);
    }
    else if (!active)
    {
      *status = "battle style: game default";
    }
    else
    {
      const auto describe = [](std::span<const StyleOption> table, std::optional<int> id) {
        const StyleOption* option = id ? FindOption(table, *id) : nullptr;
        return std::string(option ? option->name : "default");
      };
      *status = fmt::format("battle style: host model {}, guest model {}{}, music {}, venue {}",
                            describe(MODELS, sel.host_model), describe(MODELS, guest_model),
                            guest_submitted ? " (guest's pick)" : "", describe(MUSICS, sel.music),
                            describe(VENUES, sel.venue));
    }
  }
  return saved;
}

bool RegenerateFromConfig(std::string* status)
{
  bool forced;
  {
    std::lock_guard lock(s_mutex);
    forced = s_cheats_forced;
  }
  // Once PrepareForStart has forced cheats on, MAIN_ENABLE_CHEATS no longer
  // reflects the user's OU choice -- it was OFF, or no force would have
  // happened. Without this, the regenerate before a SECOND Start in the same
  // room would drop the OU-Fixes disable and hand the guest OU patches the
  // host never asked for.
  const bool ou_enabled = !forced && Config::Get(Config::MAIN_ENABLE_CHEATS);
  return RegenerateIni(ConfigSelection(), ou_enabled, status);
}

void PrepareForStart()
{
  // Solo boots have no room-closed event, so their cleanup rides the emulation
  // state instead: when the core reaches Uninitialized outside a netplay
  // session, the session is over by definition. Registered once, stands down
  // while a room is open (there, stopping the game does not end the session --
  // the room-closed path owns cleanup). Same once-flag idiom as TeamInjector's
  // deferred purge, and EndSession is idempotent and only writes the INI when
  // something of ours is actually in it.
  static std::once_flag hook_once;
  std::call_once(hook_once, [] {
    // The handle is [[nodiscard]] and dropping it unregisters the hook -- a
    // static keeps it alive for the process, same as TeamInjector's.
    static Common::EventHook s_state_hook;
    s_state_hook = Core::AddOnStateChangedCallback([](Core::State state) {
      if (state != Core::State::Uninitialized)
        return;
      {
        std::lock_guard lock(s_mutex);
        if (s_netplay_session)
          return;
      }
      EndSession();
    });
  });

  RegenerateFromConfig(nullptr);

  bool active;
  bool forced;
  {
    std::lock_guard lock(s_mutex);
    active = s_block_active;
    forced = s_cheats_forced;
  }
  // The AR engine and SyncCodes are both gated on the host's global cheats
  // flag; a cosmetic pick with the OU toggle off would silently do nothing
  // without this. Runs before RequestStartGame, therefore before the
  // SetupNetSettings snapshot and the SyncCodes INI read. When nothing is
  // active the flag is left entirely alone -- a stock session stays stock.
  if (active && !forced && !Config::Get(Config::MAIN_ENABLE_CHEATS))
  {
    {
      std::lock_guard lock(s_mutex);
      s_cheats_forced = true;
    }
    // RegenerateFromConfig above already saw the pre-force value (OU off) and
    // wrote the "$XD OU Fixes" local disable, so forcing afterwards is safe.
    Config::SetBaseOrCurrent(Config::MAIN_ENABLE_CHEATS, true);
    LogNote("battlestyle cheats forced on for this session (OU stays disabled)");
  }
  else if (!active && forced)
  {
    // The force is a per-start invariant, not a latch. If the block emptied
    // between battles in the same room (guest re-submitted without a model,
    // host zeroed the dropdowns), the regenerate above stripped the OU-Fixes
    // disable -- so leaving cheats forced on would ship Sys-enabled OU
    // patches the host's toggle says are OFF. Give the flag back before the
    // SetupNetSettings snapshot reads it.
    {
      std::lock_guard lock(s_mutex);
      s_cheats_forced = false;
    }
    Config::SetBaseOrCurrent(Config::MAIN_ENABLE_CHEATS, false);
    LogNote("battlestyle block emptied; forced cheats handed back");
  }
}

void EndSession()
{
  bool forced;
  {
    std::lock_guard lock(s_mutex);
    s_guest_model.reset();
    forced = s_cheats_forced;
    s_cheats_forced = false;
    s_netplay_session = false;
  }
  // Pure removal: with the stash cleared and an all-default selection this
  // strips the block, its enabled line and the OU-Fixes disable, leaving the
  // user's GXXE01.ini as it was before the session.
  RegenerateIni(Selection{}, /*ou_enabled=*/true, nullptr);
  // Give the cheats flag back if the pre-start hook forced it: the user's OU
  // toggle reads this key, and leaving it flipped would turn a cosmetics-only
  // session into OU rules next time.
  if (forced)
  {
    Config::SetBaseOrCurrent(Config::MAIN_ENABLE_CHEATS, false);
    Config::Save();
  }
#ifdef HAS_LIBMGBA
  // The session's log is closed by the time cleanup runs; append like the team
  // purge does, so the same file also records that the block was taken out.
  GBADetectLog::LogPostSession(
      fmt::format("battlestyle cleanup done cheats_restored={}", forced ? 1 : 0));
#endif
}
}  // namespace XDNetplay::BattleCustomizer
