// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.Gen3Mon

@Composable
fun TeamEditorScreen(
    state: TeamEditorState,
    names: DisplayNames?,
    onRoleChange: (TeamRole) -> Unit,
    onSelect: (Int) -> Unit,
    onTrainerNameChange: (String) -> Unit,
    onImport: (String) -> Unit,
    onRemoveSelected: () -> Unit,
    onSave: () -> Unit,
    onShare: () -> Unit,
    onBack: () -> Unit
) {
    var showImport by remember { mutableStateOf(false) }

    SeaBackground {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(16.dp)
        ) {
            // Title bar
            SeaPanel(title = null) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("XD NETPLAY", color = XDColors.TextAccent, fontWeight = FontWeight.Bold)
                    Text("  ✦  ", color = XDColors.TextDim)
                    Text("Team Editor", color = XDColors.TextPrimary)
                    Spacer(Modifier.weight(1f))
                    TextButton(onClick = onBack) { Text("Back", color = XDColors.TextDim) }
                }
            }
            Spacer(Modifier.height(12.dp))

            // Role selector
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TeamRole.entries.forEach { role ->
                    val selected = role == state.role
                    Text(
                        text = role.label,
                        color = if (selected) XDColors.ChipText else XDColors.TextDim,
                        fontSize = 13.sp,
                        fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
                        modifier = Modifier
                            .background(
                                if (selected) XDColors.Accent else XDColors.Panel,
                                RoundedCornerShape(16.dp)
                            )
                            .border(1.dp, XDColors.PanelLight, RoundedCornerShape(16.dp))
                            .clickable { onRoleChange(role) }
                            .padding(horizontal = 14.dp, vertical = 6.dp)
                    )
                }
            }
            Spacer(Modifier.height(6.dp))
            // Which slot matters where is the #1 point of confusion for
            // joiners — say it right here, per selected role.
            Text(
                text = if (state.role == TeamRole.HOST)
                    "Your team: plays when you host, and it is what \"Use my save\" submits when you join someone else's room."
                else
                    "Only used in rooms YOU host (the fallback team a joining guest plays if they never submit one) and in solo play — it does not affect rooms you join.",
                color = XDColors.TextDim,
                fontSize = 12.sp
            )
            Spacer(Modifier.height(14.dp))

            if (!state.ready) {
                SeaPanel(title = "SETUP NEEDED") {
                    state.messages.forEach { Text(it, color = XDColors.TextPrimary) }
                    // Suppressed when the messages already fully explain the
                    // lock (imported FRLG save): the ROM is configured fine
                    // there, so this hint would point at the wrong fix.
                    if (state.setupHint) {
                        Text(
                            "Set your Emerald ROM in Settings → GBA, then come back.",
                            color = XDColors.TextDim
                        )
                    }
                }
                return@Column
            }

            // Trainer name. Keystrokes that the Gen 3 charset cannot hold are
            // rejected as they are typed (see TeamEditorActivity.setTrainerName),
            // so the field can only ever show a name the save can really store.
            SeaPanel(title = "TRAINER") {
                OutlinedTextField(
                    value = state.trainerName,
                    onValueChange = onTrainerNameChange,
                    singleLine = true,
                    label = { Text("In-game name (max 7)", color = XDColors.TextDim) },
                    supportingText = {
                        Text(
                            "Shown to your opponent in the link battle. " +
                                "Renaming also re-stamps the party's OT.",
                            color = XDColors.TextDim,
                            fontSize = 11.sp
                        )
                    },
                    isError = state.trainerName.isBlank(),
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedTextColor = XDColors.TextPrimary,
                        unfocusedTextColor = XDColors.TextPrimary,
                        focusedBorderColor = XDColors.Accent,
                        unfocusedBorderColor = XDColors.PanelLight
                    ),
                    modifier = Modifier.fillMaxWidth()
                )
            }
            Spacer(Modifier.height(14.dp))

            // Party panel
            SeaPanel(title = "PARTY — ${state.trainer}") {
                if (state.party.isEmpty()) {
                    Text(
                        "No Pokémon yet. Import a Showdown team or a pokepast.es link.",
                        color = XDColors.TextDim
                    )
                }
                state.party.forEachIndexed { i, mon ->
                    MonRow(
                        mon = mon,
                        names = names,
                        selected = i == state.selected,
                        onClick = { onSelect(i) }
                    )
                }
            }
            Spacer(Modifier.height(14.dp))

            // Detail panel
            state.party.getOrNull(state.selected)?.let { mon ->
                SeaPanel(title = "DETAIL") {
                    MonDetail(mon = mon, names = names, onRemove = onRemoveSelected)
                }
                Spacer(Modifier.height(14.dp))
            }

            // Actions
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Button(
                    onClick = { showImport = true },
                    colors = ButtonDefaults.buttonColors(
                        containerColor = XDColors.Accent,
                        contentColor = XDColors.ChipText
                    ),
                    modifier = Modifier.weight(1f)
                ) { Text("Import") }
                Button(
                    onClick = onSave,
                    // A blank name cannot be written at all, so do not offer to.
                    enabled = state.dirty && state.trainerName.isNotBlank(),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = XDColors.PanelLight,
                        contentColor = XDColors.ChipText
                    ),
                    modifier = Modifier.weight(1f)
                ) { Text(if (state.dirty) "Save •" else "Saved") }
                Button(
                    onClick = onShare,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = XDColors.PanelLight,
                        contentColor = XDColors.ChipText
                    ),
                    modifier = Modifier.weight(1f)
                ) { Text("Share") }
            }
            Spacer(Modifier.height(12.dp))

            // Status / diagnostics — always honest, always visible
            if (state.messages.isNotEmpty()) {
                SeaPanel(title = "STATUS") {
                    state.messages.forEach {
                        val warn = it.startsWith("NOT") || it.startsWith("Warning") ||
                            it.startsWith("Skipped") || it.startsWith("Could not")
                        Text(
                            text = it,
                            color = if (warn) XDColors.Accent else XDColors.TextDim,
                            fontSize = 13.sp
                        )
                    }
                }
            }
        }
    }

    if (showImport) {
        ImportDialog(
            onDismiss = { showImport = false },
            onImport = {
                showImport = false
                onImport(it)
            }
        )
    }
}

