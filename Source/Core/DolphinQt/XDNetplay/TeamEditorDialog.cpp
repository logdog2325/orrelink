// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/XDNetplay/TeamEditorDialog.h"

#include <algorithm>
#include <thread>
#include <utility>

#include <QComboBox>
#include <QDesktopServices>
#include <QGridLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QUrl>
#include <QHBoxLayout>
#include <QScreen>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/HW/GBACore.h"
#include "Core/NetPlayProto.h"

#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/Settings.h"

#include "UICommon/XDNetplay/DisposableSave.h"
#include "UICommon/XDNetplay/FormatRules.h"
#include "UICommon/XDNetplay/Gen3Text.h"
#include "UICommon/XDNetplay/MonFactory.h"
#include "UICommon/XDNetplay/ShowdownParser.h"

using namespace XDNetplay;

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
  setWindowTitle(tr("OrreLink Team Editor"));

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

  // Trainer name: 7 characters is the hard Gen 3 limit, so the field enforces
  // it rather than letting someone type a name that would be silently cut.
  m_trainer_name_edit = new QLineEdit;
  m_trainer_name_edit->setMaxLength(static_cast<int>(EmeraldSave::TRAINER_NAME_LEN));
  m_trainer_name_edit->setPlaceholderText(tr("Trainer name"));

  m_trainer_label = new QLabel(QString());
  // Which slot matters where is the #1 point of confusion for joiners, so it
  // is said right under the selector: JOINING a room only ever sends the
  // HOST slot's party (the Submit Team sheet's "Use my save" reads GBA port
  // 2); the Guest slot is the fallback team for rooms YOU host, plus solo.
  auto* role_note = MakeNoteLabel(
      tr("Host — GBA port 2: your team when you host, and what \"Use my save\" sends when you "
         "join.\nGuest — GBA port 3: played by a joiner who submits no team, and in solo play. "
         "Not used when you join."));
  top_layout->addWidget(new QLabel(tr("Editing team for:")), 0, 0);
  top_layout->addWidget(m_role_combo, 0, 1);
  top_layout->addWidget(role_note, 1, 0, 1, 2);
  top_layout->addWidget(new QLabel(tr("Trainer name (max 7):")), 2, 0);
  top_layout->addWidget(m_trainer_name_edit, 2, 1);
  top_layout->addWidget(m_trainer_label, 3, 0, 1, 2);
  top_layout->setColumnStretch(1, 1);
  layout->addLayout(top_layout);

  auto* party_box = new QGroupBox(tr("Party"));
  auto* party_layout = new QVBoxLayout;
  m_party_list = new QListWidget;
  m_remove_button = new NonDefaultQPushButton(tr("Remove Selected"));
  // Optional convenience for the level-100 formats: bump every under-level
  // mon to Lv 100 (RAISE only -- never lowers, which would break legality).
  // Shown/enabled only when the local Format pick is a level-100 format
  // (ReloadForRole updates it); "Level 100 or lower" is legal, so this is
  // opt-in and never automatic.
  m_raise_button = new NonDefaultQPushButton(tr("Raise all to Lv. 100"));
  m_raise_button->setToolTip(tr("Lv. 100 formats only. Raises every Pokémon below Lv. 100 in "
                                "this team and never lowers one."));
  auto* party_buttons = new QHBoxLayout;
  party_buttons->addWidget(m_remove_button);
  party_buttons->addWidget(m_raise_button);
  party_layout->addWidget(m_party_list);
  party_layout->addLayout(party_buttons);
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
  // Never narrower than the wrapped notes' minimum width (MakeNoteLabel); never
  // taller than the screen's working area minus window chrome (720-px displays).
  const QRect avail =
      (screen() ? screen() : QGuiApplication::primaryScreen())->availableGeometry();
  resize(std::max(520, minimumSizeHint().width()),
         std::max(minimumSizeHint().height(), std::min(640, avail.height() - 48)));
}

