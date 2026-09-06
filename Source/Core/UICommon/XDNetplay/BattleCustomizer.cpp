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
#include "Core/System.h"

#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#include "Core/HW/GBADetectLog.h"
#endif

#include "Common/IOFile.h"

#include "UICommon/XDNetplay/FormatRules.h"
#include "UICommon/XDNetplay/Gen3Save.h"

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

// Trainer models, GBA-LINK PATH. Field-tested fact: Ralf's community model
// modifier (li-r3 pins at 0x801FFEF0..0x801FFF00, 0x801FD4FC, 0x802262CC,
// 0x801FFF84) does NOTHING in link battles -- those sites serve the story
// (TID 0x1388) and deck-trainer paths. The link flow instead converts each
// GBA save's game+gender to a virtual trainer id 0x1389..0x138E at handshake
// (fn 0x80047884) and stores it as a u16 at +0 of fixed .bss per-side structs
// (accessor 0x80047CEC: base 0x804299F8 + 0x18 + slot*0x1320). The battle
// scene reads that u16 and maps it to a model through the switch at
// 0x801FFE74. Derived by disassembly of a clean main.dol in this repo; the
// runtime address math is calibrated by the venue writes, which target the
// same battle records and visibly work on device.
//
// Side mapping (slot0 = host under OrreLink's port order) is the one
// unverified hop: if a device test shows the two sides swapped, swap the P1/P2
// constants below -- nothing else changes.
//
// Tier 1 (any of the six GBA player models): pin the side's TID u16. 16-bit
// writes ONLY, upper halfword zero -- it is a repeat count, and a nonzero one
// would clobber the slot-ordinal u16 at struct+2.
constexpr u32 MODEL_TID_P1_LINES[] = {
    0x02429A10,  // 16-bit write: side-0 VS struct TID @ 0x80429A10 (A slot)
    0x0242E690,  // 16-bit write: side-0 staging block @ 0x8042E690 (B slot, belt-and-braces
                 // against the same-frame B->A memcpy at battle start, 0x8004D9E8)
};
constexpr u32 MODEL_TID_P2_LINES[] = {
    0x0242AD30,  // 16-bit write: side-1 VS struct TID @ 0x8042AD30 (A slot)
    0x0242F9B0,  // 16-bit write: side-1 staging block @ 0x8042F9B0 (B slot)
};
// Tier 2 (any model id): route the side through a known switch arm by TID,
// then patch that arm's li. Original words verified in main.dol:
// 0x801FFF60 = 38600005 (TID 0x138D -> Brendan Emerald), 0x801FFF68 =
// 38600004 (TID 0x138E -> May Emerald).
constexpr u16 TID_ARM_P1 = 0x138D;         // Emerald male arm, patched for host picks
constexpr u16 TID_ARM_P2 = 0x138E;         // Emerald female arm, patched for guest picks
constexpr u32 MODEL_ARM_P1_LINE = 0x041FFF60;  // 32-bit write: li r3, id @ 0x801FFF60
constexpr u32 MODEL_ARM_P2_LINE = 0x041FFF68;  // 32-bit write: li r3, id @ 0x801FFF68
constexpr u32 PPC_LI_R3 = 0x38600000;      // li r3, 0 -- the id occupies the low byte

// Model id -> the virtual TID whose switch arm yields it natively (the six
// GBA player models; everything else needs the Tier-2 arm patch).
constexpr u16 GbaModelTid(int model_id)
{
  switch (model_id)
  {
  case 0x09: return 0x1389;  // Brendan (Ruby/Sapphire)
  case 0x08: return 0x138A;  // May (Ruby/Sapphire)
  case 0x07: return 0x138B;  // Red (FireRed/LeafGreen)
  case 0x06: return 0x138C;  // Leaf (FireRed/LeafGreen)
  case 0x05: return 0x138D;  // Brendan (Emerald)
  case 0x04: return 0x138E;  // May (Emerald)
  default: return 0;
  }
}

// Pre-battle bust remap (the connection-screen close-up). The bust never
// consults the TID->model switch: a save-class mapper (fn 0x80085BB0; game u32
// at staging+4, gender byte in the save mirror) indexes two 7-entry u32
// widget-id tables in DOL .data, and the chosen widget carries the model the
// menu close-up renders. Remapping the entry for a side's SAVE class to the
// PICKED model's class makes the bust match the battle. Verified by
// disassembly with an exhaustive reach audit: the tables are read only by the
// ten class-indexed connection-menu sites and written by nothing, so the remap
// is cosmetic-only and nothing fights it. Vanilla words, clean main.dol at
// file 0x2E8684/0x2E86A0: A = {208,209,20A,210,211,212,213}, B = {201..207}.
constexpr u32 BUST_TABLE_A = 0x042EB684;  // 32-bit writes into 0x802EB684[class]
constexpr u32 BUST_TABLE_B = 0x042EB6A0;  // 32-bit writes into 0x802EB6A0[class]
constexpr u32 BUST_A_BY_CLASS[7] = {0, 0x209, 0x20A, 0x210, 0x211, 0x212, 0x213};
constexpr u32 BUST_B_BY_CLASS[7] = {0, 0x202, 0x203, 0x204, 0x205, 0x206, 0x207};

