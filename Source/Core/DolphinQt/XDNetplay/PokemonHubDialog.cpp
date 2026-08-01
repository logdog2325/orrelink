// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/XDNetplay/PokemonHubDialog.h"

#include <initializer_list>

#include <QCheckBox>
#include <QFont>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/Settings.h"

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

  connect(m_show_on_startup_check, &QCheckBox::toggled, this, [](bool checked) {
    Settings::GetQSettings().setValue(QStringLiteral("xdnetplay/showlauncheronstartup"), checked);
  });
}