void TeamEditorDialog::ConnectWidgets()
{
  connect(m_role_combo, &QComboBox::currentIndexChanged, this,
          [this](int) { ReloadForRole(); });
  connect(m_import_button, &QPushButton::clicked, this, &TeamEditorDialog::OnImport);
  connect(m_remove_button, &QPushButton::clicked, this, &TeamEditorDialog::OnRemoveSelected);
  connect(m_raise_button, &QPushButton::clicked, this, &TeamEditorDialog::OnRaiseToLevel100);
  connect(m_save_button, &QPushButton::clicked, this, &TeamEditorDialog::OnSave);
  connect(m_show_folder_button, &QPushButton::clicked, this, &TeamEditorDialog::OnShowInFolder);
}

void TeamEditorDialog::showEvent(QShowEvent* event)
{
  QDialog::showEvent(event);
  // The editor displays whatever the socket save holds -- after a killed
  // session that can be the OPPONENT's team. Heal leftovers before every
  // (re)load: the dialog instance is cached across opens, so a ctor-only heal
  // would miss every open after the first. No-op unless leftovers exist;
  // stands down while a room or emulation is live.
  XDNetplay::DisposableSave::HealLeftoverSession();
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
  m_trainer_name_edit->clear();
  // A previous load may have locked the controls (FRLG save); every reload
  // starts from "editable" and re-decides below.
  SetEditingEnabled(true);

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

  // Competitive integrity. While this machine is HOSTING a room, the socket-3
  // save is not "the guest slot" in any abstract sense -- it is the opponent's
  // submitted party, EVs, IVs and natures included, written there so netplay
  // can sync it at start. Showing it here would be a scouting tool. The host's
  // own socket-2 save stays editable, and once the room closes the cleanup has
  // already put the host's own team back in socket 3, so this unlocks by
  // itself. (Joiners are unaffected: their local socket-3 save is their own --
  // netplay runs a joiner from NetPlayTemp copies instead.)
  // Gated on the ROOM, not on the running game: submissions arrive in the lobby
  // and between battles, which is exactly when the game is not running.
  if (device == 2 && Settings::Instance().GetNetPlayServer())
  {
    SetMessages({tr("The guest slot is hidden while you are hosting — it holds your opponent's "
                    "submitted team. Close the room to see your own team here again.")});
    RefreshPartyList();
    return;
  }
  // The host's own slot locks while a room is open too. With an imported save,
  // the file on disk is the privacy DISPOSABLE for the session's duration; an
  // editor save here would write the full import back over it, and the next
  // Start would sync exactly the file the disposable exists to keep off the
  // wire. (Any edit would also be silently reverted by the room-close restore.)
  if (Settings::Instance().GetNetPlayServer())
  {
    SetMessages({tr("The team editor is locked while a netplay room is open. Close the room to "
                    "edit teams.")});
    RefreshPartyList();
    return;
  }

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

  // A user-imported save can be any Gen 3 game (the launcher's import
  // validates game-vs-ROM, not game-vs-editor). Verify with the detected
  // game's own checksum table -- Ruby/Sapphire sizes some sections
  // differently, and the Emerald table would cry wolf on a perfectly healthy
  // RS save.
  const Gen3Game game = EmeraldSave::DetectGame(*parsed);
  if (!parsed->VerifyAllChecksums(game).empty())
    messages << tr("Warning: section checksum(s) invalid in the source save");

  if (game == Gen3Game::FireRedLeafGreen)
  {
    // FRLG keeps its party at different section offsets, so Emerald-offset
    // edits would land inside unrelated data and corrupt the save. It stays
    // fully PLAYABLE (the import path allowed it on an FRLG ROM); it is only
    // this editor that must keep its hands off. The trainer name lives at the
    // same offset in every Gen 3 game, so showing it read-only is safe.
    // m_save stays unset: even if a disabled control fired, every action
    // bails on "No save loaded."
    m_trainer_name_edit->setText(QString::fromStdString(parsed->GetTrainerName()));
    std::string file_name = m_save_path;
    SplitPath(m_save_path, nullptr, &file_name, nullptr);
    m_trainer_label->setText(tr("Trainer ID %1  ·  %2.sav  ·  FireRed/LeafGreen")
                                 .arg(parsed->GetTrainerPublicId())
                                 .arg(QString::fromStdString(file_name)));
    SetEditingEnabled(false);
    SetMessages({tr("This is a FireRed/LeafGreen save. It will be used for play, but the Team "
                    "Editor only edits Ruby/Sapphire/Emerald saves (FRLG stores the party "
                    "elsewhere). Restore the default save to edit a team here.")});
    RefreshPartyList();
    return;
  }

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

  // Ruby/Sapphire stays editable alongside Emerald: the party and trainer
  // fields this editor touches sit at the same section-0/1 offsets in both,
  // and RS's section 1 checksums over the same length as Emerald's. (RS sizes
  // section 0 shorter, but the game zero-fills the tail of every written
  // sector, so the Emerald-length checksum the name edit writes computes to
  // the same value -- and the verified-write re-check refuses the save rather
  // than corrupt it should that assumption ever not hold.)
  std::string file_name = m_save_path;
  SplitPath(m_save_path, nullptr, &file_name, nullptr);
  m_trainer_name_edit->setText(QString::fromStdString(m_save->GetTrainerName()));
  QString trainer_label = tr("Trainer ID %1  ·  %2.sav")
                              .arg(m_save->GetTrainerPublicId())
                              .arg(QString::fromStdString(file_name));
  if (game == Gen3Game::RubySapphire)
    trainer_label += tr("  ·  Ruby/Sapphire");
  m_trainer_label->setText(trainer_label);
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
  // The raise-to-100 action only makes sense for a level-100 format, and only
  // when at least one mon is actually below 100 (never a level-DOWN).
  const int format = Config::Get(Config::MAIN_XD_FORMAT);
  const bool level100_format = FormatRules::FormatFixedLevel(format) == 100;
  const bool any_below = std::any_of(m_party.begin(), m_party.end(),
                                     [](const Gen3Mon& m) { return !m.IsEmpty() && m.level < 100; });
  m_raise_button->setVisible(level100_format);
  m_raise_button->setEnabled(level100_format && any_below);
}

