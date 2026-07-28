// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/XDNetplay/TeamEditorDialog.h"

#include <thread>
#include <utility>

#include <QComboBox>
#include <QDesktopServices>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QUrl>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/HW/GBACore.h"

#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"

#include "UICommon/XDNetplay/MonFactory.h"
#include "UICommon/XDNetplay/ShowdownParser.h"

using namespace XDNetplay;

namespace
{
// Read a whole binary file; empty vector + false on failure.
bool ReadFileBytes(const std::string& path, std::vector<u8>* out)
{
  File::IOFile file(path, "rb");
  if (!file)
    return false;
  out->resize(file.GetSize());
  return file.ReadBytes(out->data(), out->size());
}
}  // namespace

TeamEditorDialog::TeamEditorDialog(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("XD Netplay Team Editor"));

  CreateMainLayout();
  ConnectWidgets();
}

void TeamEditorDialog::CreateMainLayout()
{
  auto* layout = new QVBoxLayout;

  auto* top_layout = new QGridLayout;
  m_role_combo = new QComboBox;
  m_role_combo->addItem(tr("Host — GBA port 2"));
  m_role_combo->addItem(tr("Guest — GBA port 3"));
  m_trainer_label = new QLabel(QString());
  top_layout->addWidget(new QLabel(tr("Editing team for:")), 0, 0);
  top_layout->addWidget(m_role_combo, 0, 1);
  top_layout->addWidget(m_trainer_label, 1, 0, 1, 2);
  top_layout->setColumnStretch(1, 1);
  layout->addLayout(top_layout);

  auto* party_box = new QGroupBox(tr("Party"));
  auto* party_layout = new QVBoxLayout;
  m_party_list = new QListWidget;
  m_remove_button = new NonDefaultQPushButton(tr("Remove Selected"));
  party_layout->addWidget(m_party_list);
  party_layout->addWidget(m_remove_button);
  party_box->setLayout(party_layout);
  layout->addWidget(party_box);

  auto* import_box = new QGroupBox(tr("Import"));
  auto* import_layout = new QVBoxLayout;
  m_paste_edit = new QPlainTextEdit;
  m_paste_edit->setPlaceholderText(
      tr("Paste a Showdown team export or a pokepast.es link here.\n"
         "Importing replaces the whole party (up to 6)."));
  m_import_button = new NonDefaultQPushButton(tr("Import"));
  import_layout->addWidget(m_paste_edit);
  import_layout->addWidget(m_import_button);
  import_box->setLayout(import_layout);
  layout->addWidget(import_box);

  auto* action_layout = new QGridLayout;
  m_save_button = new NonDefaultQPushButton(tr("Save Team (verified)"));
  m_show_folder_button = new NonDefaultQPushButton(tr("Show save in folder"));
  action_layout->addWidget(m_save_button, 0, 0);
  action_layout->addWidget(m_show_folder_button, 0, 1);
  layout->addLayout(action_layout);

  m_log_label = new QLabel(QString());
  m_log_label->setWordWrap(true);
  layout->addWidget(m_log_label);

  setLayout(layout);
  resize(520, 640);
}

void TeamEditorDialog::ConnectWidgets()
{
  connect(m_role_combo, &QComboBox::currentIndexChanged, this,
          [this](int) { ReloadForRole(); });
  connect(m_import_button, &QPushButton::clicked, this, &TeamEditorDialog::OnImport);
  connect(m_remove_button, &QPushButton::clicked, this, &TeamEditorDialog::OnRemoveSelected);
  connect(m_save_button, &QPushButton::clicked, this, &TeamEditorDialog::OnSave);
  connect(m_show_folder_button, &QPushButton::clicked, this, &TeamEditorDialog::OnShowInFolder);
}

void TeamEditorDialog::showEvent(QShowEvent* event)
{
  QDialog::showEvent(event);
  ReloadForRole();
}

int TeamEditorDialog::DeviceNumber() const
{
  return m_role_combo->currentIndex() == 0 ? 1 : 2;
}

