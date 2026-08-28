// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>

#include <QDialog>

#include "DolphinQt/GameList/GameListModel.h"
#include "UICommon/NetPlayIndex.h"

class QCheckBox;
class QComboBox;
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

  // One GBA socket's row in the "Save Files" box: which save source is active
  // (bundled team save vs a user import, plus game and trainer read from the
  // file itself), an import picker and a restore-default action.
  struct SaveSlotRow
  {
    QLabel* state = nullptr;
    QPushButton* import_button = nullptr;
    QPushButton* restore_button = nullptr;
  };

  void CreateMainLayout();
  void ConnectWidgets();
  void RefreshChecklist();
  void RefreshSaveSlots();
  void AutoDiscoverFromGameFolder();

  std::shared_ptr<const UICommon::GameFile> FindXdGame() const;

  void OnFixGamePath();
  void OnFixEmeraldRom();
  void OnFixBios();
  void OnFixGbaInput();
  void OnFixTeamSaves();
  void OnFixVsSave();

  // |device| is the GBA device number: 1 = port 2 (your side), 2 = port 3
  // (the guest slot).
  void OnImportSave(int device);
  void OnRestoreDefaultSave(int device);

  void OnBootSolo();
  void OnHost();
  void OnJoin();
  void OnSearchForMatch();
  void OnTeamEditor();

  // GUI-thread tail of OnSearchForMatch(), invoked via QueueOnObject once the
  // worker thread has finished talking to the session index. |listed| is false
  // when the index could not be reached at all (which is NOT the same as "no
  // rooms"); |match| holds the room to join, or nothing to host instead.
  void FinishSearch(bool listed, const std::optional<NetPlaySession>& match);
  void SetMatchStatus(const QString& text);

  const GameListModel& m_game_list_model;

  ChecklistRow m_game_row;
  ChecklistRow m_rom_row;
  ChecklistRow m_bios_row;
  ChecklistRow m_team_saves_row;
  ChecklistRow m_vs_save_row;
  ChecklistRow m_gba_input_row;

  SaveSlotRow m_save_row_port2;
  SaveSlotRow m_save_row_port3;

  // Battle Style: the host's cosmetic picks (own trainer model, guest-model
  // fallback, music, location), persisted in the MAIN_XD_STYLE_* keys and
  // assembled into the synced "$OrreLink Battle Style" AR block by
  // BattleCustomizer when a session starts. Every combo's first entry is
  // "Game default", which genuinely emits nothing.
  //
  // m_format_combo is the one non-cosmetic pick in that group: the battle
  // FORMAT (Free / Orre Colosseum / OU), persisted in MAIN_XD_FORMAT and enforced
  // by FormatRules' gates -- see the tooltip built in CreateMainLayout.
  QComboBox* m_format_combo;
  QComboBox* m_style_host_model_combo;
  QComboBox* m_style_guest_model_combo;
  QComboBox* m_style_music_combo;
  QComboBox* m_style_venue_combo;

  QCheckBox* m_practice_dummy_check;
  QCheckBox* m_show_on_startup_check;
  QPushButton* m_boot_button;
  QPushButton* m_host_button;
  QLineEdit* m_join_code_edit;
  QPushButton* m_join_button;
  QPushButton* m_search_button;
  QPushButton* m_browse_button;
  QPushButton* m_team_editor_button;
  QLabel* m_match_status;
  // Guards against a second search being kicked off while one is in flight.
  // The worker thread is detached and only ever touches the GUI through
  // QueueOnObject, so this flag lives on the GUI thread alone.
  bool m_searching = false;
  TeamEditorDialog* m_team_editor = nullptr;
};