void TeamEditorDialog::SetMessages(const QStringList& messages)
{
  m_log_label->setText(messages.join(QStringLiteral("\n")));
}

void TeamEditorDialog::SetEditingEnabled(bool enabled)
{
  m_trainer_name_edit->setEnabled(enabled);
  m_paste_edit->setEnabled(enabled);
  m_import_button->setEnabled(enabled);
  m_remove_button->setEnabled(enabled);
  m_save_button->setEnabled(enabled);
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
  // QPointer, not a raw this: the worker outlives the dialog if Dolphin quits
  // mid-fetch, and QueueOnObject on a destroyed receiver is undefined.
  QPointer<TeamEditorDialog> self(this);
  std::thread([self, url] {
    Common::HttpRequest request;
    // pokepast.es answers the bare paste URL with a 301 to its canonical form.
    request.FollowRedirects();
    Common::HttpRequest::Response response = request.Get(url);
    if (!self)
      return;
    // QueueOnObject takes a raw receiver; the QPointer copy inside the lambda
    // is what guards the deferred call.
    QueueOnObject(self.data(), [self, response = std::move(response)] {
      if (!self)
        return;
      self->m_fetching = false;
      if (!response)
      {
        self->SetMessages({tr("Could not fetch the paste (network error).")});
        return;
      }
      self->ApplyImportText(std::string(response->begin(), response->end()));
    });
  }).detach();
}

QString TeamEditorDialog::FormatComplianceNote() const
{
  // Free/OU (or an unknown key value): one int compare, no validation at all.
  const int format = Config::Get(Config::MAIN_XD_FORMAT);
  if (!FormatRules::HasTeamRules(format))
    return {};
  // Without game data the editor could not have built a party to check anyway
  // (and the note is a courtesy, never a judge -- when in doubt, stay quiet).
  if (!m_data)
    return {};
  const FormatRules::Verdict verdict = FormatRules::ValidateParty(format, m_party, *m_data);
  if (verdict.ok)
    return {};
  return tr("note: this team is not %1 legal - %2")
      .arg(QString::fromUtf8(FormatRules::FormatDisplayName(format)))
      .arg(QString::fromStdString(verdict.reason));
}

