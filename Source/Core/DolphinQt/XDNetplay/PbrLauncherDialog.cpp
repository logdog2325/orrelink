// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/XDNetplay/PbrLauncherDialog.h"

#include <initializer_list>
#include <string>
#include <string_view>

#include <QAbstractItemModel>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"

#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/XDNetplay/PbrWiimmfi.h"

#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/Settings.h"

#include "UICommon/GameFile.h"

namespace
{
// PbrSaveData is a fixed-size image; PBR itself never writes any other length.
constexpr u64 PBR_SAVE_SIZE = 0x380000;

// Ok = done, Warn = worth fixing but never blocks Play, Missing = the one thing
// that actually stops a boot. Same three-state vocabulary as the Android
// launcher, because the two screens describe the same setup.
enum class RowState
{
  Ok,
  Warn,
  Missing,
};

void SetRowState(QLabel* status, RowState state)
{
  switch (state)
  {
  case RowState::Ok:
    status->setText(QStringLiteral("✔"));
    status->setStyleSheet(QStringLiteral("QLabel { color: green; }"));
    break;
  case RowState::Warn:
    status->setText(QStringLiteral("!"));
    status->setStyleSheet(QStringLiteral("QLabel { color: orange; }"));
    break;
  case RowState::Missing:
    status->setText(QStringLiteral("✘"));
    status->setStyleSheet(QStringLiteral("QLabel { color: red; }"));
    break;
  }
}

// A disc channel's title ID is 0x00010000 followed by the game ID's first four
// characters packed big-endian, which is exactly the "<title-hi>/<title-lo>"
// pair Common::GetTitleDataPath formats. Deriving it beats a lookup table:
// RPBE01 -> 0x0001000052504245, RPBP01 -> ...52504250, RPBJ01 -> ...5250424a.
u64 PbrTitleId(std::string_view game_id)
{
  u64 title_lo = 0;
  for (size_t i = 0; i < 4; i++)
    title_lo = (title_lo << 8) | static_cast<u8>(game_id[i]);
  return (0x00010000ULL << 32) | title_lo;
}

// <user dir>/Wii/title/00010000/<title-lo>/data/GeniusPbr/PbrSaveData.
std::string SavePathForGameId(std::string_view game_id)
{
  return Common::GetTitleDataPath(PbrTitleId(game_id), Common::FromWhichRoot::Configured) +
         "/GeniusPbr/PbrSaveData";
}

std::string BundledSavePath()
{
  return File::GetSysDirectory() + "XDNetplay" DIR_SEP "PbrSaveData";
}

// Desktop deliberately does not carry PBR's save cipher. Android needs it
// because it ships the starter save decrypted and gzipped (551 KB instead of
// 3.5 MB -- the encrypted form does not compress), so it has to re-encrypt at
// install time and can verify by loading the result back. The desktop build
// ships the already-encrypted image, so size is the only check available here:
// a wrong-size file is certainly not a PBR save, and a right-size one cannot be
// second-guessed without the cipher.
bool LooksLikePbrSave(const std::string& path)
{
  return File::Exists(path) && File::GetSize(path) == PBR_SAVE_SIZE;
}

enum class InstallResult
{
  Ok,
  BadSource,
  CopyFailed,
  RenameFailed,
};

// Writes through <target>.tmp and renames, so a half-written 3.5 MB image can
// never land in the NAND -- PBR treats a truncated save as corrupt and wipes the
// profile. An existing save is moved aside to <target>.userbak instead of being
// destroyed: once a player has their own progress and online identity in there,
// losing it is unrecoverable and we are not the ones who should decide that.
InstallResult InstallSave(const std::string& source, const std::string& target)
{
  if (!LooksLikePbrSave(source))
    return InstallResult::BadSource;

  // A leftover .tmp from an interrupted run is expected, not an anomaly, so all
  // of these deletes stay quiet about a missing file.
  constexpr auto quiet = File::IfAbsentBehavior::NoConsoleWarning;

  const std::string temp = target + ".tmp";
  File::CreateFullPath(target);
  File::Delete(temp, quiet);
  if (!File::CopyRegularFile(source, temp) || File::GetSize(temp) != PBR_SAVE_SIZE)
  {
    File::Delete(temp, quiet);
    return InstallResult::CopyFailed;
  }

  // Never clobber an earlier backup: std::filesystem::rename would silently
  // replace it, so the FIRST save the user ever had -- the one most worth
  // keeping -- would be lost on a second install. Find a free slot instead.
  std::string backup = target + ".userbak";
  for (int i = 1; File::Exists(backup) && i < 100; ++i)
    backup = target + ".userbak." + std::to_string(i);
  if (File::Exists(target) && !File::Rename(target, backup))
  {
    File::Delete(temp, quiet);
    return InstallResult::RenameFailed;
  }

  // Checked, not fire-and-forget: a failed rename here leaves the user with no
  // save at all while the checklist would happily claim success.
  if (!File::Rename(temp, target))
  {
    File::Delete(temp, quiet);
    return InstallResult::RenameFailed;
  }
  return InstallResult::Ok;
}

// What Wiimmfi actually wants for the online modes is the console identity that
// comes with a real BootMii backup. The System Menu TMD is the cheapest on-disk
// marker that any NAND was installed at all; offline play needs none of it, so
// this row is informational and never blocks Play.
bool NandInstalled()
{
  return File::Exists(
      Common::GetTMDFileName(Titles::SYSTEM_MENU, Common::FromWhichRoot::Configured));
}

// PBR is a Wii game and must not spin up Emerald cores. The XD launcher sets SI
// ports 2/3 to Emulated GBA for its GBA-vs-GBA link and it auto-opens at
// startup, so by the time anyone reaches this dialog those ports are usually
// still armed. XD re-asserts them from EnsureGbaConfig on every one of its own
// boots, so clearing them here cannot break the XD flow.
void EnsurePbrConfig()
{
  for (int channel : {1, 2})
  {
    if (Config::Get(Config::GetInfoForSIDevice(channel)) ==
        SerialInterface::SIDEVICE_GC_GBA_EMULATED)
    {
      Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(channel), SerialInterface::SIDEVICE_NONE);
    }
  }
  Config::Save();
}
}  // namespace

