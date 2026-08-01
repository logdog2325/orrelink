// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include <QDialog>

#include "DolphinQt/GameList/GameListModel.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class TeamEditorDialog;

namespace UICommon
{
class GameFile;
}

// One-stop launcher for Pokemon XD GBA-link battles: a setup checklist with
// fix-it buttons (XD ISO, Emerald ROM, bundled saves) plus one-click solo
// boot, host, join, and public-session browsing. Also auto-discovers an
// Emerald ROM dropped next to the XD ISO. The GBA BIOS never appears in the
// UI: the bundled open-source BIOS is sufficient, and an official dump found
// next to the ISO is silently verified and imported.
class XDLauncherDialog : public QDialog
{
  Q_OBJECT
public:
  explicit XDLauncherDialog(const GameListModel& game_list_model, QWidget* parent);

  // Whether a launcher screen should open automatically in front of the main
  // window at startup (QSettings xdnetplay/showlauncheronstartup, default
  // true). The screen that actually opens is the Pokémon Hub, not this dialog
  // -- see PokemonHubDialog::ShowOnStartup, which reads the same key. This
  // accessor stays because this dialog carries a second copy of the checkbox.
  static bool ShowOnStartup();

signals:
  void BootXD(const QString& path);
  void HostXD(const UICommon::GameFile& game);
  void JoinXD();
  void BrowsePublic();

protected:
  void showEvent(QShowEvent* event) override;

private:
  struct ChecklistRow
  {
    QLabel* status = nullptr;
    QLabel* description = nullptr;
    QPushButton* fix_button = nullptr;
  };

  void CreateMainLayout();
  void ConnectWidgets();
  void RefreshChecklist();
  void AutoDiscoverFromGameFolder();

  std::shared_ptr<const UICommon::GameFile> FindXdGame() const;

  void OnFixGamePath();
  void OnFixEmeraldRom();
  void OnFixBios();
  void OnFixGbaInput();
  void OnFixTeamSaves();
  void OnFixVsSave();
  void OnGbaInputInfo();

  void OnBootSolo();
  void OnHost();
  void OnJoin();
  void OnTeamEditor();

  const GameListModel& m_game_list_model;

  ChecklistRow m_game_row;
  ChecklistRow m_rom_row;
  ChecklistRow m_bios_row;
  ChecklistRow m_team_saves_row;
  ChecklistRow m_vs_save_row;
  ChecklistRow m_gba_input_row;

  QCheckBox* m_practice_dummy_check;
  QCheckBox* m_cheats_check;
  QCheckBox* m_show_on_startup_check;
  QPushButton* m_boot_button;
  QPushButton* m_host_button;
  QLineEdit* m_join_code_edit;
  QPushButton* m_join_button;
  QPushButton* m_browse_button;
  QPushButton* m_team_editor_button;
  TeamEditorDialog* m_team_editor = nullptr;
};