void TeamEditorDialog::ReloadForRole()
{
  m_save.reset();
  m_party.clear();
  m_save_path.clear();
  m_trainer_label->setText(QString());

  QStringList messages;

#ifdef HAS_LIBMGBA
  std::string error;
  if (!m_data)
  {
    m_data = Gen3Data::LoadBundled(&error);
    if (m_data)
    {
      // Reverse id -> display-name map for the party list.
      for (const auto& [name, species] : m_data->GetSpecies())
      {
        if (m_species_names.count(species.id) == 0)
        {
          QString display = QString::fromStdString(name);
          if (!display.isEmpty())
            display[0] = display[0].toUpper();
          m_species_names[species.id] = display;
        }
      }
    }
    else
    {
      messages << tr("Could not load game data: %1").arg(QString::fromStdString(error));
    }
  }

  // Preferred ROM for the role, falling back to the other port's ROM (both
  // point at the same imported Emerald dump in a launcher-made setup).
  const int device = DeviceNumber();
  std::string rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device]);
  if (rom.empty() || !File::Exists(rom))
    rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[device == 1 ? 2 : 1]);
  if (rom.empty() || !File::Exists(rom))
  {
    SetMessages({tr("No Emerald ROM configured — run the launcher checklist first.")});
    RefreshPartyList();
    return;
  }

  m_save_path = HW::GBA::Core::GetSavePath(rom, device);

  std::vector<u8> bytes;
  if (File::Exists(m_save_path) && ReadFileBytes(m_save_path, &bytes))
  {
    messages << tr("Loaded %1").arg(QString::fromStdString(m_save_path));
  }
  else
  {
    const std::string template_path =
        File::GetSysDirectory() + (device == 1 ? "XDNetplay/EMERALD-2.sav" : "XDNetplay/EMERALD-3.sav");
    if (!ReadFileBytes(template_path, &bytes))
    {
      SetMessages({tr("No save and no bundled template found.")});
      RefreshPartyList();
      return;
    }
    messages << tr("No save yet — starting from the bundled template");
  }

  auto parsed = EmeraldSave::Create(std::move(bytes), &error);
  if (!parsed)
  {
    SetMessages({tr("Could not open the save: %1").arg(QString::fromStdString(error))});
    RefreshPartyList();
    return;
  }
  if (!parsed->VerifyAllChecksums().empty())
    messages << tr("Warning: section checksum(s) invalid in the source save");

  auto party = parsed->ReadParty(&error);
  if (!party)
  {
    SetMessages({tr("Could not decode the party: %1").arg(QString::fromStdString(error))});
    RefreshPartyList();
    return;
  }

  m_save = std::move(parsed);
  m_party.clear();
  for (Gen3Mon& mon : *party)
  {
    if (!mon.IsEmpty())
      m_party.push_back(std::move(mon));
  }

  std::string file_name = m_save_path;
  SplitPath(m_save_path, nullptr, &file_name, nullptr);
  m_trainer_label->setText(tr("Trainer %1  ·  %2.sav")
                               .arg(QString::fromStdString(m_save->GetTrainerName()))
                               .arg(QString::fromStdString(file_name)));
#else
  messages << tr("GBA support (libmgba) is not compiled into this build.");
#endif

  SetMessages(messages);
  RefreshPartyList();
}

void TeamEditorDialog::RefreshPartyList()
{
  m_party_list->clear();
  for (const Gen3Mon& mon : m_party)
  {
    const auto species_it = m_species_names.find(static_cast<int>(mon.species));
    const QString species_name = species_it != m_species_names.end() ?
                                     species_it->second :
                                     tr("Species %1").arg(mon.species);
    m_party_list->addItem(tr("%1  (%2)  Lv.%3  %4")
                              .arg(QString::fromStdString(mon.GetNickname()))
                              .arg(species_name)
                              .arg(mon.level)
                              .arg(QString::fromUtf8(mon.GetNatureName())));
  }
}

void TeamEditorDialog::SetMessages(const QStringList& messages)
{
  m_log_label->setText(messages.join(QStringLiteral("\n")));
}

