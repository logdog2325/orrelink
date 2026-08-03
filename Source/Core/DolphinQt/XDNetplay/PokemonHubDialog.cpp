// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/XDNetplay/PokemonHubDialog.h"

#include <initializer_list>
#include <thread>
#include <utility>

#include <QCheckBox>
#include <QDesktopServices>
#include <QFont>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/Settings.h"

#include "UICommon/XDNetplay/Version.h"

PokemonHubDialog::PokemonHubDialog(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("Pokémon Hub"));

  CreateMainLayout();
  ConnectWidgets();
}

bool PokemonHubDialog::ShowOnStartup()
{
  // Same key XDLauncherDialog's own "show at startup" checkbox writes: one
  // setting reachable from either screen, so whichever one the user is looking
  // at can turn the startup popup off.
  return Settings::GetQSettings()
      .value(QStringLiteral("xdnetplay/showlauncheronstartup"), true)
      .toBool();
}

void PokemonHubDialog::showEvent(QShowEvent* event)
{
  QDialog::showEvent(event);
  // The XD launcher carries the same toggle, so this one can be stale by the
  // time the hub is reopened from the Tools menu.
  const QSignalBlocker blocker(m_show_on_startup_check);
  m_show_on_startup_check->setChecked(ShowOnStartup());
}

void PokemonHubDialog::CreateMainLayout()
{
  auto* layout = new QVBoxLayout;

  auto* title = new QLabel(tr("Pokémon Battles"));
  QFont title_font = title->font();
  title_font.setBold(true);
  // pointSizeF() is -1 whenever the active font was sized in pixels instead,
  // which would turn a naive bump into unreadably small text.
  if (title_font.pointSizeF() > 0.0)
    title_font.setPointSizeF(title_font.pointSizeF() * 1.4);
  title->setFont(title_font);
  layout->addWidget(title);

  auto* subtitle = new QLabel(
      tr("Choose a mode. Each launcher applies its own settings when it boots, so the two never "
         "step on each other."));
  subtitle->setWordWrap(true);
  layout->addWidget(subtitle);

  // Deliberately oversized: this is the first screen a new player sees, and the
  // only decision it asks for is which of the two it should be.
  m_xd_button = new NonDefaultQPushButton(tr("Pokémon XD — GBA-vs-GBA netplay"));
  m_pbr_button = new NonDefaultQPushButton(tr("Pokémon Battle Revolution — Wiimmfi online"));
  for (QPushButton* button : {m_xd_button, m_pbr_button})
  {
    button->setMinimumHeight(48);
    layout->addWidget(button);
  }

  m_update_button = new NonDefaultQPushButton(tr("Check for Updates"));
  layout->addWidget(m_update_button);

  auto* version_label = new QLabel(tr("OrreLink %1").arg(QString::fromUtf8(XDNetplay::VERSION)));
  layout->addWidget(version_label);

  m_show_on_startup_check = new QCheckBox(tr("Show this at startup"));
  m_show_on_startup_check->setChecked(ShowOnStartup());
  layout->addWidget(m_show_on_startup_check);

  setLayout(layout);
}

void PokemonHubDialog::ConnectWidgets()
{
  // Close first, then hand off: the hub has served its purpose once a mode is
  // picked, and leaving it stacked behind the launcher only gives the user a
  // second window to dismiss.
  connect(m_xd_button, &QPushButton::clicked, this, [this] {
    close();
    emit OpenXDLauncher();
  });
  connect(m_pbr_button, &QPushButton::clicked, this, [this] {
    close();
    emit OpenPbrLauncher();
  });

  connect(m_update_button, &QPushButton::clicked, this, &PokemonHubDialog::CheckForUpdates);

  connect(m_show_on_startup_check, &QCheckBox::toggled, this, [](bool checked) {
    Settings::GetQSettings().setValue(QStringLiteral("xdnetplay/showlauncheronstartup"), checked);
  });
}

void PokemonHubDialog::CheckForUpdates()
{
  if (m_checking_for_update)
    return;

  m_checking_for_update = true;
  m_update_button->setEnabled(false);
  m_update_button->setText(tr("Checking…"));

  // QPointer, not a raw this: the request outlives the hub whenever the user closes it mid-check,
  // and QueueOnObject on a destroyed receiver is undefined.
  QPointer<PokemonHubDialog> self(this);
  std::thread([self] {
    XDNetplay::UpdateCheckResult result = XDNetplay::CheckForUpdate();
    if (!self)
      return;
    // QueueOnObject takes a raw receiver; the QPointer copy inside the lambda is what guards the
    // deferred call.
    QueueOnObject(self.data(), [self, result = std::move(result)] {
      if (!self)
        return;
      self->ShowUpdateResult(result);
    });
  }).detach();
}

void PokemonHubDialog::ShowUpdateResult(const XDNetplay::UpdateCheckResult& result)
{
  m_checking_for_update = false;
  m_update_button->setEnabled(true);
  m_update_button->setText(tr("Check for Updates"));

  const bool up_to_date = result.status == XDNetplay::Status::UpToDate;
  const bool update_available = result.status == XDNetplay::Status::UpdateAvailable;

  ModalMessageBox box(this);
  box.setWindowTitle(tr("Check for Updates"));
  box.setIcon(up_to_date || update_available ? QMessageBox::Information : QMessageBox::Warning);
  // Everything below carries text straight from the release metadata, and a QMessageBox label
  // defaults to Qt::AutoText with external links enabled -- so an unescaped "<a href=...>" in a
  // release title would render as a live link that opens an arbitrary URL on one click, including
  // on the up-to-date path where no download button is offered at all. Escape at the point of
  // conversion, as the chat log does (NetPlayDialog::AppendChat). Escaping the argument before
  // .arg() also keeps the translated literal itself untouched.
  box.setText(QString::fromStdString(result.message).toHtmlEscaped());

  // Only ever populated by a fetch that parsed, so this doubles as "we actually heard back".
  if (!result.release_name.empty())
  {
    const QString name = QString::fromStdString(result.release_name).toHtmlEscaped();
    box.setInformativeText(
        result.published_at.empty() ?
            name :
            tr("%1 — published %2")
                .arg(name, QString::fromStdString(result.published_at).toHtmlEscaped()));
  }

  // Nothing is downloaded here; the release page is where the user picks a build for their
  // platform. Offered for Unknown too: that verdict means the user has to judge the tags
  // themselves.
  const bool unknown_with_tag =
      result.status == XDNetplay::Status::Unknown && !result.latest_tag.empty();
  const bool offer_page = !result.html_url.empty() && (update_available || unknown_with_tag);

  QPushButton* open_button = nullptr;
  if (offer_page)
    open_button = box.addButton(tr("Open Download Page"), QMessageBox::ActionRole);
  box.addButton(QMessageBox::Close);

  box.exec();

  if (open_button && box.clickedButton() == open_button)
    QDesktopServices::openUrl(QUrl(QString::fromStdString(result.html_url)));
}
