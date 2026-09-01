// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/XDNetplay/XDLauncherDialog.h"

#include <array>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <QAbstractItemModel>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/HW/GBACore.h"

#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/XDNetplay/TeamEditorDialog.h"
#include "DolphinQt/XDNetplay/XDNetplayConfig.h"

#include "UICommon/GameFile.h"
#include "UICommon/NetPlayIndex.h"
#include "UICommon/XDNetplay/BattleCustomizer.h"
#include "UICommon/XDNetplay/DisposableSave.h"
#include "UICommon/XDNetplay/FormatRules.h"
#include "UICommon/XDNetplay/Gen3Save.h"
#include "UICommon/XDNetplay/SaveImport.h"

namespace
{
// A word-wrapped explanatory line that can never be squashed below its text.
// QLabel::minimumSizeHint() for a wrapping label is ONE line tall, so with the
// default Preferred policy a window shorter than the layout's preferred height
// shrinks the note's row and the text paints clipped (the field report: "have
// to stretch the window to read it"). Minimum/Minimum -- the pattern upstream
// uses in ConvertDialog and AchievementBox -- makes the layout's minimum equal
// the wrapped sizeHint instead, so it wraps correctly at any window size.
QLabel* MakeNoteLabel(const QString& text)
{
  auto* label = new QLabel(text);
  label->setWordWrap(true);
  label->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
  return label;
}

constexpr size_t GBA_HEADER_TITLE_OFFSET = 0xA0;
constexpr size_t GBA_HEADER_FIXED_OFFSET = 0xB2;
constexpr u8 GBA_HEADER_FIXED_VALUE = 0x96;
constexpr std::string_view EMERALD_TITLE_PREFIX = "POKEMON EMER";

// Cheap header check used by the manual picker: any GBA ROM will do there,
// the user explicitly chose the file.
bool HasGbaHeader(const std::string& path)
{
  File::IOFile rom(path, "rb");
  u8 fixed_value = 0;
  return rom && rom.Seek(GBA_HEADER_FIXED_OFFSET, File::SeekOrigin::Begin) &&
         rom.ReadBytes(&fixed_value, sizeof(fixed_value)) && fixed_value == GBA_HEADER_FIXED_VALUE;
}

// Strict check used by auto-discovery so a random GBA ROM sitting next to the
// ISO is not grabbed: valid header byte plus a "POKEMON EMER" internal title.
bool IsEmeraldRom(const std::string& path)
{
  File::IOFile rom(path, "rb");
  std::array<char, 12> title{};
  if (!rom || !rom.Seek(GBA_HEADER_TITLE_OFFSET, File::SeekOrigin::Begin) ||
      !rom.ReadBytes(title.data(), title.size()))
  {
    return false;
  }
  if (!std::string_view(title.data(), title.size()).starts_with(EMERALD_TITLE_PREFIX))
    return false;

  u8 fixed_value = 0;
  return rom.Seek(GBA_HEADER_FIXED_OFFSET, File::SeekOrigin::Begin) &&
         rom.ReadBytes(&fixed_value, sizeof(fixed_value)) && fixed_value == GBA_HEADER_FIXED_VALUE;
}

bool EmeraldRomConfigured()
{
  const std::string rom_path = Config::Get(Config::MAIN_GBA_ROM_PATHS[1]);
  return !rom_path.empty() && File::Exists(rom_path);
}

// Import an Emerald dump: copy it into the GBA user directory and point both
// GBA ports at the copy. Shared by the manual picker and auto-discovery.
bool ImportEmeraldRom(const std::string& source)
{
  const std::string destination = File::GetUserPath(D_GBAUSER_IDX) + "EMERALD.gba";
  if (source != destination)
  {
    File::CreateFullPath(destination);
    if (!File::CopyRegularFile(source, destination))
      return false;
  }
  Config::SetBaseOrCurrent(Config::MAIN_GBA_ROM_PATHS[1], destination);
  Config::SetBaseOrCurrent(Config::MAIN_GBA_ROM_PATHS[2], destination);
  Config::Save();
  return true;
}

bool TeamSavesInstalled()
{
#ifdef HAS_LIBMGBA
  const std::string rom2 = Config::Get(Config::MAIN_GBA_ROM_PATHS[1]);
  const std::string rom3 = Config::Get(Config::MAIN_GBA_ROM_PATHS[2]);
  return !rom2.empty() && !rom3.empty() && File::Exists(HW::GBA::Core::GetSavePath(rom2, 1)) &&
         File::Exists(HW::GBA::Core::GetSavePath(rom3, 2));
#else
  return false;
#endif
}

#ifdef HAS_LIBMGBA
// The save file GBA socket |device| (1 = port 2, 2 = port 3) boots from, or
// empty while no ROM is configured. Same ROM fallback as the team editor and
// TeamInjector: the socket's own ROM, then the other socket's (a
// launcher-made setup points both ports at one Emerald dump).
std::string SaveSlotPath(int device)
{
  std::string rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device]);
  if (rom.empty() || !File::Exists(rom))
    rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device == 1 ? 2 : 1]);
  if (rom.empty() || !File::Exists(rom))
    return {};
  return HW::GBA::Core::GetSavePath(rom, device);
}
#endif

bool VsSaveInstalled()
{
  return File::Exists(File::GetUserPath(D_GCUSER_IDX) + "USA/Card A/01-GXXE-PokemonXD.gci");
}

void SetRowState(QLabel* status, bool ok)
{
  status->setText(ok ? QStringLiteral("✔") : QStringLiteral("✘"));
  status->setStyleSheet(ok ? QStringLiteral("QLabel { color: green; }") :
                             QStringLiteral("QLabel { color: red; }"));
}

// Traversal-server settings shared by hosting and joining.
void PrepareNetplayConfig()
{
  XDNetplay::EnsureGbaConfig();
  Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE, "traversal");
  if (Config::Get(Config::NETPLAY_NICKNAME).empty())
    Config::SetBaseOrCurrent(Config::NETPLAY_NICKNAME, "Player");
}