// The bust widgets per class, both sizes. Each widget's record sits at the
// static address 0x803001A8 + id*72 (verified dump), with five per-language
// 12-byte sub-records at +8 whose W/H u16 pair is one u32 at sub+4. Zeroing
// that word on all five sub-records makes the widget draw NOTHING -- the
// chosen way to hide the bust when a picked model has no portrait, because it
// touches no lookup path that can miss (repointing at absent records or
// textures null-derefs; a zero-size crop just draws no pixels). The busts are
// pre-rendered 2D art -- the disc ships exactly seven portraits -- so hiding
// is the only honest presentation for the other sixty models.
constexpr u32 BUST_WIDGET_A[7] = {0x208, 0x209, 0x20A, 0x210, 0x211, 0x212, 0x213};  // small
constexpr u32 BUST_WIDGET_B[7] = {0x201, 0x202, 0x203, 0x204, 0x205, 0x206, 0x207};  // large
constexpr u32 BustWhWordAddr(u32 widget_id, u32 language_slot)
{
  return 0x04000000u | ((0x803001A8u + widget_id * 72 + 8 + 12 * language_slot + 4) & 0x01FFFFFFu);
}

// THE preview mugshot drawer -- found on the sixth attempt, and the reason
// the five before it failed: the mugshot never goes through the element/
// image system at all. It is drawn by a raw-GX quad writer at 0x800845AC
// that reads a PRE-RESOLVED texture-object pointer from a per-side BSS slot
// ([0x80435050 + side*8 + 0xC], filled by an async loader callback at
// 0x800475B8) and pushes a textured quad directly. Every earlier patch
// (element records, img entries, dispatcher jump tables, SetElementImage
// call sites -- all applied and verified on device) targeted machinery this
// drawer never touches. Eight bl callers, byte-verified: the 6v6 preview
// sides (0x8007644C/58), the 3v3 sides (0x80075D64/70) and the 4-slot
// layout (0x80076C9C/A8/B4/C0).
//
// The kill reuses the drawer's OWN control flow: at 0x800845F8 it does
// `beq 0x80084738` -- skip straight to the epilogue when the texobj is NULL
// (vanilla takes this branch for empty sides). Patching that beq
// (0x41820140) to an unconditional b (0x48000140) makes EVERY call take the
// game's own no-draw path: stack-balanced by construction, covers all three
// preview layouts and any caller, in text (never reloaded, never raced).
constexpr u32 PREVIEW_DRAWER_SKIP_LINE = 0x040845F8;
constexpr u32 PREVIEW_DRAWER_SKIP_VALUE = 0x48000140;  // vanilla 0x41820140 (beq -> b)

