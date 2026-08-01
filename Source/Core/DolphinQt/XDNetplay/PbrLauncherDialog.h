// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include <QDialog>

#include "DolphinQt/GameList/GameListModel.h"

class QLabel;
class QPushButton;
class QShowEvent;

namespace UICommon
{
class GameFile;
}

// Desktop counterpart of the Android PBR launcher: a setup checklist for
// Pokemon Battle Revolution plus one-click boot. Mac and Windows previously had
// no PBR UI at all even though the Wiimmfi redirect is already frontend-
// agnostic (BootManager applies it to every boot), so the only thing missing
// was a place to tell the user what state their setup is in.
//
// Unlike the XD launcher this dialog owns almost no configuration: PBR needs no
// GBA ROM, no BIOS and no memory card. The three things a user can actually get
// wrong are having no disc in the game list, having no PBR save (the game only
// writes one after it has been played, which leaves the team editor with
// nothing to read), and expecting online play to work without a NAND from their
// own console.
class PbrLauncherDialog : public QDialog
{
  Q_OBJECT
public:
  explicit PbrLauncherDialog(const GameListModel& game_list_model, QWidget* parent);

signals:
  void BootPbr(const QString& path);

protected:
  void showEvent(QShowEvent* event) override;

private:
  struct ChecklistRow
  {
    QLabel* status = nullptr;
    QLabel* description = nullptr;
    QPushButton* fix_button = nullptr;
    QPushButton* alt_button = nullptr;
  };

  void CreateMainLayout();
  void ConnectWidgets();
  void RefreshChecklist();

  std::shared_ptr<const UICommon::GameFile> FindPbrGame() const;

  // Save path for the region of the disc we actually found, so a PAL player's
  // save is not filed under the USA title where their game would never read it.
  // Falls back to the USA slot when no disc is in the game list yet.
  std::string DetectedSavePath() const;

  void OnFixGamePath();
  void OnInstallBundledSave();
  void OnInstallOwnSave();
  void OnPlay();

  const GameListModel& m_game_list_model;

  ChecklistRow m_game_row;
  ChecklistRow m_save_row;
  ChecklistRow m_nand_row;

  QPushButton* m_play_button;
};