// Hosting only: publish the room to Dolphin's public session index.
//
// This closes a hole that made the whole desktop lobby look empty. The XD
// launcher's Host button used to write nothing but the traversal settings, and
// NETPLAY_USE_INDEX defaults to false -- so NetPlayServer::SetupIndex()
// early-returned and every desktop host was invisible both in the session
// browser and to the auto-matcher, while Android hosts (which do set the key,
// in FindBattlesActivity.hostPublicBattle) showed up fine. Desktop now matches
// Android.
//
// SetupIndex() also refuses to publish with an empty region, so fall back to
// "NA" when the user has never picked one. The region is only ever a browser
// filter and the matcher deliberately ignores it: a player base this small,
// split seven ways by continent, would never find each other.
//
// |open_challenge| clears any index password. The launcher has no password UI,
// so a leftover password could only come from the NetPlay Browser's publish
// panel -- worth respecting for a deliberate Host click, but an auto-matched
// room must be joinable by a stranger (and the matcher skips password rooms
// anyway, so leaving one set would make the host unmatchable by its own
// opponents).
//
// Timing worth knowing when debugging "my room is not findable": SetupIndex()
// runs from the NetPlayServer constructor, BEFORE MainWindow::NetPlayHost gets
// to ChangeGame(), so the very first publish carries game_id "UNKNOWN" and
// LooksLikeXdSession rejects it. NetPlayServer's ping loop pushes the real
// name through NetPlayIndex::SetGame() within ~1s and the index's own
// notification loop uploads it every 5s, so a room becomes matchable a few
// seconds after it opens -- irrelevant when waiting minutes for an opponent,
// but it does mean two clients searching in the same instant can both end up
// hosting. Searching again resolves it.
void PreparePublishConfig(bool open_challenge)
{
  Config::SetBaseOrCurrent(Config::NETPLAY_USE_INDEX, true);
  // The "[Orre] " / "[OU] " room tags ride the published name from the host's
  // Format pick -- labels only, no matchmaking filter change; with Format =
  // Free the name is byte-identical to before the feature.
  Config::SetBaseOrCurrent(
      Config::NETPLAY_INDEX_NAME,
      XDNetplay::MakeOpenSessionName(Config::Get(Config::NETPLAY_NICKNAME),
                                     Config::Get(Config::MAIN_XD_FORMAT)));
  if (Config::Get(Config::NETPLAY_INDEX_REGION).empty())
    Config::SetBaseOrCurrent(Config::NETPLAY_INDEX_REGION, "NA");
  if (open_challenge)
    Config::SetBaseOrCurrent(Config::NETPLAY_INDEX_PASSWORD, "");
}

// Pick the room "Search for Match" should join, or nothing to host instead.
//
// The rules are deliberately dumb so that both platforms reach the same answer
// and a failed match is explainable:
//   * same build -- netplay only connects between identical git hashes
//     (GetScmRevGitStr), and the index keys on GetScmDescStr, so a foreign
//     build's room is unjoinable by construction. The server-side filter
//     already asks for this; re-checking costs nothing and the index is a
//     third-party service we do not control.
//   * looks like Pokemon XD (see XDNetplay::LooksLikeXdSession -- the index's
//     "game" field is a display name, not a game id).
//   * exactly one player waiting. Zero would be a room mid-teardown; two is a
//     battle already under way, and XD's link is strictly two-player.
//   * not in game, not password protected.
//   * first hit in index order wins. The index returns rooms in the order they
//     registered, so this is "join whoever has been waiting longest" -- and,
//     more importantly, it is deterministic, which makes a bad match
//     reproducible instead of a coin flip.
std::optional<NetPlaySession> PickMatch(const std::vector<NetPlaySession>& sessions)
{
  for (const NetPlaySession& session : sessions)
  {
    if (session.version != Common::GetScmDescStr())
      continue;
    if (!XDNetplay::LooksLikeXdSession(session.game_id))
      continue;
    if (session.player_count != 1 || session.in_game || session.has_password)
      continue;
    return session;
  }
  return std::nullopt;
}

// Battlefield indices whose rooms are outdoor / cave / water rather than a
// colosseum floor. Terrain is the one way the location pick is not purely
// cosmetic: Nature Power's move, Camouflage's type and Secret Power's side
// effect all resolve from it (and Secret Power is a common Gen 3 TM). Per the
// venue research these entries get a suffix in their dropdown label -- no
// separate warning dialog. Keep in sync with BattleCustomizer's VenueTable.
constexpr int TERRAIN_VENUES[] = {5,  7,  10, 12, 14, 15, 16, 17, 19, 21, 22, 23, 39,
                                  42, 44, 45, 49, 50, 53, 54, 55, 56, 57, 58, 59};

bool VenueAltersTerrain(int id)
{
  for (const int terrain_id : TERRAIN_VENUES)
  {
    if (terrain_id == id)
      return true;
  }
  return false;
}
}  // namespace

XDLauncherDialog::XDLauncherDialog(const GameListModel& game_list_model, QWidget* parent)
    : QDialog(parent), m_game_list_model(game_list_model)
{
  setWindowTitle(tr("OrreLink Launcher"));

  CreateMainLayout();
  ConnectWidgets();
}

