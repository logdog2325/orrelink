// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/XDNetplay/XDLauncherDialog.h"

#include <array>
#include <initializer_list>
#include <string>
#include <string_view>

#include <QCheckBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/HW/GBACore.h"

#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/XDNetplay/XDNetplayConfig.h"

#include "UICommon/GameFile.h"

namespace
{
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
}  // namespace

XDLauncherDialog::XDLauncherDialog(const GameListModel& game_list_model, QWidget* parent)
    : QDialog(parent), m_game_list_model(game_list_model)
{
  setWindowTitle(tr("XD Netplay Launcher"));

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
  add_row(&m_team_saves_row, tr("Team saves installed"), tr("Install"));
  add_row(&m_vs_save_row, tr("XD VS-mode save (memory card)"), tr("Install"));
  add_row(&m_gba_input_row, tr("GBA input"), tr("Controllers..."));
  m_gba_input_row.status->setText(QStringLiteral("•"));
  m_gba_input_row.status->setStyleSheet(QString());
  checklist_layout->setColumnStretch(1, 1);
  checklist_box->setLayout(checklist_layout);
  layout->addWidget(checklist_box);

  auto* battle_box = new QGroupBox(tr("Battle"));
  auto* battle_layout = new QGridLayout;
  m_boot_button = new NonDefaultQPushButton(tr("Boot Pokémon XD (solo)"));
  m_practice_dummy_check = new QCheckBox(tr("Practice vs dummy"));
  m_practice_dummy_check->setToolTip(
      tr("Auto-plays GBA port 3 so you can practice link battles alone."));
  m_host_button = new NonDefaultQPushButton(tr("Host Battle"));
  m_join_code_edit = new QLineEdit;
  m_join_code_edit->setPlaceholderText(tr("Host code"));
  m_join_button = new NonDefaultQPushButton(tr("Join Battle"));
  m_browse_button = new NonDefaultQPushButton(tr("Browse Public Sessions"));
  battle_layout->addWidget(m_boot_button, 0, 0);
  battle_layout->addWidget(m_practice_dummy_check, 0, 1);
  battle_layout->addWidget(m_host_button, 1, 0, 1, 2);
  battle_layout->addWidget(m_join_code_edit, 2, 0);
  battle_layout->addWidget(m_join_button, 2, 1);
  battle_layout->addWidget(m_browse_button, 3, 0, 1, 2);
  battle_box->setLayout(battle_layout);
  layout->addWidget(battle_box);

  m_show_on_startup_check = new QCheckBox(tr("Show this launcher at startup"));
  m_show_on_startup_check->setChecked(ShowOnStartup());
  layout->addWidget(m_show_on_startup_check);

  setLayout(layout);
}

void XDLauncherDialog::ConnectWidgets()
{
  connect(m_game_row.fix_button, &QPushButton::clicked, this, &XDLauncherDialog::OnFixGamePath);
  connect(m_rom_row.fix_button, &QPushButton::clicked, this, &XDLauncherDialog::OnFixEmeraldRom);
  connect(m_team_saves_row.fix_button, &QPushButton::clicked, this,
          &XDLauncherDialog::OnFixTeamSaves);
  connect(m_vs_save_row.fix_button, &QPushButton::clicked, this, &XDLauncherDialog::OnFixVsSave);
  connect(m_gba_input_row.fix_button, &QPushButton::clicked, this,
          &XDLauncherDialog::OnGbaInputInfo);

  connect(m_boot_button, &QPushButton::clicked, this, &XDLauncherDialog::OnBootSolo);
  connect(m_host_button, &QPushButton::clicked, this, &XDLauncherDialog::OnHost);
  connect(m_join_button, &QPushButton::clicked, this, &XDLauncherDialog::OnJoin);
  connect(m_browse_button, &QPushButton::clicked, this, [this] { emit BrowsePublic(); });

  connect(m_practice_dummy_check, &QCheckBox::toggled, this, [](bool checked) {
    Config::SetBaseOrCurrent(Config::MAIN_GBA_PRACTICE_DUMMY, checked);
    Config::Save();
  });

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
  {
    const QSignalBlocker blocker(m_practice_dummy_check);
    m_practice_dummy_check->setChecked(Config::Get(Config::MAIN_GBA_PRACTICE_DUMMY));
  }
  RefreshChecklist();
}