PbrLauncherDialog::PbrLauncherDialog(const GameListModel& game_list_model, QWidget* parent)
    : QDialog(parent), m_game_list_model(game_list_model)
{
  setWindowTitle(tr("PBR Launcher"));

  CreateMainLayout();
  ConnectWidgets();
}

void PbrLauncherDialog::CreateMainLayout()
{
  auto* layout = new QVBoxLayout;

  auto* checklist_box = new QGroupBox(tr("Setup Checklist"));
  auto* checklist_layout = new QGridLayout;
  int row = 0;
  const auto add_row = [&](ChecklistRow* target, const QString& description,
                           const QString& fix_label, const QString& alt_label) {
    target->status = new QLabel(QStringLiteral("✘"));
    target->description = new QLabel(description);
    // The rows carry full sentences, not labels; without this they force the
    // dialog as wide as the longest one.
    target->description->setWordWrap(true);
    checklist_layout->addWidget(target->status, row, 0);
    checklist_layout->addWidget(target->description, row, 1);
    if (!fix_label.isEmpty())
    {
      target->fix_button = new NonDefaultQPushButton(fix_label);
      checklist_layout->addWidget(target->fix_button, row, 2);
    }
    if (!alt_label.isEmpty())
    {
      target->alt_button = new NonDefaultQPushButton(alt_label);
      checklist_layout->addWidget(target->alt_button, row, 3);
    }
    row++;
  };
  add_row(&m_game_row, tr("Pokémon Battle Revolution disc"), tr("Choose disc..."), QString());
  // Two buttons rather than a modal fork: installing the bundled starter save
  // and installing your own are both one-click actions, and hiding one behind a
  // question box would make the common case slower for no gain.
  add_row(&m_save_row, tr("PBR save"), tr("Install starter save"), tr("Use my own..."));
  add_row(&m_nand_row, tr("Wii NAND"), QString(), QString());
  checklist_layout->setColumnStretch(1, 1);
  checklist_box->setLayout(checklist_layout);
  layout->addWidget(checklist_box);

  m_play_button = new NonDefaultQPushButton(tr("Play PBR"));
  m_play_button->setEnabled(false);
  layout->addWidget(m_play_button);

  auto* note = new QLabel(
      tr("Bring a clean dump in any format — the Wiimmfi redirect is applied in memory at boot, so "
         "your file is never modified and a disc that is already patched is left alone. No SSL "
         "certificates, no DNS change, no activation wait."));
  note->setWordWrap(true);
  layout->addWidget(note);

  setLayout(layout);
}

