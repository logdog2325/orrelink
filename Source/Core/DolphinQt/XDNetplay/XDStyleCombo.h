// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QString>

class QComboBox;

// Battle Style dropdowns (trainer model, battle music, battle location), built
// from BattleCustomizer's tables. Shared by the launcher's Battle Style group
// and the netplay room's host dialogs, so both always list the same entries
// with the same labels.
namespace XDNetplay::StyleCombo
{
enum class Table
{
  Model,
  Music,
  Venue,
};

// Fills combo: "Game default" (item data 0) first, then every entry of the
// table with its id as item data. Music and location lists get a separator
// where the experimental tier starts. Labels:
//  - music and locations: "(untested)" for the experimental tier;
//  - locations whose terrain changes Nature Power, Camouflage and Secret Power
//    say so;
//  - models carry no tier wording, only "(no portrait)" when the disc has no
//    pre-rendered bust for the pick.
void Populate(QComboBox* combo, Table table);

// Selects the entry whose item data is id; an id the combo does not carry
// lands on index 0, "Game default". The combo's signals are blocked while it
// does, so a persist-on-change connection does not fire for a prefill.
void SelectId(QComboBox* combo, int id);
}  // namespace XDNetplay::StyleCombo