void XDLauncherDialog::RefreshChecklist()
{
  AutoDiscoverFromGameFolder();

  SetRowState(m_game_row.status, FindXdGame() != nullptr);
  SetRowState(m_rom_row.status, EmeraldRomConfigured());
  SetRowState(m_team_saves_row.status, TeamSavesInstalled());
  SetRowState(m_vs_save_row.status, VsSaveInstalled());
  // m_gba_input_row stays informational.
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

void XDLauncherDialog::OnFixEmeraldRom()
{
  const QString path = QFileDialog::getOpenFileName(this, tr("Select an Emerald ROM"), QString(),
                                                    tr("GBA ROMs (*.gba)"));
  if (path.isEmpty())
    return;

  if (!HasGbaHeader(path.toStdString()))
  {
    ModalMessageBox::warning(this, tr("XD Netplay"),
                             tr("This file does not look like a GBA ROM."));
    return;
  }

  if (!ImportEmeraldRom(path.toStdString()))
  {
    ModalMessageBox::warning(this, tr("XD Netplay"),
                             tr("Failed to copy the ROM into Dolphin's GBA folder."));
    return;
  }
  RefreshChecklist();
}

void XDLauncherDialog::OnFixTeamSaves()
{
  if (!EmeraldRomConfigured())
  {
    ModalMessageBox::warning(this, tr("XD Netplay"), tr("Configure the Emerald ROM first."));
    return;
  }
  if (!XDNetplay::SeedTeamSaves())
  {
    ModalMessageBox::warning(this, tr("XD Netplay"),
                             tr("Could not install the bundled team saves."));
  }
  RefreshChecklist();
}

void XDLauncherDialog::OnFixVsSave()
{
  if (!XDNetplay::SeedVsModeGci())
  {
    ModalMessageBox::warning(this, tr("XD Netplay"),
                             tr("Could not install the bundled VS-mode save."));
  }
  RefreshChecklist();
}

void XDLauncherDialog::OnGbaInputInfo()
{
  ModalMessageBox::information(
      this, tr("GBA Input"),
      tr("Map the GBA buttons in the main Controllers settings: ports 2 and 3 are set to "
         "Emulated GBA by the launcher; only the button mapping is up to you."));
}

void XDLauncherDialog::OnBootSolo()
{
  const auto game = FindXdGame();
  if (!game)
  {
    ModalMessageBox::warning(
        this, tr("XD Netplay"),
        tr("Pokémon XD (USA) was not found in your game list. Add its folder first."));
    return;
  }

  XDNetplay::EnsureGbaConfig();
  emit BootXD(QString::fromStdString(game->GetFilePath()));
}

void XDLauncherDialog::OnHost()
{
  const auto game = FindXdGame();
  if (!game)
  {
    ModalMessageBox::warning(
        this, tr("XD Netplay"),
        tr("Pokémon XD (USA) was not found in your game list. Add its folder first."));
    return;
  }

  PrepareNetplayConfig();
  Config::Save();
  emit HostXD(*game);
}

void XDLauncherDialog::OnJoin()
{
  const std::string code = m_join_code_edit->text().trimmed().toStdString();
  if (code.empty())
  {
    ModalMessageBox::warning(this, tr("XD Netplay"),
                             tr("Enter the host code your opponent shared."));
    return;
  }

  PrepareNetplayConfig();
  Config::SetBaseOrCurrent(Config::NETPLAY_HOST_CODE, code);
  Config::Save();
  emit JoinXD();
}