void XDLauncherDialog::CreateMainLayout()
{
  auto* layout = new QVBoxLayout;

  auto* checklist_box = new QGroupBox(tr("Setup Checklist"));
  auto* checklist_layout = new QGridLayout;
  int row = 0;
  const auto add_row = [&](ChecklistRow* target, const QString& description,
                           const QString& fix_label) {
    target->status = new QLabel(QStringLiteral("✘"));
    target->description = new QLabel(description);
    target->fix_button = new NonDefaultQPushButton(fix_label);
    checklist_layout->addWidget(target->status, row, 0);
    checklist_layout->addWidget(target->description, row, 1);
    checklist_layout->addWidget(target->fix_button, row, 2);
    row++;
  };
  add_row(&m_game_row, tr("Pokémon XD (USA) — choose your XD ISO file"), tr("Choose ISO..."));
  add_row(&m_rom_row, tr("Emerald ROM configured"), tr("Choose ROM..."));
  add_row(&m_bios_row, tr("Official GBA BIOS"), tr("Choose BIOS..."));
  add_row(&m_team_saves_row, tr("Team saves installed"), tr("Install"));
  add_row(&m_vs_save_row, tr("XD VS-mode save (memory card)"), tr("Install"));
  add_row(&m_gba_input_row, tr("GBA controls"), tr("Use defaults"));
  checklist_layout->setColumnStretch(1, 1);
  checklist_box->setLayout(checklist_layout);
  layout->addWidget(checklist_box);

  // Save Files: which Gen 3 save each GBA socket boots from. The bundled
  // team-editor save stays the default; importing your own cartridge save is
  // opt-in and per port (SaveImport does the validation and the no-destruction
  // bookkeeping). The caption is a permanent fixture rather than a popup
  // because it is the two facts every importer must know BEFORE picking a
  // file: netplay syncs the HOST's saves (so a joiner's import never reaches
  // the room -- joiners submit their team instead), and a hosting session
  // never syncs the import itself, only the rebuilt party-and-identity save
  // DisposableSave swaps in for the room's duration.
  auto* saves_box = new QGroupBox(tr("Save Files"));
  auto* saves_layout = new QGridLayout;
  auto* saves_caption = MakeNoteLabel(
      tr("Solo play uses an imported save exactly as it is. Hosting a netplay room does NOT "
         "share the file: the session runs on a rebuilt save carrying only your party and "
         "trainer identity, and your own save returns when the room closes. When you join "
         "someone else's room, use Submit Team instead."));
  saves_layout->addWidget(saves_caption, 0, 0, 1, 4);
  int save_row = 1;
  const auto add_save_row = [&](SaveSlotRow* target, const QString& label) {
    target->state = MakeNoteLabel(QString());
    target->import_button = new NonDefaultQPushButton(tr("Import my save..."));
    target->restore_button = new NonDefaultQPushButton(tr("Restore default"));
    target->restore_button->setToolTip(
        tr("Puts the bundled team-editor save back. The replaced save is kept next to it as a "
           ".bak file."));
    saves_layout->addWidget(new QLabel(label), save_row, 0);
    saves_layout->addWidget(target->state, save_row, 1);
    saves_layout->addWidget(target->import_button, save_row, 2);
    saves_layout->addWidget(target->restore_button, save_row, 3);
    save_row++;
  };
  add_save_row(&m_save_row_port2, tr("Your save (GBA port 2):"));
  add_save_row(&m_save_row_port3, tr("Guest-slot save (GBA port 3):"));
  saves_layout->setColumnStretch(1, 1);
  saves_box->setLayout(saves_layout);
  layout->addWidget(saves_box);

  auto* battle_box = new QGroupBox(tr("Battle"));
  auto* battle_layout = new QGridLayout;
  m_boot_button = new NonDefaultQPushButton(tr("Boot Pokémon XD (solo)"));
  m_practice_dummy_check = new QCheckBox(tr("Practice vs dummy"));
  m_practice_dummy_check->setToolTip(
      tr("Auto-plays GBA port 3 so you can practice link battles alone."));
  // The headline action: one button, no codes to trade. Everything below it is
  // the manual escape hatch for people who already know their opponent.
  m_search_button = new NonDefaultQPushButton(tr("Search for Match"));
  m_search_button->setToolTip(tr("Looks for someone already waiting and joins them. "
                                 "If nobody is waiting, hosts a public room and waits "
                                 "for an opponent."));
  m_host_button = new NonDefaultQPushButton(tr("Host Battle"));
  m_join_code_edit = new QLineEdit;
  m_join_code_edit->setPlaceholderText(tr("Host code"));
  m_join_button = new NonDefaultQPushButton(tr("Join Battle"));
  m_browse_button = new NonDefaultQPushButton(tr("Browse Public Sessions"));
  m_team_editor_button = new NonDefaultQPushButton(tr("Team Editor..."));
  // Empty until a search runs, so the dialog does not grow a permanently blank
  // row; word wrap because the "hosting and waiting" line is a sentence.
  m_match_status = MakeNoteLabel(QString());
  m_match_status->hide();
  battle_layout->addWidget(m_boot_button, 0, 0);
  battle_layout->addWidget(m_practice_dummy_check, 0, 1);
  battle_layout->addWidget(m_search_button, 1, 0, 1, 2);
  battle_layout->addWidget(m_match_status, 2, 0, 1, 2);
  battle_layout->addWidget(m_host_button, 3, 0, 1, 2);
  battle_layout->addWidget(m_join_code_edit, 4, 0);
  battle_layout->addWidget(m_join_button, 4, 1);
  battle_layout->addWidget(m_browse_button, 5, 0, 1, 2);
  battle_layout->addWidget(m_team_editor_button, 6, 0, 1, 2);
  battle_box->setLayout(battle_layout);
  layout->addWidget(battle_box);

  // Battle Style: picks the HOST makes. All but the first are purely cosmetic:
  // BattleCustomizer turns them into one synced AR code at Start, so both
  // players always see the same thing; "Game default" genuinely emits nothing
  // and an all-default session stays byte-for-byte stock. Music and venue
  // entries after each list's separator are valid but untested in battle --
  // their labels say so; venues whose terrain changes a few moves carry that
  // in the label too, no extra dialogs. MODELS are field-proven, so their
  // lists carry no tier presentation at all: instead, models without a
  // pre-rendered bust say "(no portrait)" in their label -- that suffix is the
  // whole explanation (a separate note was removed on community feedback).
  //
  // The first row is the battle FORMAT (v1.4.0) -- the one pick that is not
  // cosmetic. It is persisted in MAIN_XD_FORMAT exactly like the style keys;
  // FormatRules' gates (host-party check before a room opens, guest-submission
  // check on arrival) do the enforcing, never this dropdown.
  auto* style_box = new QGroupBox(tr("Battle Style"));
  auto* style_layout = new QGridLayout;
  int style_row = 0;
  {
    // Who controls what is a recurring question: music/location are HOST
    // picks (they ship in the host-assembled session code both players run);
    // models are PER PLAYER (a joiner picks theirs in the Submit Team
    // sheet, and that pick beats the host's fallback dropdown).
    auto* who_note = MakeNoteLabel(
        tr("The host picks format, music and location. Joiners set team, name and model in "
           "Submit Team; hosts set their name in the room or Team Editor."));
    style_layout->addWidget(who_note, style_row, 0, 1, 2);
    style_row++;
  }
  const auto add_style_combo = [&](const QString& label, const QString& tooltip) {
    auto* description = new QLabel(label);
    auto* combo = new QComboBox;
    if (!tooltip.isEmpty())
    {
      description->setToolTip(tooltip);
      combo->setToolTip(tooltip);
    }
    style_layout->addWidget(description, style_row, 0);
    style_layout->addWidget(combo, style_row, 1);
    style_row++;
    return combo;
  };
  m_format_combo = add_style_combo(
      tr("Format:"),
      tr("Applies when you host: your room plays under this ruleset.\n\n"
         "The community formats are three RULESETS times two ENTRY SHAPES:\n\n"
         "Orre Colosseum / Hoenn Stadium — the canon ruleset (Lv 100):\n"
         "  • Gen 1–3 species, except Mewtwo, Mew, Lugia, Ho-Oh, Celebi,\n"
         "    Kyogre, Groudon, Rayquaza, Jirachi and Deoxys\n"
         "  • Species Clause and Item Clause — no duplicates\n"
         "  • Soul Dew is banned\n"
         "Unlimited — everything allowed, Soul Dew included; the clauses\n"
         "still apply. Lv 100.\n"
         "Limited — LEVEL 50 (Pokémon above Lv 50 cannot enter) and ALL\n"
         "legendaries banned, the birds, beasts, Regis and Latis included.\n\n"
         "Orre shapes: bring 6, pick 4.  Hoenn shapes: bring 6, pick 3.\n"
         "The in-game rules screen is pre-set to match, and teams are checked\n"
         "before a room opens (yours and the port-3 fallback) and on every\n"
         "guest submission. Sleep, Freeze, Self-KO, Species and Item clauses\n"
         "are enforced by the game itself — the pinned ruleset has them on.\n\n"
         "OU — runs the community $XD OU Fixes patches for both players\n"
         "(bring-6-pick-4 and its mechanics fixes). No team restrictions.\n\n"
         "Free — no patches, no restrictions; sessions are exactly as before\n"
         "this option existed. When you JOIN a room, the host's Format\n"
         "applies, not yours; your own pick still gives you legality notes in\n"
         "the Team Editor and the Submit Team dialog."));
  // The entry labels come from FormatRules so the dropdown, the refusal
  // dialogs and the room-chat messages all name the formats identically.
  // Dropdown order mirrors FormatBridge.ALL_FORMATS on Android: Free, the
  // Orre family, the Hoenn family, then OU.
  for (const int format_id :
       {XDNetplay::FormatRules::FORMAT_FREE, XDNetplay::FormatRules::FORMAT_ORRE_COLOSSEUM,
        XDNetplay::FormatRules::FORMAT_ORRE_UNLIMITED, XDNetplay::FormatRules::FORMAT_ORRE_LIMITED,
        XDNetplay::FormatRules::FORMAT_HOENN_STADIUM,
        XDNetplay::FormatRules::FORMAT_HOENN_UNLIMITED,
        XDNetplay::FormatRules::FORMAT_HOENN_LIMITED, XDNetplay::FormatRules::FORMAT_OU})
  {
    m_format_combo->addItem(
        QString::fromUtf8(XDNetplay::FormatRules::FormatDisplayName(format_id)), format_id);
  }
  const auto populate_style_combo =
      [this](QComboBox* combo, std::span<const XDNetplay::BattleCustomizer::StyleOption> table,
             bool model_table, bool terrain_suffix) {
        using XDNetplay::BattleCustomizer::Tier;
        combo->addItem(tr("Game default"), 0);
        bool past_tested = false;
        for (const XDNetplay::BattleCustomizer::StyleOption& option : table)
        {
          // Music/venue tables list tested-safe entries first; the tier change
          // is where the "here be dragons" separator goes. Model lists get no
          // tier presentation (the field has disproven "untested" there):
          // their one distinction is whether the pick has a pre-rendered bust,
          // said in the label as "(no portrait)".
          if (!model_table && !past_tested && option.tier == Tier::Experimental)
          {
            combo->insertSeparator(combo->count());
            past_tested = true;
          }
          const QString name = QString::fromUtf8(option.name);
          const bool untested = !model_table && option.tier == Tier::Experimental;
          const bool no_portrait =
              model_table && !XDNetplay::BattleCustomizer::ModelHasPortrait(option.id);
          const bool terrain = terrain_suffix && VenueAltersTerrain(option.id);
          QString item;
          if (untested && terrain)
            item = tr("%1 (untested, alters Nature Power etc.)").arg(name);
          else if (untested)
            item = tr("%1 (untested)").arg(name);
          else if (terrain)
            item = tr("%1 (alters Nature Power etc.)").arg(name);
          else if (no_portrait)
            item = tr("%1 (no portrait)").arg(name);
          else
            item = name;
          combo->addItem(item, option.id);
        }
      };
  m_style_host_model_combo = add_style_combo(
      tr("Your model:"), tr("The trainer model you appear as. Applies when you host;\n"
                            "both players see the same battle."));
  m_style_guest_model_combo = add_style_combo(
      tr("Guest model (used only if they don't pick):"),
      tr("Fallback only: a model the guest picks when submitting\n"
         "their team always wins over this."));
  m_style_music_combo = add_style_combo(tr("Battle music:"), QString());
  m_style_venue_combo = add_style_combo(
      tr("Battle location:"), tr("Locations that alter Nature Power, Camouflage or Secret Power\n"
                                 "say so in their name -- everything else is purely cosmetic."));
  populate_style_combo(m_style_host_model_combo, XDNetplay::BattleCustomizer::ModelTable(), true,
                       false);
  populate_style_combo(m_style_guest_model_combo, XDNetplay::BattleCustomizer::ModelTable(), true,
                       false);
  populate_style_combo(m_style_music_combo, XDNetplay::BattleCustomizer::MusicTable(), false,
                       false);
  populate_style_combo(m_style_venue_combo, XDNetplay::BattleCustomizer::VenueTable(), false, true);
  style_layout->setColumnStretch(1, 1);
  style_box->setLayout(style_layout);
  layout->addWidget(style_box);

  // Same setting the hub's own checkbox writes. Reworded because the hub, not
  // this launcher, is what actually opens at startup now -- a checkbox that
  // names the wrong window is worse than no checkbox.
  m_show_on_startup_check = new QCheckBox(tr("Show the Pokémon Hub at startup"));
  m_show_on_startup_check->setChecked(ShowOnStartup());
  layout->addWidget(m_show_on_startup_check);

  setLayout(layout);
}