void PbrLauncherDialog::ConnectWidgets()
{
  // The game scan is asynchronous: without this the disc row stays red until
  // the user reopens the dialog, even though the pick succeeded.
  connect(&m_game_list_model, &QAbstractItemModel::rowsInserted, this,
          [this] { RefreshChecklist(); });
  connect(&m_game_list_model, &QAbstractItemModel::modelReset, this,
          [this] { RefreshChecklist(); });

  connect(m_game_row.fix_button, &QPushButton::clicked, this, &PbrLauncherDialog::OnFixGamePath);
  connect(m_save_row.fix_button, &QPushButton::clicked, this,
          &PbrLauncherDialog::OnInstallBundledSave);
  connect(m_save_row.alt_button, &QPushButton::clicked, this, &PbrLauncherDialog::OnInstallOwnSave);
  connect(m_play_button, &QPushButton::clicked, this, &PbrLauncherDialog::OnPlay);
}

void PbrLauncherDialog::showEvent(QShowEvent* event)
{
  QDialog::showEvent(event);
  RefreshChecklist();
}

void PbrLauncherDialog::RefreshChecklist()
{
  const auto game = FindPbrGame();
  const std::string game_id = game ? game->GetGameID() : std::string();
  const bool wiimmfi_supported = game && XDNetplay::PbrWiimmfi::IsSupportedGameID(game_id);

  SetRowState(m_game_row.status, !game ? RowState::Missing :
                                         (wiimmfi_supported ? RowState::Ok : RowState::Warn));
  if (!game)
  {
    m_game_row.description->setText(
        tr("Pokémon Battle Revolution not found — choose your dump (ISO/RVZ/WBFS/CISO). "
           "A clean dump is fine; it does not need to be pre-patched."));
  }
  else if (wiimmfi_supported)
  {
    m_game_row.description->setText(
        tr("Pokémon Battle Revolution found (%1) — the Wiimmfi patch is applied in memory at "
           "boot, so your dump is never modified.")
            .arg(QString::fromStdString(game_id)));
  }
  else
  {
    m_game_row.description->setText(
        tr("Pokémon Battle Revolution found (%1), but that region has no Wiimmfi payload. Online "
           "play needs the US (RPBE01) or European (RPBP01) disc; this one still boots offline.")
            .arg(QString::fromStdString(game_id)));
  }

  const std::string save_path = DetectedSavePath();
  // Validate, do not merely exist-check: a truncated or zero-byte PbrSaveData
  // (a half-finished manual copy, a crashed tool) would otherwise show a green
  // tick AND disable the install button, hiding the very problem to fix.
  const bool save_present = File::Exists(save_path);
  const bool save_installed = LooksLikePbrSave(save_path);
  const bool bundled_available = LooksLikePbrSave(BundledSavePath());
  SetRowState(m_save_row.status, save_installed ? RowState::Ok : RowState::Warn);
  if (save_installed)
  {
    m_save_row.description->setText(
        tr("PBR save installed for %1.")
            .arg(QString::fromStdString(game ? game_id : std::string("RPBE01"))));
  }
  else if (save_present)
  {
    // Present but the wrong size: say which problem it is, or the user stares
    // at "no save yet" while a file is sitting right there.
    m_save_row.description->setText(
        tr("A PbrSaveData file is there but is not a valid PBR save (wrong size). Replace it with "
           "the bundled starter save or your own backup."));
  }
  else if (bundled_available)
  {
    m_save_row.description->setText(
        tr("No PBR save yet — the game only writes one after it has been played, which leaves the "
           "team editor with nothing to read. Install the bundled starter save, or your own."));
  }
  else
  {
    // The starter save is a build asset (Sys/XDNetplay/PbrSaveData). A build
    // that shipped without it must still be usable, so say so plainly rather
    // than offering a button that can only fail.
    m_save_row.description->setText(
        tr("No PBR save yet, and this build has no bundled starter save. Install your own PBR "
           "save file, or play the game once to let it create one."));
  }
  // Never offered as a way to overwrite: the button only makes sense while
  // there is nothing to lose. Installing over an existing save stays possible
  // through "Use my own...", which asks first and keeps a .userbak.
  m_save_row.fix_button->setEnabled(bundled_available && !save_installed);

  const bool nand = NandInstalled();
  SetRowState(m_nand_row.status, nand ? RowState::Ok : RowState::Warn);
  m_nand_row.description->setText(
      nand ? tr("Wii NAND installed. Online play uses the console identity it carries — make sure "
                "it is a backup from your own Wii.") :
             tr("No Wii NAND installed. Offline play works fine without one, but online play needs "
                "a NAND backup from YOUR OWN Wii — shared or generated NANDs get rejected and can "
                "get you banned. Import one under Tools > Manage NAND > Import BootMii NAND "
                "Backup..."));

  // Deliberately gated on the disc alone, not on Wiimmfi support: an
  // unsupported-region disc (RPBJ01) still boots and plays offline, and the
  // disc row already spells out what the user loses.
  m_play_button->setEnabled(game != nullptr);
}