// Model id -> bust class (1 FRLG-m, 2 FRLG-f, 3 RS-m, 4 RS-f, 5 E-m, 6 E-f).
// Only the six GBA player models have bust widgets in the connection menu;
// anything else returns 0 = no bust remap (battle model still changes).
constexpr int ModelBustClass(int model_id)
{
  switch (model_id)
  {
  case 0x07: return 1;  // Red (FireRed/LeafGreen)
  case 0x06: return 2;  // Leaf (FireRed/LeafGreen)
  case 0x09: return 3;  // Brendan (Ruby/Sapphire)
  case 0x08: return 4;  // May (Ruby/Sapphire)
  case 0x05: return 5;  // Brendan (Emerald)
  case 0x04: return 6;  // May (Emerald)
  default: return 0;
  }
}

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
// tested-safe = the six GBA player models, served natively by the link flow's
// TID switch (pure data pin); experimental = everything else -- reachable only
// through the Tier-2 arm patch, and whether the link scene's people archive
// can load these models at all is not yet device-verified.
constexpr StyleOption MODELS[] = {
    {0x01, "Michael (variant 1)", Tier::Experimental},
    {0x02, "Michael (variant 2, Snag Machine)", Tier::Experimental},
    {0x03, "Michael (variant 2)", Tier::Experimental},
    {0x04, "May (Emerald)", Tier::TestedSafe},
    {0x05, "Brendan (Emerald)", Tier::TestedSafe},
    {0x06, "Leaf (FireRed/LeafGreen)", Tier::TestedSafe},
    {0x07, "Red (FireRed/LeafGreen)", Tier::TestedSafe},
    {0x08, "May (Ruby/Sapphire)", Tier::TestedSafe},
    {0x09, "Brendan (Ruby/Sapphire)", Tier::TestedSafe},
    {0x0B, "Cipher Peon (female)", Tier::Experimental},
    {0x0C, "Cipher Peon (male A)", Tier::Experimental},
    {0x0D, "Cipher Peon (male B)", Tier::Experimental},
    {0x0E, "Cipher Peon (male C)", Tier::Experimental},
    {0x0F, "Resix (Hexagon Bros, red)", Tier::Experimental},
    {0x10, "Blusix (Hexagon Bros, blue)", Tier::Experimental},
    {0x11, "Browsix (Hexagon Bros, brown)", Tier::Experimental},
    {0x12, "Yellosix (Hexagon Bros, yellow)", Tier::Experimental},
    {0x13, "Purpsix (Hexagon Bros, purple)", Tier::Experimental},
    {0x14, "Greesix (Hexagon Bros, green)", Tier::Experimental},
    {0x15, "Cipher Admin Lovrina", Tier::Experimental},
    {0x16, "Sailor", Tier::Experimental},
    {0x17, "Thug Zook", Tier::Experimental},
    {0x18, "Cipher Admin Ardos", Tier::Experimental},
    {0x19, "Matron", Tier::Experimental},
    {0x1A, "Grand Master Greevil", Tier::Experimental},
    {0x1C, "Cipher Admin Eldes", Tier::Experimental},
    {0x1D, "Cipher Admin Gorigan", Tier::Experimental},
    {0x1E, "Snagem Head Gonzap", Tier::Experimental},
    {0x1F, "Super Trainer (female A)", Tier::Experimental},
    {0x20, "Super Trainer (female B)", Tier::Experimental},
    {0x21, "Vander", Tier::Experimental},
    {0x22, "Super Trainer (male B)", Tier::Experimental},
    {0x23, "Super Trainer (male C)", Tier::Experimental},
    {0x24, "Hunter", Tier::Experimental},
    {0x25, "Beauty", Tier::Experimental},
    {0x26, "Casual Dude", Tier::Experimental},
    {0x27, "Fun Old Man", Tier::Experimental},
    {0x28, "Curmudgeon", Tier::Experimental},
    {0x2B, "Miror B.", Tier::Experimental},
    {0x2C, "Bodybuilder (female)", Tier::Experimental},
    {0x2D, "Bodybuilder (male)", Tier::Experimental},
    {0x2E, "Mt. Battle Master Battlus", Tier::Experimental},
    {0x2F, "Casual Guy", Tier::Experimental},
    {0x30, "Researcher", Tier::Experimental},
    {0x31, "Rider", Tier::Experimental},
    {0x32, "Navigator", Tier::Experimental},
    {0x33, "Pre Gym Leader Justy", Tier::Experimental},
    {0x34, "Team Snagem Grunt (A)", Tier::Experimental},
    {0x35, "Team Snagem Grunt (B)", Tier::Experimental},
    {0x36, "Chobin", Tier::Experimental},
    {0x37, "Chaser (female A)", Tier::Experimental},
    {0x38, "Chaser (female B)", Tier::Experimental},
    {0x39, "Chaser (male)", Tier::Experimental},
    {0x3A, "Cail", Tier::Experimental},
    {0x3B, "Cooltrainer (female)", Tier::Experimental},
    {0x3C, "Cooltrainer (male)", Tier::Experimental},
    {0x3D, "Cipher Admin Snattle", Tier::Experimental},
    {0x3E, "Willie", Tier::Experimental},
    {0x3F, "Worker", Tier::Experimental},
    {0x42, "Michael (variant 3, Snag Machine)", Tier::Experimental},
    {0x43, "Michael (variant 3)", Tier::Experimental},
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
// "No music": the VS BGM selector (0x8004D580) hands staBGM_tunaide[n] to a
// null-guarded setter with no range check, and the streamed-voice starter
// (0x801871B4) tests the stored id against ZERO before touching the GSDVD
// stream table -- id 0 is the engine's own "start nothing" sentinel: no
// stream, no fault, cries/hit SFX unaffected (they go through MusyX, a
// separate table). The table id is kept OUTSIDE u16 range so it can never
// collide with a real stream id or with 0 (= "Game default" in the pickers);
// GenerateCodeBlock maps it to the written value 0. Requested by a community
// player; the old silent "Event Theme" pick was removed for the same effect
// with none of the guarantees.
constexpr int MUSIC_SILENT_ID = 0x10000;

constexpr StyleOption MUSICS[] = {
    {MUSIC_SILENT_ID, "No music (silent battle)", Tier::TestedSafe},
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
    // The low-id block (unused/leftover LOOPING STREAMS -- ids 1-6 are all
    // classified as streams in the original archive research, re-checked
    // 2026-08). Id 7 ("Shadow Event") is deliberately ABSENT: the research
    // classified it as a sequenced-archive CONTAINER, the excluded class
    // (sequenced/ME/jingle data, not a looping stream), and it slipped into
    // this tier anyway; a field report confirmed picking it plays no music at
    // all. Excluded by omission, exactly like the jingles. The re-check found
    // no other excluded-class entries in the experimental tier: everything
    // above is a looping stm_* / ev_* stream per the same classification.
    {1, "Snowfreak (unused)", Tier::Experimental},
    {2, "Shinpi (unused)", Tier::Experimental},
    {3, "Tretre (unused)", Tier::Experimental},
    {5, "Latin (unused)", Tier::Experimental},
    {6, "Keio Azuchi (unused)", Tier::Experimental},
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
// The user's MAIN_ENABLE_CHEATS value from before PrepareForStart first
// reconciled it for this session (the flag is DERIVED state during an XD
// session: on iff the style/rules block or the OU format needs the AR engine).
// Set once per session on the first reconciliation that changes the flag;
// EndSession writes it back, so a desktop user's global cheats preference for
// other games survives XD sessions unchanged.
std::optional<bool> s_cheats_before;
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

bool ModelHasPortrait(int model_id)
{
  // Exactly the models with a bust class -- i.e. the six GBA player models the
  // connection menu binds bust widgets for (see ModelBustClass above).
  return ModelBustClass(model_id) != 0;
}

std::string GenerateCodeBlock(std::optional<int> p1_model, std::optional<int> p2_model,
                              std::optional<int> bgm, std::optional<int> venue, int p1_class,
                              int p2_class, bool hide_default_busts)
{
  // Each field independently: an id that fails validation is treated as absent
  // (fall back, never clamp -- an out-of-table model id dereferences garbage
  // past trainer_pkx_data, an out-of-table venue can hang the battle load).
  const bool p1 = p1_model && IsValidModelId(*p1_model);
  const bool p2 = p2_model && IsValidModelId(*p2_model);
  const bool music = bgm && IsValidMusicId(*bgm);
  const bool location = venue && IsValidVenueId(*venue);

  std::string block;
  if (!p1 && !p2 && !music && !location && !hide_default_busts)
    return block;  // genuine game default: no code at all
  // hide_default_busts alone still builds a block: a format session with
  // all-default cosmetic picks -- exactly the field-reported case -- must
  // emit the guard line plus the class-0 bust zeroes, or the fix (and the
  // guard preceding the appended format lines) silently no-ops.

  // Guard line first: "00000000 40000000" is the AR full terminator. On the
  // host -- where this runs as its own fresh code -- it is a no-op (Dolphin
  // logs it as an unsupported zero code and moves on), but a GUEST executes
  // every synced code merged into ONE "Synced Codes" stream, where an
  // all-lines-until conditional leaked by an earlier code would otherwise
  // swallow our writes. The terminator closes any such scope. Our own lines
  // are unconditional 04/02 writes, so nothing here can leak forward either.
  AppendLine(&block, 0x00000000, 0x40000000);

  // Each side independently: a GBA player model is a pure TID pin (Tier 1);
  // any other model routes the side through a fixed Emerald arm and patches
  // that arm's li (Tier 2). When only ONE side takes the Tier-2 route, the
  // other side's TID is pinned to its bundled-save natural value too, so a
  // same-gender custom save cannot wander into the patched arm and wear the
  // wrong model.
  const bool p1_arm = p1 && GbaModelTid(*p1_model) == 0;
  const bool p2_arm = p2 && GbaModelTid(*p2_model) == 0;
  if (p1 || p2_arm)
  {
    const u16 tid = p1 ? (p1_arm ? TID_ARM_P1 : GbaModelTid(*p1_model)) : TID_ARM_P1;
    for (const u32 addr : MODEL_TID_P1_LINES)
      AppendLine(&block, addr, tid);
  }
  if (p2 || p1_arm)
  {
    const u16 tid = p2 ? (p2_arm ? TID_ARM_P2 : GbaModelTid(*p2_model)) : TID_ARM_P2;
    for (const u32 addr : MODEL_TID_P2_LINES)
      AppendLine(&block, addr, tid);
  }
  if (p1_arm)
    AppendLine(&block, MODEL_ARM_P1_LINE, PPC_LI_R3 | (static_cast<u32>(*p1_model) & 0xFF));
  if (p2_arm)
    AppendLine(&block, MODEL_ARM_P2_LINE, PPC_LI_R3 | (static_cast<u32>(*p2_model) & 0xFF));

  // Bust remap, per side: only for the six GBA player models, only when the
  // side's save class is known, differs from the picked class, and does not
  // collide with the other side's table slot (same class on both saves would
  // make the two sides' writes fight over one entry -- skip both, bust stays
  // vanilla, and the log's ar dump shows the absence).
  {
    const int w1 = p1 ? ModelBustClass(*p1_model) : 0;
    const int w2 = p2 ? ModelBustClass(*p2_model) : 0;
    const bool classes_collide = p1_class != 0 && p1_class == p2_class;
    if (w1 != 0 && p1_class >= 1 && p1_class <= 6 && w1 != p1_class && !classes_collide)
    {
      AppendLine(&block, BUST_TABLE_A + 4 * static_cast<u32>(p1_class), BUST_A_BY_CLASS[w1]);
      AppendLine(&block, BUST_TABLE_B + 4 * static_cast<u32>(p1_class), BUST_B_BY_CLASS[w1]);
    }
    if (w2 != 0 && p2_class >= 1 && p2_class <= 6 && w2 != p2_class && !classes_collide)
    {
      AppendLine(&block, BUST_TABLE_A + 4 * static_cast<u32>(p2_class), BUST_A_BY_CLASS[w2]);
      AppendLine(&block, BUST_TABLE_B + 4 * static_cast<u32>(p2_class), BUST_B_BY_CLASS[w2]);
    }

    // A pick with NO portrait (w == 0 while a model IS picked): hide that
    // side's bust rather than showing the protagonist over a Miror B battle.
    // Zero the crop size on the class's two widgets, all five language
    // sub-records. Collision rule as above: same class on both saves shares
    // the widgets, so stand down unless BOTH sides want them hidden.
    const bool p1_hide = p1 && w1 == 0 && p1_class >= 1 && p1_class <= 6;
    const bool p2_hide = p2 && w2 == 0 && p2_class >= 1 && p2_class <= 6;
    const auto emit_hide = [&block](int cls) {
      for (u32 m = 0; m < 5; m++)
      {
        AppendLine(&block, BustWhWordAddr(BUST_WIDGET_A[cls], m), 0);
        AppendLine(&block, BustWhWordAddr(BUST_WIDGET_B[cls], m), 0);
      }
    };
    // No collision: each side hides its own class. Collision (shared class):
    // hide only when BOTH sides are bustless, emitted once.
    bool any_hide = false;
    if (p1_hide && (!classes_collide || p2_hide))
    {
      emit_hide(p1_class);
      any_hide = true;
    }
    if (p2_hide && !classes_collide)
    {
      emit_hide(p2_class);
      any_hide = true;
    }
    // Class 0 rides along with any hide OR any format pin: the column-0
    // GC-protagonist bust is every bust widget's compile-time DEFAULT record
    // (verified: all sixteen bust descriptors default to 0x201/0x208), so a
    // widget drawn before its per-class re-crop -- the team preview screen
    // does this -- shows the protagonist even though no class ever selected
    // it. The complete-image scan proved these fourteen records are the ONLY
    // path to the bust atlas, so zeroing class 0 closes the last surface.
    // With a format pinned the session is GBA-vs-GBA by construction, where
    // that default head is ALWAYS wrong (field report: both preview sides
    // showed the protagonist with default picks) -- so formats hide it
    // unconditionally. A genuine GC-vs-GBA session (where column 0 is a real
    // player's bust) never gets these lines: no format pin, no hide.
    if (any_hide || hide_default_busts)
    {
      emit_hide(0);
      // Same trigger, one line: skip the preview mugshot drawer entirely (see
      // PREVIEW_DRAWER_SKIP_LINE above).
      AppendLine(&block, PREVIEW_DRAWER_SKIP_LINE, PREVIEW_DRAWER_SKIP_VALUE);
    }
  }
  if (music)
  {
    // Three identical u32 slots (primary encoding; see MUSIC_LINES). The
    // silent pick writes 0 (see MUSIC_SILENT_ID); everything else is a u16
    // stream id.
    const u32 value = *bgm == MUSIC_SILENT_ID ? 0u : (static_cast<u32>(*bgm) & 0xFFFF);
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
    // VS-mode setup menu, stage row (menu global idx10 0x804349F4, bounded 0..6 by the
    // input handler 0x8007B520; >=7 crashes the sprite draw). Pinned to 6 -- the "random"
    // choice -- so the in-game picker cannot contradict the host's location: the game then
    // writes rand()%6 into rec+6 once at battle start (0x8004D45C), which the four lines
    // above overwrite every frame. idx11 (slide-anim "from", 0x804349F8) and the B-cancel
    // backup (0x80434A74) are pinned too so a left/right press slides and snaps back.
    // Never touch 0x80434B0C (blocks Start).
    constexpr u32 STAGE_ROW_RANDOM = 6;
    AppendLine(&block, 0x044349F4, STAGE_ROW_RANDOM);
    AppendLine(&block, 0x044349F8, STAGE_ROW_RANDOM);
    AppendLine(&block, 0x04434A74, STAGE_ROW_RANDOM);
    // Blank the "random" sprite so the row reads empty, the same signal as the blanked
    // Custom-1 rules name: sprite id 0x2D7's descriptor at 0x803001A8 + 0x2D7*0x48, five
    // language sub-records at +8 + lang*0xC, W/H word at +4 -> 0 draws nothing (the same
    // mechanism as the bust hides above).
    for (const u32 addr : {0x0430CE2Cu, 0x0430CE38u, 0x0430CE44u, 0x0430CE50u, 0x0430CE5Cu})
      AppendLine(&block, addr, 0);
  }
  return block;
}

// The community formats' in-game rulesets, pinned into Custom 1. Design:
// instead of pinning individual clause bytes -- several of whose meanings are
// unverified -- the WHOLE 144-byte slot is written with one of the game's own
// stock tournament presets (main.dol preset table at file 0x2E7C08 +
// slot*0x90: slot 3 = Lv100, min 1 / max 100 / total 600; slot 2 = Lv50,
// min 1 / max 50 / total 300; both share every other byte, tournament marker
// at +0x08, all-zero tail -- verified by extracting both slots from a clean
// GXXE01 main.dol), with exactly one change: the u16 at +0x1A (entries, the
// low half of word 6) set to 4 for the Orre bring-6-pick-4 shapes or 3 for
// the Hoenn bring-6-pick-3 shapes. Every byte is the game's own tournament
// value, so no unverified meaning is ever guessed at. The rules screen's menu
// globals are pinned alongside so the selection is Double / Custom 1 and
// enforcement (GameCube-side entry validator, lha ruleset+0/+2) routes
// through the pinned slot. Do-not-pin findings from the field map are
// respected: never the getter's working buffer (rebuilt in-call), never
// ctx+0x10 (races the random venue roll); the venue stays on the
// field-proven record +0x06 pin.
constexpr u32 ORRE_MENU_GLOBAL_LINES[][2] = {
    {0x044349EC, 0x00000001},  // player layout = GBA vs GBA. The layout global IS the mode row:
                               // 0 = GC vs GBA, 1 = GBA vs GBA, 2/3 = tag (>= 2 forces tag at
                               // commit). Pinning 0 held the row on GC vs GBA and per-frame
                               // reverted any attempt to select GBA vs GBA -- the field bug.
                               // Value 1 verified against the layout table at 0x802EAA70 (ports
                               // {2,3}, matching this fork's SI config exactly) and the mode
                               // label atlas; layout 1 also skips the GC-pad presence gates, so
                               // no new refusal dialog becomes reachable.
    {0x044349FC, 0x00000003},  // rules choice = Custom 1 (save-backed slot the getter
                               // returns DIRECTLY for selections 3..5 -- the pin holds)
};
// Battle type is per SHAPE, so it is emitted separately from the shared
// globals above: Orre shapes battle Double (1 -> ctx+8 = 1 -> record 7),
// Hoenn shapes battle SINGLE (0 -- the constructor default, verified against
// the commit path: it composes cleanly with layout 1 and the Custom-1 pin,
// and the tag-forcing override only fires for layout >= 2).
constexpr u32 BATTLE_TYPE_GLOBAL = 0x044349F0;
constexpr u32 ORRE_RULESET_BASE = 0x044334C0;  // Custom 1: 0x80433310 + 3*144
// Stock preset slots with two deliberate edits applied at emit time / below:
// the entries word (index 6) is still at the STOCK value 6 -- the emitter
// patches its low half per format -- and word 7's high u16 (+0x1C, the ENTRY
// MODE) is zeroed from the stock 1. Entry mode is the decoded reason
// "entries" never bit: the effective-entries getter (0x8004cfe0) returns 6
// whenever +0x1C != 0 and only reads +0x1A (entries) when it is 0, while the
// rules SCREEN prints +0x1A either way -- hence the 1.5.0 field bug "shows 4,
// plays 6v6". With +0x1C = 0 the game's own pick-N flow runs and the whole
// chain enforces N (getter -> entry validator 0x8004b14c -> init 0x8004b210
// -> GBA reply handler -> marshaller), matching what the game's own
// default-custom builder writes (li 4 / sth +0x1A, li 0 -> +0x1C).
// Clause bytes (+0x0C..+0x13) are stock and were decoded: Species, Item,
// Sleep, Freeze and BOTH Self-KO halves are all ON in these values -- the
// game itself enforces them, no honor system needed. The per-item ban flags
// (+0x52..+0x71, all 0) CANNOT ban Soul Dew: the 32-entry bannable-item
// table (0x8032EB08) does not contain item 191 at all, and the game's
// hardcoded Soul Dew check lives only in the default-rules builder that
// custom slots bypass -- so Soul Dew stays app-side-enforced (the paste/
// host/submission gates), by structural necessity.
// Only words 0-1 differ between the two levels.
constexpr u32 RULESET_LV100_WORDS[36] = {
    0x00010064, 0x02580006, 0x00000002, 0x00000000, 0x01010001, 0xFFC4FFEC,
    0x01010006, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
};
constexpr u32 RULESET_LV50_WORDS[36] = {
    0x00010032, 0x012C0006, 0x00000002, 0x00000000, 0x01010001, 0xFFC4FFEC,
    0x01010006, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
};
constexpr int RULESET_ENTRIES_WORD = 6;  // u16 at +0x1A = low half of word 6

std::string FormatRuleLines()
{
  // Sleep/Freeze/Self-KO clauses ride the stock clause bytes in the preset
  // words above (all decoded ON -- the game itself enforces them);
  // species/item/ban clauses are enforced app-side by FormatRules. This pins
  // only what the in-game rules screen controls -- Double, the level preset,
  // and the entry count -- via the game's own tournament presets (see above).
  // The format matrix: Standard/Unlimited pin Lv100 (they differ only in the
  // app-side legality layer), Limited pins Lv50; Orre shapes enter 4, Hoenn
  // shapes enter 3.
  const int format = Config::Get(Config::MAIN_XD_FORMAT);
  const u32* words = nullptr;
  u32 entries = 0;
  u32 battle_type = 1;  // Double (Orre shapes); Hoenn shapes override to Single
  switch (format)
  {
  case FormatRules::FORMAT_ORRE_COLOSSEUM:
  case FormatRules::FORMAT_ORRE_UNLIMITED:
    words = RULESET_LV100_WORDS;
    entries = 4;
    break;
  case FormatRules::FORMAT_ORRE_LIMITED:
    words = RULESET_LV50_WORDS;
    entries = 4;
    break;
  case FormatRules::FORMAT_HOENN_STADIUM:
  case FormatRules::FORMAT_HOENN_UNLIMITED:
    words = RULESET_LV100_WORDS;
    entries = 3;
    battle_type = 0;  // Single
    break;
  case FormatRules::FORMAT_HOENN_LIMITED:
    words = RULESET_LV50_WORDS;
    entries = 3;
    battle_type = 0;  // Single
    break;
  default:
    return {};  // Free / OU / unknown: no rules pin, sessions stay stock
  }

  std::string lines;
  for (const auto& line : ORRE_MENU_GLOBAL_LINES)
    AppendLine(&lines, line[0], line[1]);
  AppendLine(&lines, BATTLE_TYPE_GLOBAL, battle_type);
  for (size_t i = 0; i < 36; i++)
  {
    u32 word = words[i];
    if (i == RULESET_ENTRIES_WORD)
      word = (word & 0xFFFF0000u) | entries;
    AppendLine(&lines, ORRE_RULESET_BASE + static_cast<u32>(4 * i), word);
  }
  return lines;
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
    s_cheats_before.reset();
    s_block_active = false;
    s_netplay_session = true;
  }
  // All-default = pure removal of anything a crashed session left behind.
  RegenerateIni(Selection{}, /*ou_enabled=*/true, nullptr, /*include_format_rules=*/false);
}

void ScrubLeftovers()
{
  // Same stance as the file heals: while emulation runs, whatever is stashed
  // belongs to the LIVE boot -- most importantly s_cheats_before, which the
  // core-state hook's EndSession needs intact to restore the cheats flag when
  // that boot ends. (Desktop hits this for real: the launcher's Host/Join
  // buttons run EnsureGbaConfig -> here BEFORE MainWindow refuses because a
  // game is running.) Every caller is a recurring boundary, so standing down
  // only delays the scrub.
  if (!Core::IsUninitialized(Core::System::GetInstance()))
    return;
  {
    std::lock_guard lock(s_mutex);
    // A live room owns the stashes and the INI block -- nothing is a leftover.
    if (s_netplay_session)
      return;
    s_guest_model.reset();
    s_cheats_before.reset();
    s_block_active = false;
  }
  RegenerateIni(Selection{}, /*ou_enabled=*/true, nullptr, /*include_format_rules=*/false);
}

bool IsNetplaySessionActive()
{
  std::lock_guard lock(s_mutex);
  return s_netplay_session;
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

// The bust class (1..6) of the save that will occupy GBA port |device|+1, or
// 0 when it cannot be determined (no ROM, unreadable save, FRLG-refused build,
// non-mgba build). Same ROM fallback as TeamInjector: the socket's own ROM,
// then the other socket's.
static int SaveBustClass(int device)
{
#ifdef HAS_LIBMGBA
  std::string rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device]);
  if (rom.empty() || !File::Exists(rom))
    rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device == 1 ? 2 : 1]);
  if (rom.empty() || !File::Exists(rom))
    return 0;
  const std::string path = HW::GBA::Core::GetSavePath(rom, device);
  File::IOFile file(path, "rb");
  if (!file)
    return 0;
  std::vector<u8> bytes(file.GetSize());
  if (!file.ReadBytes(bytes.data(), bytes.size()))
    return 0;
  std::string error;
  const auto save = EmeraldSave::Create(std::move(bytes), &error);
  if (!save)
    return 0;
  const u32 gender = save->GetTrainerGender();
  if (gender > 1)
    return 0;
  switch (EmeraldSave::DetectGame(*save))
  {
  case Gen3Game::FireRedLeafGreen:
    return 1 + static_cast<int>(gender);
  case Gen3Game::RubySapphire:
    return 3 + static_cast<int>(gender);
  case Gen3Game::Emerald:
    return 5 + static_cast<int>(gender);
  }
  return 0;