void XDLauncherDialog::ConnectWidgets()
{
  // The game scan is asynchronous: without this the XD row stays red until the
  // user reopens the dialog, even though the pick succeeded.
  connect(&m_game_list_model, &QAbstractItemModel::rowsInserted, this,
          [this] { RefreshChecklist(); });
  connect(&m_game_list_model, &QAbstractItemModel::modelReset, this,
          [this] { RefreshChecklist(); });

  connect(m_game_row.fix_button, &QPushButton::clicked, this, &XDLauncherDialog::OnFixGamePath);
  connect(m_rom_row.fix_button, &QPushButton::clicked, this, &XDLauncherDialog::OnFixEmeraldRom);
  connect(m_bios_row.fix_button, &QPushButton::clicked, this, &XDLauncherDialog::OnFixBios);
  connect(m_gba_input_row.fix_button, &QPushButton::clicked, this,
          &XDLauncherDialog::OnFixGbaInput);
  connect(m_team_saves_row.fix_button, &QPushButton::clicked, this,
          &XDLauncherDialog::OnFixTeamSaves);
  connect(m_vs_save_row.fix_button, &QPushButton::clicked, this, &XDLauncherDialog::OnFixVsSave);

  connect(m_save_row_port2.import_button, &QPushButton::clicked, this,
          [this] { OnImportSave(1); });
  connect(m_save_row_port3.import_button, &QPushButton::clicked, this,
          [this] { OnImportSave(2); });
  connect(m_save_row_port2.restore_button, &QPushButton::clicked, this,
          [this] { OnRestoreDefaultSave(1); });
  connect(m_save_row_port3.restore_button, &QPushButton::clicked, this,
          [this] { OnRestoreDefaultSave(2); });

  connect(m_boot_button, &QPushButton::clicked, this, &XDLauncherDialog::OnBootSolo);
  connect(m_search_button, &QPushButton::clicked, this, &XDLauncherDialog::OnSearchForMatch);
  connect(m_host_button, &QPushButton::clicked, this, &XDLauncherDialog::OnHost);
  connect(m_join_button, &QPushButton::clicked, this, &XDLauncherDialog::OnJoin);
  connect(m_browse_button, &QPushButton::clicked, this, [this] {
    // The browser's join path skips this dialog, so gate at the door: joining
    // without the dump or the official BIOS produces exactly the silent
    // GBA-not-detected session the checklist exists to prevent.
    if (!FindXdGame())
    {
      ModalMessageBox::warning(
          this, tr("OrreLink"),
          tr("Pokémon XD (USA) was not found in your game list. Add its folder first."));
      return;
    }
    if (!XDNetplay::CheckOfficialBios(nullptr))
    {
      ModalMessageBox::warning(
          this, tr("OrreLink"),
          tr("The official GBA BIOS is required — XD cannot detect the GBA without it. "
             "Add it with \"Choose BIOS...\" first."));
      return;
    }
    emit BrowsePublic();
  });
  connect(m_team_editor_button, &QPushButton::clicked, this, &XDLauncherDialog::OnTeamEditor);

  connect(m_practice_dummy_check, &QCheckBox::toggled, this, [](bool checked) {
    Config::SetBaseOrCurrent(Config::MAIN_GBA_PRACTICE_DUMMY, checked);
    Config::Save();
  });

  // Persist a Battle Style pick the moment it is made -- same idiom as the
  // checkboxes above. Only the config key is written here: the AR block is
  // assembled from these keys by BattleCustomizer's lifecycle hooks (start
  // forcing / team submission), never from the launcher directly.
  const auto connect_style_combo = [this](QComboBox* combo, const Config::Info<int>& setting) {
    connect(combo, &QComboBox::currentIndexChanged, this, [combo, &setting](int index) {
      Config::SetBaseOrCurrent(setting, combo->itemData(index).toInt());
      Config::Save();
    });
  };
  // The Format pick persists through the identical idiom: only the config key
  // is written here. Everything that enforces a format (the host gate, the
  // guest-submission gates, the "[Orre] " session tag) reads MAIN_XD_FORMAT
  // for itself at its own moment -- the dropdown never enforces anything.
  connect_style_combo(m_format_combo, Config::MAIN_XD_FORMAT);
  connect_style_combo(m_style_host_model_combo, Config::MAIN_XD_STYLE_HOST_MODEL);
  connect_style_combo(m_style_guest_model_combo, Config::MAIN_XD_STYLE_GUEST_MODEL);
  connect_style_combo(m_style_music_combo, Config::MAIN_XD_STYLE_MUSIC);
  connect_style_combo(m_style_venue_combo, Config::MAIN_XD_STYLE_VENUE);

  connect(m_show_on_startup_check, &QCheckBox::toggled, this, [](bool checked) {
    Settings::GetQSettings().setValue(QStringLiteral("xdnetplay/showlauncheronstartup"), checked);
  });
}