std::shared_ptr<const UICommon::GameFile> PbrLauncherDialog::FindPbrGame() const
{
  // Prefer a region we can actually redirect to Wiimmfi: a user with both a
  // Japanese and a US dump in the list should get the one that works online.
  std::shared_ptr<const UICommon::GameFile> fallback;
  for (int i = 0; i < m_game_list_model.rowCount(QModelIndex()); i++)
  {
    auto game = m_game_list_model.GetGameFile(i);
    if (!game || !XDNetplay::PbrWiimmfi::IsPbrGameID(game->GetGameID()))
      continue;
    if (XDNetplay::PbrWiimmfi::IsSupportedGameID(game->GetGameID()))
      return game;
    if (!fallback)
      fallback = game;
  }
  return fallback;
}

std::string PbrLauncherDialog::DetectedSavePath() const
{
  const auto game = FindPbrGame();
  const std::string game_id = game ? game->GetGameID() : std::string();
  // A game ID shorter than four characters cannot produce a title-lo; the USA
  // slot is the same fallback the Android build uses for an unknown ID.
  if (game_id.size() < 4)
    return SavePathForGameId(XDNetplay::PbrWiimmfi::GAME_ID_USA);
  return SavePathForGameId(game_id);
}

void PbrLauncherDialog::OnFixGamePath()
{
  // A file picker, not a folder picker: users know where their dump is, not
  // what a "game folder" means. The game list still works on directories, so
  // register the file's parent directory. Same trade-off the XD launcher makes.
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Select your Pokémon Battle Revolution dump"), QString(),
      tr("Wii disc images (*.iso *.wbfs *.rvz *.ciso *.gcz *.wia)"));
  if (!path.isEmpty())
  {
    std::string parent_dir;
    SplitPath(path.toStdString(), &parent_dir, nullptr, nullptr);
    if (!parent_dir.empty())
      Settings::Instance().AddPath(QString::fromStdString(parent_dir));
  }
  RefreshChecklist();
}

void PbrLauncherDialog::OnInstallBundledSave()
{
  const std::string target = DetectedSavePath();
  if (File::Exists(target))
  {
    // Should be unreachable (the button is disabled in that state), but the
    // checklist can be stale if something wrote the save behind our back.
    ModalMessageBox::information(this, tr("PBR Launcher"),
                                 tr("You already have a PBR save. The bundled starter save is "
                                    "only offered when there is nothing to lose."));
    RefreshChecklist();
    return;
  }

  const InstallResult result = InstallSave(BundledSavePath(), target);
  if (result == InstallResult::BadSource)
  {
    ModalMessageBox::warning(this, tr("PBR Launcher"),
                             tr("This build does not ship a usable starter save. Install your own "
                                "PBR save file with \"Use my own...\" instead."));
  }
  else if (result != InstallResult::Ok)
  {
    ModalMessageBox::warning(
        this, tr("PBR Launcher"),
        tr("Could not write the PBR save to:\n\n%1").arg(QString::fromStdString(target)));
  }
  RefreshChecklist();
}

void PbrLauncherDialog::OnInstallOwnSave()
{
  const QString path = QFileDialog::getOpenFileName(this, tr("Select a PBR save file"), QString(),
                                                    tr("PBR save (PbrSaveData);;All files (*)"));
  if (path.isEmpty())
    return;

  const std::string source = path.toStdString();
  if (!LooksLikePbrSave(source))
  {
    ModalMessageBox::warning(this, tr("PBR Launcher"),
                             tr("That file is not a PBR save: a PbrSaveData image is exactly "
                                "3,670,016 bytes."));
    return;
  }

  const std::string target = DetectedSavePath();
  if (File::Exists(target) &&
      ModalMessageBox::question(
          this, tr("PBR Launcher"),
          tr("You already have a PBR save. Replace it?\n\nThe current save is kept next to it as "
             "PbrSaveData.userbak.")) != QMessageBox::Yes)
  {
    return;
  }

  if (InstallSave(source, target) != InstallResult::Ok)
  {
    ModalMessageBox::warning(
        this, tr("PBR Launcher"),
        tr("Could not write the PBR save to:\n\n%1").arg(QString::fromStdString(target)));
  }
  RefreshChecklist();
}

void PbrLauncherDialog::OnPlay()
{
  const auto game = FindPbrGame();
  if (!game)
  {
    ModalMessageBox::warning(
        this, tr("PBR Launcher"),
        tr("Pokémon Battle Revolution was not found in your game list. Add its folder first."));
    return;
  }

  EnsurePbrConfig();
  emit BootPbr(QString::fromStdString(game->GetFilePath()));
}
