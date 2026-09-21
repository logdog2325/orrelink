// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/XDNetplay/XDStyleCombo.h"

#include <span>

#include <QComboBox>
#include <QCoreApplication>
#include <QSignalBlocker>

#include "UICommon/XDNetplay/BattleCustomizer.h"

namespace XDNetplay::StyleCombo
{
namespace
{
using BattleCustomizer::StyleOption;
using BattleCustomizer::Tier;

// These labels lived in the launcher before they were shared; keeping its
// translation context means an existing translation of them still applies.
QString Translate(const char* text)
{
  return QCoreApplication::translate("XDLauncherDialog", text);
}

// Battlefield indices whose rooms are outdoor / cave / water rather than a
// colosseum floor. Terrain is the one way the location pick is not purely
// cosmetic: Nature Power's move, Camouflage's type and Secret Power's side
// effect all resolve from it (and Secret Power is a common Gen 3 TM). Per the
// venue research these entries get a suffix in their dropdown label -- no
// separate warning dialog. Keep in sync with BattleCustomizer's VenueTable.
constexpr int TERRAIN_VENUES[] = {5,  7,  10, 12, 14, 15, 16, 17, 19, 21, 22, 23, 39,
                                  42, 44, 45, 49, 50, 53, 54, 55, 56, 57, 58, 59};

bool VenueAltersTerrain(int id)
{
  for (const int terrain_id : TERRAIN_VENUES)
  {
    if (terrain_id == id)
      return true;
  }
  return false;
}

std::span<const StyleOption> TableEntries(Table table)
{
  switch (table)
  {
  case Table::Model:
    return BattleCustomizer::ModelTable();
  case Table::Music:
    return BattleCustomizer::MusicTable();
  case Table::Venue:
    return BattleCustomizer::VenueTable();
  }
  return {};
}

QString OptionLabel(Table table, const StyleOption& option)
{
  const bool model_table = table == Table::Model;
  const QString name = QString::fromUtf8(option.name);
  // Model lists get no tier presentation (the field has disproven "untested"
  // there): their one distinction is whether the pick has a pre-rendered
  // bust, said in the label as "(no portrait)".
  const bool untested = !model_table && option.tier == Tier::Experimental;
  const bool no_portrait = model_table && !BattleCustomizer::ModelHasPortrait(option.id);
  const bool terrain = table == Table::Venue && VenueAltersTerrain(option.id);
  if (untested && terrain)
    return Translate("%1 (untested, alters Nature Power etc.)").arg(name);
  if (untested)
    return Translate("%1 (untested)").arg(name);
  if (terrain)
    return Translate("%1 (alters Nature Power etc.)").arg(name);
  if (no_portrait)
    return Translate("%1 (no portrait)").arg(name);
  return name;
}
}  // namespace

void Populate(QComboBox* combo, Table table)
{
  combo->addItem(Translate("Game default"), 0);
  bool past_tested = false;
  for (const StyleOption& option : TableEntries(table))
  {
    // Music/venue tables list tested-safe entries first; the tier change is
    // where the "here be dragons" separator goes.
    if (table != Table::Model && !past_tested && option.tier == Tier::Experimental)
    {
      combo->insertSeparator(combo->count());
      past_tested = true;
    }
    combo->addItem(OptionLabel(table, option), option.id);
  }
}

void SelectId(QComboBox* combo, int id)
{
  const QSignalBlocker blocker(combo);
  const int index = combo->findData(id);
  combo->setCurrentIndex(index >= 0 ? index : 0);
}
}  // namespace XDNetplay::StyleCombo