bool XDLauncherDialog::ShowOnStartup()
{
  return Settings::GetQSettings()
      .value(QStringLiteral("xdnetplay/showlauncheronstartup"), true)
      .toBool();
}

void XDLauncherDialog::showEvent(QShowEvent* event)
{
  QDialog::showEvent(event);
  // Boundary heal for a killed session's leftovers (an opponent's team in a
  // socket save, a disposable over an import): the launcher opening is the
  // first thing a player does after a crash, and everything it leads to --
  // solo boot, the team editor, hosting -- reads the socket saves. No-op
  // unless leftovers exist; refuses to run while a room or emulation is live.
  XDNetplay::DisposableSave::HealLeftoverSession();
  {
    const QSignalBlocker blocker(m_practice_dummy_check);
    m_practice_dummy_check->setChecked(Config::Get(Config::MAIN_GBA_PRACTICE_DUMMY));
  }
  {
    // The hub carries the same toggle, so this one can be stale by the time the
    // launcher is opened from it.
    const QSignalBlocker blocker(m_show_on_startup_check);
    m_show_on_startup_check->setChecked(ShowOnStartup());
  }
  // Battle Style combos re-read their keys on every show. An id the tables no
  // longer carry displays as "Game default" -- which is also exactly what
  // BattleCustomizer will generate for it (invalid ids are treated as absent,
  // never clamped), so the UI and the code block cannot disagree.
  const auto refresh_style_combo = [](QComboBox* combo, const Config::Info<int>& setting) {
    const QSignalBlocker blocker(combo);
    const int index = combo->findData(Config::Get(setting));
    combo->setCurrentIndex(index >= 0 ? index : 0);
  };
  // Same story for the Format pick: findData on a value the dropdown does not
  // carry lands on index 0 -- Free -- which is precisely how FormatRules::
  // IsOrreColosseum treats an unknown key value (no enforcement, never a
  // surprise lockout), so here too the UI and the code cannot disagree.
  refresh_style_combo(m_format_combo, Config::MAIN_XD_FORMAT);
  refresh_style_combo(m_style_host_model_combo, Config::MAIN_XD_STYLE_HOST_MODEL);
  refresh_style_combo(m_style_guest_model_combo, Config::MAIN_XD_STYLE_GUEST_MODEL);
  refresh_style_combo(m_style_music_combo, Config::MAIN_XD_STYLE_MUSIC);
  refresh_style_combo(m_style_venue_combo, Config::MAIN_XD_STYLE_VENUE);
  RefreshChecklist();
}