@Composable
private fun MonRow(mon: Gen3Mon, names: DisplayNames?, selected: Boolean, onClick: () -> Unit) {
    val speciesName = names?.species?.get(mon.species) ?: "#${mon.species}"
    val itemName = if (mon.heldItem == 0) "—" else names?.items?.get(mon.heldItem) ?: "#${mon.heldItem}"
    val shiny = isShiny(mon)
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .background(
                if (selected) XDColors.PanelLight.copy(alpha = 0.75f) else Color.Transparent,
                RoundedCornerShape(10.dp)
            )
            .clickable(onClick = onClick)
            .padding(horizontal = 8.dp, vertical = 6.dp)
    ) {
        // Monogram slot, pastel-tinted by species — gold-ringed and starred when shiny
        Box(
            contentAlignment = Alignment.Center,
            modifier = Modifier
                .size(40.dp)
                .background(XDColors.pastelFor(mon.species), CircleShape)
                .border(
                    width = if (shiny) 2.dp else 1.dp,
                    color = if (shiny) XDColors.Shiny else Color.White,
                    shape = CircleShape
                )
        ) {
            Text(
                text = speciesName.take(1).uppercase(),
                color = XDColors.ChipText,
                fontWeight = FontWeight.Bold
            )
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = mon.nickname.ifEmpty { speciesName },
                    color = if (selected) XDColors.TextAccent else XDColors.TextPrimary,
                    fontWeight = FontWeight.SemiBold
                )
                if (shiny) Text("  ★", color = XDColors.Shiny)
            }
            Text(
                text = "$speciesName · ${mon.natureName}",
                color = XDColors.TextDim,
                fontSize = 12.sp
            )
        }
        Column(horizontalAlignment = Alignment.End) {
            Text("Lv ${mon.level}", color = XDColors.TextPrimary, fontSize = 13.sp)
            Text(itemName, color = XDColors.TextDim, fontSize = 12.sp)
        }
    }
}

