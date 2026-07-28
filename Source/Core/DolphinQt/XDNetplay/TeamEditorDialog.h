// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QDialog>
#include <QString>

#include "UICommon/XDNetplay/Gen3Data.h"
#include "UICommon/XDNetplay/Gen3Mon.h"
#include "UICommon/XDNetplay/Gen3Save.h"

class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;

// Gen 3 team editor for the XD Netplay GBA saves, the desktop port of the
// Android build's TeamEditorActivity. Pick a role (host = GBA port 2, guest =
// GBA port 3), paste a Showdown export or a pokepast.es link, and the party
// in the corresponding EMERALD save is rebuilt with legal, deterministic
// mons owned by that save's trainer. Writing uses the verified-write
// discipline (re-parse, tmp, byte-identical readback, .bak, rename).
class TeamEditorDialog : public QDialog
{
  Q_OBJECT
public:
  explicit TeamEditorDialog(QWidget* parent);

protected:
  void showEvent(QShowEvent* event) override;

private:
  void CreateMainLayout();
  void ConnectWidgets();

  // 0-based GBA device number for the selected role: 1 = host, 2 = guest.
  int DeviceNumber() const;

  void ReloadForRole();
  void RefreshPartyList();
  void SetMessages(const QStringList& messages);

  void OnImport();
  void ApplyImportText(const std::string& text);
  void OnRemoveSelected();
  void OnSave();
  void OnShowInFolder();

  std::optional<XDNetplay::Gen3Data> m_data;
  std::optional<XDNetplay::EmeraldSave> m_save;
  std::vector<XDNetplay::Gen3Mon> m_party;
  std::map<int, QString> m_species_names;  // internal id -> display name
  std::string m_save_path;
  bool m_fetching = false;

  QComboBox* m_role_combo;
  QLabel* m_trainer_label;
  QListWidget* m_party_list;
  QPlainTextEdit* m_paste_edit;
  QPushButton* m_import_button;
  QPushButton* m_remove_button;
  QPushButton* m_save_button;
  QPushButton* m_show_folder_button;
  QLabel* m_log_label;
};