void XDLauncherDialog::RefreshChecklist()
{
  AutoDiscoverFromGameFolder();

  SetRowState(m_game_row.status, FindXdGame() != nullptr);
  SetRowState(m_rom_row.status, EmeraldRomConfigured());
  // Not required. The bundled open-source BIOS links fine now that the
  // auto-reset no longer races Emerald's connection handshake (the reset was
  // the real problem, never the BIOS). An official dump is still used if the
  // user has one; otherwise the bundled BIOS boots the GBA.
  // Required, and settled the hard way: the official dump is the only BIOS the
  // game will accept. Its boot code contains Nintendo's own JoyBus listener,
  // which is what answers the GameCube's handshake -- no open-source BIOS
  // reimplements it, and mGBA's HLE BIOS does not either. Tested on device:
  // the bundled build, a current Cult-of-GBA build and HLE all leave the link
  // window opening while the handshake never starts.
  const bool official_bios = XDNetplay::CheckOfficialBios(nullptr);
  SetRowState(m_bios_row.status, official_bios);
  m_bios_row.description->setText(
      official_bios ? tr("Official GBA BIOS configured") :
                      tr("Official GBA BIOS required — the game cannot detect the GBA without it"));
  SetRowState(m_team_saves_row.status, TeamSavesInstalled());
  SetRowState(m_vs_save_row.status, VsSaveInstalled());
  const bool input_mapped = XDNetplay::GbaInputMapped();
  SetRowState(m_gba_input_row.status, input_mapped);
  m_gba_input_row.description->setText(
      input_mapped ? tr("GBA controls mapped (your GBA is \"GBA 1\" in Controllers)") :
                     tr("GBA controls not mapped — your GBA will not respond to input"));
  RefreshSaveSlots();
}

void XDLauncherDialog::RefreshSaveSlots()
{
  const auto refresh_row = [](SaveSlotRow* row, int device,
                              const Config::Info<std::string>& imported_key) {
#ifdef HAS_LIBMGBA
    // The config key only remembers the picked file's NAME for display; the
    // authoritative "an import happened" signal is the .preimport backup, so
    // the restore action also unlocks when the key was lost but the backup
    // proves an import occurred.
    const std::string imported_name = Config::Get(imported_key);
    const std::string save_path = SaveSlotPath(device);

    QString text;
    if (save_path.empty())
    {
      text = tr("No GBA ROM configured yet");
    }
    else if (!File::Exists(save_path))
    {
      text = tr("Not installed yet — use the checklist's Install button");
    }
    else
    {
      text = imported_name.empty() ?
                 tr("Team editor save (default)") :
                 tr("Imported: %1").arg(QString::fromStdString(imported_name));
      // Game and trainer come from the save file itself, so the row cannot
      // disagree with what would actually boot. A parse failure (hand-edited
      // file, mid-write crash) is worth surfacing here instead of hiding.
      if (const auto save = XDNetplay::LoadSaveFile(save_path))
      {
        text += tr(" — %1, trainer %2")
                    .arg(QString::fromUtf8(
                        XDNetplay::GameDisplayName(XDNetplay::EmeraldSave::DetectGame(*save))))
                    .arg(QString::fromStdString(save->GetTrainerName()));
      }
      else
      {
        text += tr(" — unreadable save file");
      }
    }
    row->state->setText(text);
    row->import_button->setEnabled(true);
    row->restore_button->setEnabled(XDNetplay::SaveImport::HasImportBackup(device) ||
                                    !imported_name.empty());
#else
    static_cast<void>(device);
    static_cast<void>(imported_key);
    row->state->setText(tr("GBA support (libmgba) is not compiled into this build"));
    row->import_button->setEnabled(false);
    row->restore_button->setEnabled(false);
#endif
  };
  refresh_row(&m_save_row_port2, 1, Config::MAIN_XD_IMPORTED_SAVE_2);
  refresh_row(&m_save_row_port3, 2, Config::MAIN_XD_IMPORTED_SAVE_3);
}

void XDLauncherDialog::OnImportSave(int device)
{
  const QString path =
      QFileDialog::getOpenFileName(this, tr("Select a Gen 3 save file"), QString(),
                                   tr("Gen 3 save files (*.sav *.sa2 *.srm);;All files (*)"));
  if (path.isEmpty())
    return;

  // All validation (size, structure, checksums, save-game vs port-ROM match)
  // and the no-destruction bookkeeping (.preimport backup, tmp/readback/
  // rename) live in SaveImport; the picked file is only ever read. |error| and
  // |status| arrive user-displayable, refusal reasons included.
  std::string status;
  std::string error;
  if (!XDNetplay::SaveImport::ImportUserSave(path.toStdString(), device, &status, &error))
  {
    ModalMessageBox::warning(this, tr("OrreLink"), QString::fromStdString(error));
  }
  else
  {
    ModalMessageBox::information(this, tr("OrreLink"), QString::fromStdString(status));
  }
  RefreshChecklist();
}

void XDLauncherDialog::OnRestoreDefaultSave(int device)
{
  std::string error;
  if (!XDNetplay::SaveImport::RestoreDefaultSave(device, &error))
  {
    ModalMessageBox::warning(this, tr("OrreLink"), QString::fromStdString(error));
  }
  else
  {
    ModalMessageBox::information(
        this, tr("OrreLink"),
        tr("The bundled team-editor save is back in place. If it replaced another save, that one "
           "was kept next to it as a .bak file."));
  }
  RefreshChecklist();
}