@Composable
private fun MonDetail(mon: Gen3Mon, names: DisplayNames?, onRemove: () -> Unit) {
    val ivs = mon.ivs()
    val evTotal = mon.evs.sum()
    StatBlock(
        labels = listOf("HP", "ATK", "DEF", "SPE", "SPA", "SPD"),
        stats = listOf(mon.maxHp, mon.attack, mon.defense, mon.speed, mon.spAttack, mon.spDefense),
        evs = listOf(mon.evs[0], mon.evs[1], mon.evs[2], mon.evs[3], mon.evs[4], mon.evs[5]),
        ivs = listOf(ivs[0], ivs[1], ivs[2], ivs[3], ivs[4], ivs[5])
    )
    Spacer(Modifier.height(6.dp))
    Text(
        text = "EV total $evTotal / 510",
        color = if (evTotal > 510) XDColors.Accent else XDColors.TextDim,
        fontSize = 12.sp
    )
    Spacer(Modifier.height(8.dp))
    Text("MOVES", color = XDColors.TextAccent, fontSize = 12.sp, fontWeight = FontWeight.Bold)
    mon.moves.filter { it != 0 }.forEach { moveId ->
        Text(
            text = "· " + (names?.moves?.get(moveId) ?: "#$moveId"),
            color = XDColors.TextPrimary,
            fontSize = 14.sp
        )
    }
    Spacer(Modifier.height(8.dp))
    TextButton(onClick = onRemove) { Text("Remove from party", color = XDColors.Accent) }
}

@Composable
private fun StatBlock(labels: List<String>, stats: List<Int>, evs: List<Int>, ivs: List<Int>) {
    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        labels.forEachIndexed { i, label ->
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text(label, color = XDColors.TextAccent, fontSize = 11.sp, fontWeight = FontWeight.Bold)
                Text("${stats[i]}", color = XDColors.TextPrimary, fontSize = 15.sp)
                Text("${evs[i]}", color = XDColors.TextDim, fontSize = 10.sp)
                Text("${ivs[i]}", color = XDColors.PanelLight, fontSize = 10.sp)
            }
        }
    }
    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
        Text("stat / EV / IV", color = XDColors.TextDim, fontSize = 9.sp)
    }
}

@Composable
private fun ImportDialog(onDismiss: () -> Unit, onImport: (String) -> Unit) {
    var text by remember { mutableStateOf("") }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = Color(0xFF183454),
        title = { Text("Import team", color = XDColors.TextPrimary) },
        text = {
            Column {
                Text(
                    "Paste a Showdown team export or a pokepast.es link.",
                    color = XDColors.TextDim,
                    fontSize = 13.sp
                )
                Spacer(Modifier.height(8.dp))
                OutlinedTextField(
                    value = text,
                    onValueChange = { text = it },
                    minLines = 6,
                    maxLines = 10,
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedTextColor = XDColors.TextPrimary,
                        unfocusedTextColor = XDColors.TextPrimary,
                        focusedBorderColor = XDColors.Accent,
                        unfocusedBorderColor = XDColors.PanelLight
                    ),
                    modifier = Modifier.fillMaxWidth()
                )
            }
        },
        confirmButton = {
            TextButton(onClick = { onImport(text) }, enabled = text.isNotBlank()) {
                Text("Import", color = XDColors.Accent)
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel", color = XDColors.TextDim) }
        }
    )
}

private fun isShiny(mon: Gen3Mon): Boolean {
    val tid = mon.otId and 0xFFFF
    val sid = (mon.otId ushr 16) and 0xFFFF
    val pHi = (mon.pid ushr 16) and 0xFFFF
    val pLo = mon.pid and 0xFFFF
    return (tid xor sid xor pHi xor pLo) < 8
}
