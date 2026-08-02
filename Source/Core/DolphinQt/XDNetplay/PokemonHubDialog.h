// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>

#include "UICommon/XDNetplay/UpdateCheck.h"

class QCheckBox;
class QPushButton;
class QShowEvent;

// Desktop counterpart of the Android PokemonHubActivity: the entry point that
// asks which mode the user wants before either launcher opens. Two modes now
// share this fork (XD's GBA link and PBR's Wiimmfi online) and they force
// mutually exclusive local settings, so dropping a new player straight into one
// of them was a coin flip that landed wrong half the time.
//
// The hub itself owns no configuration and does not touch the game list -- it
// emits a signal per mode and lets MainWindow open the launcher that already
// knows how to set that mode up.
class PokemonHubDialog : public QDialog
{
  Q_OBJECT
public:
  explicit PokemonHubDialog(QWidget* parent);

  // Whether the hub should open automatically in front of the main window at
  // startup. Stored under the QSettings key the XD launcher has always used
  // (xdnetplay/showlauncheronstartup, default true): the hub replaced the XD
  // launcher as what actually opens, and renaming the key would silently turn
  // auto-open back on for everyone who had switched it off.
  static bool ShowOnStartup();

signals:
  void OpenXDLauncher();
  void OpenPbrLauncher();

protected:
  void showEvent(QShowEvent* event) override;

private:
  void CreateMainLayout();
  void ConnectWidgets();

  void CheckForUpdates();
  void ShowUpdateResult(const XDNetplay::UpdateCheckResult& result);

  QPushButton* m_xd_button;
  QPushButton* m_pbr_button;
  QPushButton* m_update_button;
  QCheckBox* m_show_on_startup_check;

  bool m_checking_for_update = false;
};