void XDLauncherDialog::AutoDiscoverFromGameFolder()
{
  // A user can drop EMERALD.gba next to the XD ISO (or in a GBA/ subfolder)
  // and the launcher configures itself. Only runs while something is still
  // unconfigured and never overwrites a valid configuration.
  const auto game = FindXdGame();
  if (!game)
    return;

  std::string game_dir;
  SplitPath(game->GetFilePath(), &game_dir, nullptr, nullptr);
  if (game_dir.empty())
    return;

  bool rom_ok = EmeraldRomConfigured();
  bool bios_ok = false;
  for (const std::string& dir : {game_dir, game_dir + "GBA/"})
  {
    // Invisible quality-of-life pass: adopt an official BIOS dump lying here.
    // The bundled open-source BIOS already works without it, so this has no
    // checklist row and no UI of any kind.
    if (!bios_ok)
      bios_ok = XDNetplay::AutoImportOfficialBios(dir);

    if (rom_ok || !File::IsDirectory(dir))
      continue;

    const File::FSTEntry entry = File::ScanDirectoryTree(dir, false);
    for (const File::FSTEntry& child : entry.children)
    {
      if (child.isDirectory)
        continue;

      std::string name = child.virtualName;
      Common::ToLower(&name);

      if (name.ends_with(".gba") && IsEmeraldRom(child.physicalName))
      {
        rom_ok = ImportEmeraldRom(child.physicalName);
        if (rom_ok)
          break;
      }
    }
  }
}

std::shared_ptr<const UICommon::GameFile> XDLauncherDialog::FindXdGame() const
{
  for (int i = 0; i < m_game_list_model.rowCount(QModelIndex()); i++)
  {
    auto game = m_game_list_model.GetGameFile(i);
    if (game && XDNetplay::IsXdGameId(game->GetGameID()))
      return game;
  }
  return nullptr;
}

void XDLauncherDialog::OnFixGamePath()
{
  // A file picker, not a folder picker: users know where their ISO is, not
  // what a "game folder" means. The game list still works on directories, so
  // register the file's parent directory.
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Select your Pokémon XD ISO"), QString(),
      tr("GameCube disc images (*.iso *.ciso *.rvz *.gcm *.gcz *.wia)"));
  if (!path.isEmpty())
  {
    std::string parent_dir;
    SplitPath(path.toStdString(), &parent_dir, nullptr, nullptr);
    if (!parent_dir.empty())
      Settings::Instance().AddPath(QString::fromStdString(parent_dir));
  }
  RefreshChecklist();
}


void XDLauncherDialog::OnFixGbaInput()
{
  if (!XDNetplay::ApplyDefaultGbaInput())
  {
    ModalMessageBox::warning(
        this, tr("OrreLink"),
        tr("Could not set default GBA controls. Map them under Controllers > GBA Ports > GBA 1."));
    return;
  }
  ModalMessageBox::information(
      this, tr("OrreLink"),
      tr("Default GBA controls applied to GBA 1:\n\n"
         "A = X,  B = Z,  Start = Enter,  Select = Backspace\n"
         "D-Pad = T / G / F / H,  L = Q,  R = W\n\n"
         "Change them any time under Controllers > GBA Ports > GBA 1."));
  RefreshChecklist();
}

void XDLauncherDialog::OnFixBios()
{
  const QString path = QFileDialog::getOpenFileName(this, tr("Select the official GBA BIOS"),
                                                    QString(), tr("GBA BIOS (*.bin)"));
  if (path.isEmpty())
    return;

  if (!XDNetplay::ImportOfficialBios(path.toStdString()))
  {
    ModalMessageBox::warning(
        this, tr("OrreLink"),
        tr("That file is not the official GBA BIOS (16 KB, must match the known hash). "
           "XD cannot detect the GBA without the official dump."));
    return;
  }
  RefreshChecklist();
}

void XDLauncherDialog::OnFixEmeraldRom()
{
  const QString path = QFileDialog::getOpenFileName(this, tr("Select an Emerald ROM"), QString(),
                                                    tr("GBA ROMs (*.gba)"));
  if (path.isEmpty())
    return;

  if (!HasGbaHeader(path.toStdString()))
  {
    ModalMessageBox::warning(this, tr("OrreLink"),
                             tr("This file does not look like a GBA ROM."));
    return;
  }

  if (!ImportEmeraldRom(path.toStdString()))
  {
    ModalMessageBox::warning(this, tr("OrreLink"),
                             tr("Failed to copy the ROM into Dolphin's GBA folder."));
    return;
  }
  RefreshChecklist();
}

void XDLauncherDialog::OnFixTeamSaves()
{
  if (!EmeraldRomConfigured())
  {
    ModalMessageBox::warning(this, tr("OrreLink"), tr("Configure the Emerald ROM first."));
    return;
  }
  if (!XDNetplay::SeedTeamSaves())
  {
    ModalMessageBox::warning(this, tr("OrreLink"),
                             tr("Could not install the bundled team saves."));
  }
  RefreshChecklist();
}

void XDLauncherDialog::OnFixVsSave()
{
  if (!XDNetplay::SeedVsModeGci())
  {
    ModalMessageBox::warning(this, tr("OrreLink"),
                             tr("Could not install the bundled VS-mode save."));
  }
  RefreshChecklist();
}

void XDLauncherDialog::OnTeamEditor()
{
  if (!m_team_editor)
    m_team_editor = new TeamEditorDialog(this);
  m_team_editor->show();
  m_team_editor->raise();
  m_team_editor->activateWindow();
}

void XDLauncherDialog::OnBootSolo()
{
  const auto game = FindXdGame();
  if (!game)
  {
    ModalMessageBox::warning(
        this, tr("OrreLink"),
        tr("Pokémon XD (USA) was not found in your game list. Add its folder first."));
    return;
  }

  if (!XDNetplay::CheckOfficialBios(nullptr))
  {
    ModalMessageBox::warning(
        this, tr("OrreLink"),
        tr("The official GBA BIOS is required — XD cannot detect the GBA without it. "
           "Add it with \"Choose BIOS...\" first."));
    return;
  }

  XDNetplay::EnsureGbaConfig();
  // Battle Style applies to solo boots too -- the generator was originally
  // wired only into the netplay host path, and the first field test ran solo,
  // picked a model and a venue, and correctly concluded nothing worked.
  XDNetplay::BattleCustomizer::PrepareForStart();
  emit BootXD(QString::fromStdString(game->GetFilePath()));
}