#else
  (void)device;
  return 0;
#endif
}

bool RegenerateIni(const Selection& sel, bool ou_enabled, std::string* status,
                   bool include_format_rules)
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
  // A format rules-pin implies GBA-vs-GBA, where the preview's default
  // protagonist busts are always wrong -- have the block hide them.
  const bool format_pinned =
      include_format_rules && FormatRules::HasTeamRules(Config::Get(Config::MAIN_XD_FORMAT));
  std::string block = GenerateCodeBlock(
      sel.host_model > 0 ? std::optional<int>(sel.host_model) : std::nullopt, guest_model,
      sel.music > 0 ? std::optional<int>(sel.music) : std::nullopt,
      sel.venue > 0 ? std::optional<int>(sel.venue) : std::nullopt,
      SaveBustClass(1), SaveBustClass(2), format_pinned);
  // FORMAT seam: FormatRuleLines() contributes the picked format's in-game
  // rule pins (menu globals, battle type, Custom-1 ruleset); Free/OU return
  // "" and an all-default session stays byte-for-byte stock.
  if (include_format_rules)
  {
    if (const std::string format_lines = FormatRuleLines(); !format_lines.empty())
    {
      if (!block.empty())
        block.push_back('\n');
      block += format_lines;
    }
  }
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
        // Validity, not just non-zero: a stored pick that left the catalog
        // (curated-out music, say) generates NO lines, and the log claiming it
        // was live sent a whole debugging session sideways once.
        IsValidMusicId(sel.music) ? fmt::format("{}", sel.music) :
        sel.music > 0             ? fmt::format("default (stored {} not offered)", sel.music) :
                                    std::string{"default"},
        IsValidVenueId(sel.venue) ? fmt::format("{}", sel.venue) :
        sel.venue > 0             ? fmt::format("default (stored {} not offered)", sel.venue) :
                                    std::string{"default"},
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
  // The OU choice is the Format dropdown now (the old standalone toggle is
  // gone), so it reads straight from the format key -- PrepareForStart's
  // cheats reconciliation can no longer distort it, however many Starts a
  // room sees.
  const bool ou_enabled = Config::Get(Config::MAIN_XD_FORMAT) == FormatRules::FORMAT_OU;
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
  {
    std::lock_guard lock(s_mutex);
    active = s_block_active;
  }
  // MAIN_ENABLE_CHEATS is DERIVED state for an XD session: the old standalone
  // "OU Fixes" toggle became the OU entry of the Format dropdown, so nothing
  // user-facing writes this flag for XD any more. The AR engine (and netplay's
  // SyncCodes, via the SetupNetSettings snapshot taken after this hook) must
  // be ON when anything of ours has to load -- the style/rules block, or the
  // Sys-bundled OU Fixes code in OU format -- and OFF otherwise, INCLUDING
  // when a stale true is left over from the removed toggle: without the
  // off-direction, "Free" would silently keep shipping OU patches the UI can
  // no longer show or clear. The pre-session value is remembered once and
  // EndSession writes it back, so a desktop user's global cheats preference
  // for other games survives XD sessions unchanged. Ordering stays safe in
  // both directions: the regenerate above already wrote the INI for exactly
  // this format (OU on -> no local disable; otherwise the block, when active,
  // carries the "$XD OU Fixes" disable).
  const bool ou = Config::Get(Config::MAIN_XD_FORMAT) == FormatRules::FORMAT_OU;
  const bool need_cheats = active || ou;
  if (Config::Get(Config::MAIN_ENABLE_CHEATS) != need_cheats)
  {
    {
      std::lock_guard lock(s_mutex);
      if (!s_cheats_before)
        s_cheats_before = !need_cheats;
    }
    Config::SetBaseOrCurrent(Config::MAIN_ENABLE_CHEATS, need_cheats);
    LogNote(fmt::format("battlestyle cheats {} for this session (format={} block={})",
                        need_cheats ? "on" : "off",
                        FormatRules::FormatDisplayName(Config::Get(Config::MAIN_XD_FORMAT)),
                        active ? "active" : "empty"));
  }
}

