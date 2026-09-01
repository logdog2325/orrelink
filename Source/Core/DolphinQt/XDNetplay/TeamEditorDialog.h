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
class QLineEdit;
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

  // Enable/disable everything that can modify the loaded save (name field,
  // paste box, import/remove/save). Role switching and "Show save in folder"
  // stay live either way. Used to make a FireRed/LeafGreen save -- playable,
  // but laid out differently where the party lives -- read-only instead of
  // silently corruptible.
  void SetEditingEnabled(bool enabled);

  void OnImport();
  void ApplyImportText(const std::string& text);
  void OnRemoveSelected();
  void OnRaiseToLevel100();
  void OnSave();
  void OnShowInFolder();

  // Write the trainer-name field into m_save and re-stamp the party's OT names
  // to match. Returns false with *error set if the typed name is not writable
  // as Gen 3 text; nothing is modified in that case.
  bool ApplyTrainerName(std::string* error);

  // Paste-time FORMAT feedback (FormatRules.h): when the local Format pick is
  // Orre Colosseum and the current party breaks its rules, the non-blocking
  // "note: this team is not Orre Colosseum legal - ..." line to append to the
  // log; empty otherwise. NEVER blocks anything -- importing and saving an
  // illegal team stays allowed (only the hosting/submission gates refuse), the
  // note just says it here first. With the pick on Free this is one int
  // compare: no validation runs at all.
  QString FormatComplianceNote() const;

  std::optional<XDNetplay::Gen3Data> m_data;
  std::optional<XDNetplay::EmeraldSave> m_save;
  std::vector<XDNetplay::Gen3Mon> m_party;
  std::map<int, QString> m_species_names;  // internal id -> display name
  std::string m_save_path;
  bool m_fetching = false;

  QComboBox* m_role_combo;
  QLineEdit* m_trainer_name_edit;
  QLabel* m_trainer_label;
  QListWidget* m_party_list;
  QPlainTextEdit* m_paste_edit;
  QPushButton* m_import_button;
  QPushButton* m_remove_button;
  QPushButton* m_raise_button;
  QPushButton* m_save_button;
  QPushButton* m_show_folder_button;
  QLabel* m_log_label;
};