void XDLauncherDialog::OnHost()
{
  const auto game = FindXdGame();
  if (!game)
  {
    ModalMessageBox::warning(
        this, tr("OrreLink"),
        tr("Pokémon XD (USA) was not found in your game list. Add its folder first."));
    return;
  }

  if (!XDNetplay::CheckOfficialBios(nullptr))
  {
    ModalMessageBox::warning(
        this, tr("OrreLink"),
        tr("The official GBA BIOS is required — XD cannot detect the GBA without it. "
           "Add it with \"Choose BIOS...\" first."));
    return;
  }

  PrepareNetplayConfig();
  // A manually hosted room is published too -- otherwise nobody can find it
  // except by the host reading their code aloud, which is exactly the workflow
  // "Search for Match" exists to retire. |false|: a password the user set in
  // the NetPlay Browser's publish panel is honoured here.
  PreparePublishConfig(false);
  Config::Save();
  emit HostXD(*game);
}

void XDLauncherDialog::SetMatchStatus(const QString& text)
{
  m_match_status->setText(text);
  m_match_status->setVisible(!text.isEmpty());
}

void XDLauncherDialog::OnSearchForMatch()
{
  if (m_searching)
    return;

  const auto game = FindXdGame();
  if (!game)
  {
    ModalMessageBox::warning(
        this, tr("OrreLink"),
        tr("Pokémon XD (USA) was not found in your game list. Add its folder first."));
    return;
  }

  if (!XDNetplay::CheckOfficialBios(nullptr))
  {
    ModalMessageBox::warning(
        this, tr("OrreLink"),
        tr("The official GBA BIOS is required — XD cannot detect the GBA without it. "
           "Add it with \"Choose BIOS...\" first."));
    return;
  }

  // Do the local setup up front: whichever way the search lands, joining and
  // hosting both need the GBA config and a nickname in place.
  PrepareNetplayConfig();
  Config::Save();

  m_searching = true;
  m_search_button->setEnabled(false);
  SetMatchStatus(tr("Searching for an open battle…"));

  // NetPlayIndex::List() is a blocking HTTP round trip. Running it on the GUI
  // thread would freeze the whole launcher for however long the index server
  // takes to answer (and it can time out), so it goes to a worker and comes
  // back through QueueOnObject -- same shape as TeamEditorDialog's paste
  // fetch. QPointer, not a raw this: Dolphin can be quit mid-search, and
  // QueueOnObject on a destroyed receiver is undefined.
  QPointer<XDLauncherDialog> self(this);
  std::thread([self] {
    NetPlayIndex client;

    // Coarse server-side cut; PickMatch re-checks all of it locally.
    // Deliberately no "game" filter: the index matches that field literally
    // and hosts publish differently shaped display names (see
    // XDNetplay::LooksLikeXdSession), so a server-side game filter would drop
    // valid rooms. No region filter either -- we want any opponent, anywhere.
    std::map<std::string, std::string> filters;
    filters["version"] = Common::GetScmDescStr();
    filters["in_game"] = "0";
    filters["password"] = "0";

    const auto entries = client.List(filters);
    const bool listed = entries.has_value();
    std::optional<NetPlaySession> match;
    if (listed)
      match = PickMatch(*entries);

    if (!self)
      return;
    // QueueOnObject takes a raw receiver; the QPointer copy inside the lambda
    // is what guards the deferred call.
    QueueOnObject(self.data(), [self, listed, match = std::move(match)] {
      if (!self)
        return;
      self->FinishSearch(listed, match);
    });
  }).detach();
}

void XDLauncherDialog::FinishSearch(bool listed, const std::optional<NetPlaySession>& match)
{
  m_searching = false;
  m_search_button->setEnabled(true);

  // "Could not reach the index" is not "nobody is playing". Silently hosting
  // on a network error would strand the user in a room no one can see.
  if (!listed)
  {
    SetMatchStatus(tr("Could not reach the session list. Check your connection, then try again."));
    return;
  }

  if (match)
  {
    SetMatchStatus(tr("Found \"%1\" — joining…").arg(QString::fromStdString(match->name)));

    // Exactly the handoff NetPlayBrowser::accept() performs. Password rooms
    // never get this far (PickMatch drops them), so there is no server_id to
    // decrypt here.
    Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE, match->method);
    Config::SetBaseOrCurrent(Config::NETPLAY_CONNECT_PORT, static_cast<u16>(match->port));
    if (match->method == "traversal")
      Config::SetBaseOrCurrent(Config::NETPLAY_HOST_CODE, match->server_id);
    else
      Config::SetBaseOrCurrent(Config::NETPLAY_ADDRESS, match->server_id);
    Config::Save();

    emit JoinXD();
    return;
  }

  // Nobody waiting: become the room everyone else's search will find. Re-look
  // up the game rather than capturing it -- the game list rescans on its own
  // thread and could have dropped the entry while the search was in flight.
  const auto game = FindXdGame();
  if (!game)
  {
    SetMatchStatus(
        tr("No open battles found, and Pokémon XD is no longer in your game list."));
    return;
  }

  SetMatchStatus(tr("No open battles found — hosting one. Your room is listed publicly; "
                    "leave the NetPlay window open and wait for an opponent."));
  PreparePublishConfig(true);
  Config::Save();
  emit HostXD(*game);
}

void XDLauncherDialog::OnJoin()
{
  const std::string code = m_join_code_edit->text().trimmed().toStdString();
  if (code.empty())
  {
    ModalMessageBox::warning(this, tr("OrreLink"),
                             tr("Enter the host code your opponent shared."));
    return;
  }

  // Netplay never downloads the game: a joiner without the XD dump in their
  // game list connects and then dies at start with a generic "game not found"
  // far from the checklist that explains the fix. Same gate as Boot/Host.
  if (!FindXdGame())
  {
    ModalMessageBox::warning(
        this, tr("OrreLink"),
        tr("Pokémon XD (USA) was not found in your game list. Add its folder first."));
    return;
  }

  if (!XDNetplay::CheckOfficialBios(nullptr))
  {
    ModalMessageBox::warning(
        this, tr("OrreLink"),
        tr("The official GBA BIOS is required — XD cannot detect the GBA without it. "
           "Add it with \"Choose BIOS...\" first."));
    return;
  }

  PrepareNetplayConfig();
  Config::SetBaseOrCurrent(Config::NETPLAY_HOST_CODE, code);
  Config::Save();
  emit JoinXD();
}