void EndSession()
{
  std::optional<bool> cheats_before;
  {
    std::lock_guard lock(s_mutex);
    s_guest_model.reset();
    cheats_before = s_cheats_before;
    s_cheats_before.reset();
    s_netplay_session = false;
  }
  // Pure removal: with the stash cleared and an all-default selection this
  // strips the block, its enabled line and the OU-Fixes disable, leaving the
  // user's GXXE01.ini as it was before the session.
  RegenerateIni(Selection{}, /*ou_enabled=*/true, nullptr, /*include_format_rules=*/false);
  // Put the cheats flag back the way the user had it before the session's
  // reconciliation (see PrepareForStart): the flag is only DERIVED state while
  // an XD session runs, and a desktop user's global preference for other games
  // must survive it.
  if (cheats_before)
  {
    // Base explicitly, not SetBaseOrCurrent: a mid-game room close runs this
    // while the Netplay config layer still holds MAIN_ENABLE_CHEATS, and
    // SetBaseOrCurrent would then route the restore into the CurrentRun layer
    // -- which BootManager::RestoreConfig wipes moments later, stranding the
    // reconciled value in Base forever. The pre-session value was captured
    // FROM Base (PrepareForStart runs before any boot layers exist), so Base
    // is exactly where it belongs; the still-running game never sees the
    // write because the Netplay layer keeps overriding until teardown.
    Config::Set(Config::LayerType::Base, Config::MAIN_ENABLE_CHEATS, *cheats_before);
    Config::Save();
  }
#ifdef HAS_LIBMGBA
  // The session's log is closed by the time cleanup runs; append like the team
  // purge does, so the same file also records that the block was taken out.
  GBADetectLog::LogPostSession(
      fmt::format("battlestyle cleanup done cheats_restored={}", cheats_before ? 1 : 0));
#endif
}
}  // namespace XDNetplay::BattleCustomizer