bool TeamEditorDialog::ApplyTrainerName(std::string* error)
{
  if (!m_save)
  {
    if (error)
      *error = "no save loaded";
    return false;
  }

  const std::string typed = m_trainer_name_edit->text().trimmed().toStdString();
  if (typed.empty())
  {
    if (error)
      *error = "trainer name cannot be empty";
    return false;
  }
  if (typed == m_save->GetTrainerName())
    return true;  // unchanged: leave the save (and the party's OT) alone

  // A character the Gen 3 charset cannot hold fails here, naming itself, and
  // nothing is written -- better than storing a '?' the player never chose.
  if (!m_save->SetTrainerName(typed, error))
    return false;

  // Every Pokemon stores its OWN copy of the OT name, so a rename has to
  // re-stamp the party too; otherwise the mons read as traded outsiders in
  // game (the disobedience rules kick in above the badge cap). The OT *ID* is
  // untouched, so they remain this save's Pokemon.
  const auto ot_bytes =
      Gen3Text::Encode(m_save->GetTrainerName(), EmeraldSave::TRAINER_NAME_LEN, error);
  if (!ot_bytes)
    return false;
  for (Gen3Mon& mon : m_party)
    std::copy(ot_bytes->begin(), ot_bytes->end(), mon.ot_name_raw.begin());
  return true;
}

void TeamEditorDialog::ApplyImportText(const std::string& text)
{
  if (!m_save || !m_data)
  {
    SetMessages({tr("No save loaded.")});
    return;
  }

  // Commit the typed trainer name FIRST: MonFactory::Build stamps each mon's
  // OT from the save's trainer block, so importing before the rename lands
  // would build the whole party under the previous owner's name.
  std::string name_error;
  if (!ApplyTrainerName(&name_error))
  {
    SetMessages({tr("Nothing imported — trainer name: %1")
                     .arg(QString::fromStdString(name_error))});
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
    // Paste-time FORMAT feedback: a heads-up, never a refusal -- the import
    // above went through regardless of what the note says.
    if (const QString note = FormatComplianceNote(); !note.isEmpty())
      messages << note;
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

void TeamEditorDialog::OnRaiseToLevel100()
{
  if (!m_data)
    return;
  const int format = Config::Get(Config::MAIN_XD_FORMAT);
  if (FormatRules::FormatFixedLevel(format) != 100)
    return;  // level-100 formats only; never touch a Lv50 (Limited) team
  const int raised = MonFactory::RaisePartyToLevel100(m_party, *m_data);
  SetMessages({raised > 0 ?
                   tr("Raised %1 Pokémon to Lv. 100.").arg(raised) :
                   tr("Every Pokémon is already Lv. 100.")});
  RefreshPartyList();
}

void TeamEditorDialog::OnSave()
{
  if (!m_save || m_save_path.empty())
  {
    SetMessages({tr("No save loaded.")});
    return;
  }
  // The room may have opened after this dialog last loaded (showEvent is the
  // only reload). Writing the guest slot now would overwrite the opponent's
  // submitted party with whatever was loaded before they sent it -- and leave
  // a .bak of their team behind.
  if (Settings::Instance().GetNetPlayServer())
  {
    // Both ports: the guest slot may hold the opponent's submitted team, and
    // the host slot holds the session's privacy disposable when the save is an
    // import -- writing either mid-room is wrong in a different way.
    SetMessages({tr("NOT saved — the team editor is locked while a netplay room is open.")});
    return;
  }
  // Name before party: WriteParty serializes the mons as they stand, and
  // ApplyTrainerName is what brings their OT names in line with a rename.
  std::string error;
  if (!ApplyTrainerName(&error))
  {
    SetMessages({tr("NOT saved — trainer name: %1").arg(QString::fromStdString(error))});
    return;
  }
  if (!m_save->WriteParty(m_party))
  {
    SetMessages({tr("Party is larger than 6.")});
    return;
  }
  if (!VerifiedWriteSaveFile(m_save_path, *m_save, &error))
  {
    SetMessages({tr("NOT saved — %1").arg(QString::fromStdString(error))});
    return;
  }
  QStringList messages{
      tr("Saved %1 Pokémon as %2 to %3 (verified; previous save backed up as .bak)")
          .arg(static_cast<int>(m_party.size()))
          .arg(QString::fromStdString(m_save->GetTrainerName()))
          .arg(QString::fromStdString(m_save_path))};
  // Paste-time FORMAT feedback: the save above went through regardless -- only
  // the hosting/submission gates ever refuse an illegal team.
  if (const QString note = FormatComplianceNote(); !note.isEmpty())
    messages << note;
  SetMessages(messages);
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