void TeamEditorDialog::OnImport()
{
  if (m_fetching)
    return;

  const QString text = m_paste_edit->toPlainText().trimmed();
  if (text.isEmpty())
  {
    SetMessages({tr("Paste a Showdown export or a pokepast.es link first.")});
    return;
  }

  // A pokepast.es link is fetched (its /raw endpoint is the plain export) on
  // a worker thread; the result is marshalled back to the GUI thread.
  const QRegularExpression pokepaste_re(
      QStringLiteral("^https?://pokepast\\.es/[A-Za-z0-9]+"));
  const QRegularExpressionMatch match = pokepaste_re.match(text);
  if (!match.hasMatch())
  {
    ApplyImportText(text.toStdString());
    return;
  }

  const std::string url = match.captured(0).toStdString() + "/raw";
  SetMessages({tr("Fetching %1…").arg(match.captured(0))});
  m_fetching = true;
  std::thread([this, url] {
    Common::HttpRequest request;
    Common::HttpRequest::Response response = request.Get(url);
    QueueOnObject(this, [this, response = std::move(response)] {
      m_fetching = false;
      if (!response)
      {
        SetMessages({tr("Could not fetch the paste (network error).")});
        return;
      }
      ApplyImportText(std::string(response->begin(), response->end()));
    });
  }).detach();
}

void TeamEditorDialog::ApplyImportText(const std::string& text)
{
  if (!m_save || !m_data)
  {
    SetMessages({tr("No save loaded.")});
    return;
  }

  const std::vector<ShowdownSet> sets = ShowdownParser::ParseTeam(text);
  if (sets.empty())
  {
    SetMessages({tr("Nothing recognizable in that paste.")});
    return;
  }

  QStringList messages;
  std::vector<Gen3Mon> built;
  for (const ShowdownSet& set : sets)
  {
    if (built.size() == EmeraldSave::PARTY_MAX)
    {
      messages << tr("Skipped %1: party is full").arg(QString::fromStdString(set.species));
      continue;
    }
    std::string error;
    auto mon = MonFactory::Build(set, *m_data, m_save->GetTrainerName(), m_save->GetTrainerId(),
                                 &error);
    if (mon)
    {
      built.push_back(std::move(*mon));
    }
    else
    {
      messages << tr("Skipped %1: %2")
                      .arg(QString::fromStdString(set.species))
                      .arg(QString::fromStdString(error));
    }
  }
  if (!built.empty())
  {
    m_party = std::move(built);
    messages.prepend(
        tr("Imported %1 Pokémon (replaced party)").arg(static_cast<int>(m_party.size())));
  }
  SetMessages(messages);
  RefreshPartyList();
}

void TeamEditorDialog::OnRemoveSelected()
{
  const int row = m_party_list->currentRow();
  if (row < 0 || static_cast<size_t>(row) >= m_party.size())
    return;
  m_party.erase(m_party.begin() + row);
  SetMessages({tr("Removed from party")});
  RefreshPartyList();
}

void TeamEditorDialog::OnSave()
{
  if (!m_save || m_save_path.empty())
  {
    SetMessages({tr("No save loaded.")});
    return;
  }
  if (!m_save->WriteParty(m_party))
  {
    SetMessages({tr("Party is larger than 6.")});
    return;
  }
  std::string error;
  if (!VerifiedWriteSaveFile(m_save_path, *m_save, &error))
  {
    SetMessages({tr("NOT saved — %1").arg(QString::fromStdString(error))});
    return;
  }
  SetMessages({tr("Saved %1 Pokémon to %2 (verified; previous save backed up as .bak)")
                   .arg(static_cast<int>(m_party.size()))
                   .arg(QString::fromStdString(m_save_path))});
}

void TeamEditorDialog::OnShowInFolder()
{
  if (m_save_path.empty())
    return;
  std::string dir;
  SplitPath(m_save_path, &dir, nullptr, nullptr);
  if (!dir.empty())
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(dir)));
}
