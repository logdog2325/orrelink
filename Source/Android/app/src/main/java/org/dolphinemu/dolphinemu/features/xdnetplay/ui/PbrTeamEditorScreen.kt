// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
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
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
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
import org.dolphinemu.dolphinemu.features.xdnetplay.pbr.Bk4Factory
import org.dolphinemu.dolphinemu.features.xdnetplay.pbr.PbrContainer
import org.dolphinemu.dolphinemu.features.xdnetplay.pbr.PbrSave

/**
 * Team editor screen for PBR, in the same Pokémon Box "Sea" language as the
 * Gen 3 editor ([TeamEditorScreen]) so the two feel like one app.
 */
@Composable
fun PbrTeamEditorScreen(
    state: PbrEditorState,
    onSelectProfile: (Int) -> Unit,
    onSelectContainer: (PbrContainer) -> Unit,
    onSelectSlot: (Int) -> Unit,
    onImport: (String) -> Unit,
    onEdit: (PbrEditForm) -> Unit,
    onRemoveSelected: () -> Unit,
    onSave: () -> Unit,
    onBack: () -> Unit
) {
    var showImport by remember { mutableStateOf(false) }
    var showEdit by remember { mutableStateOf(false) }

    SeaBackground {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(16.dp)
        ) {
            SeaPanel(title = null) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("PBR ONLINE", color = XDColors.TextAccent, fontWeight = FontWeight.Bold)
                    Text("  ✦  ", color = XDColors.TextDim)
                    Text("Team Editor", color = XDColors.TextPrimary)
                    Spacer(Modifier.weight(1f))
                    TextButton(onClick = onBack) { Text("Back", color = XDColors.TextDim) }
                }
            }
            Spacer(Modifier.height(12.dp))

            if (state.loading) {
                SeaPanel(title = null) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(20.dp),
                            color = XDColors.Accent,
                            strokeWidth = 2.dp
                        )
                        Spacer(Modifier.width(12.dp))
                        Text("Reading the NAND save…", color = XDColors.TextDim)
                    }
                }
                return@Column
            }

            if (!state.ready) {
                SeaPanel(title = "NO SAVE YET") {
                    state.messages.forEach {
                        Text(it, color = XDColors.TextDim, fontSize = 13.sp)
                    }
                }
                return@Column
            }

            // Profile ("save slot") picker.
            Text("PROFILE", color = XDColors.TextAccent, fontSize = 11.sp,
                fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(6.dp))
            Row(
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                modifier = Modifier.horizontalScroll(rememberScrollState())
            ) {
                state.profiles.forEach { profile ->
                    PbrChip(
                        label = "${profile.label} (${profile.monCount})",
                        selected = profile.index == state.profile
                    ) { onSelectProfile(profile.index) }
                }
            }
            Spacer(Modifier.height(10.dp))

            // Party / box picker.
            Text("STORAGE", color = XDColors.TextAccent, fontSize = 11.sp,
                fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(6.dp))
            Row(
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                modifier = Modifier.horizontalScroll(rememberScrollState())
            ) {
                PbrChip(
                    label = PbrContainer.Party.label,
                    selected = state.containerLabel == PbrContainer.Party.label
                ) { onSelectContainer(PbrContainer.Party) }
                for (box in 0 until PbrSave.BOX_COUNT) {
                    val container = PbrContainer.Box(box)
                    PbrChip(
                        label = state.boxLabels.getOrElse(box) { container.label }.trim(),
                        selected = state.containerLabel == container.label
                    ) { onSelectContainer(container) }
                }
            }
            Spacer(Modifier.height(14.dp))

            SeaPanel(title = "${state.containerLabel} — ${state.fileLabel}") {
                if (state.entries.isEmpty()) {
                    Text(
                        "Empty. Import a Showdown team or a pokepast.es link to fill it.",
                        color = XDColors.TextDim
                    )
                }
                state.entries.forEachIndexed { i, entry ->
                    PbrMonRow(
                        entry = entry,
                        selected = i == state.selected,
                        onClick = { onSelectSlot(i) }
                    )
                }
                if (state.entries.isNotEmpty()) {
                    Text(
                        "${state.entries.size} of ${state.slotCount} slots used",
                        color = XDColors.TextDim,
                        fontSize = 11.sp
                    )
                }
            }
            Spacer(Modifier.height(14.dp))

            state.selectedEntry?.let { entry ->
                SeaPanel(title = "DETAIL") {
                    PbrMonDetail(
                        entry = entry,
                        onEdit = { showEdit = true },
                        onRemove = onRemoveSelected
                    )
                }
                Spacer(Modifier.height(14.dp))
            }

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
                    enabled = state.dirty,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = XDColors.PanelLight,
                        contentColor = XDColors.ChipText
                    ),
                    modifier = Modifier.weight(1f)
                ) { Text(if (state.dirty) "Save •" else "Saved") }
            }
            Spacer(Modifier.height(12.dp))

            if (state.messages.isNotEmpty()) {
                SeaPanel(title = "STATUS") {
                    state.messages.forEach {
                        val warn = it.startsWith("NOT") || it.startsWith("Warning") ||
                            it.startsWith("Skipped") || it.startsWith("Could not") ||
                            it.startsWith("Not applied")
                        Text(
                            text = it,
                            color = if (warn) XDColors.Accent else XDColors.TextDim,
                            fontSize = 13.sp
                        )
                    }
                }
                Spacer(Modifier.height(12.dp))
            }

            // Honest about what has and has not been proven, same as the rest
            // of this fork's UI.
            SeaPanel(title = "GOOD TO KNOW") {
                Text(
                    "This edits the party and box storage inside PbrSaveData. Every write " +
                        "is re-read and re-verified before it replaces the file, and the " +
                        "previous save is kept as PbrSaveData.bak.",
                    color = XDColors.TextDim,
                    fontSize = 12.sp
                )
                Spacer(Modifier.height(6.dp))
                Text(
                    "PBR's Battle Pass teams are stored separately and are not touched here, " +
                        "so a team you build may need to be picked again in-game. Close the " +
                        "game before saving — Dolphin writes the NAND back on exit.",
                    color = XDColors.TextDim,
                    fontSize = 12.sp
                )
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    if (showImport) {
        PbrImportDialog(
            containerLabel = state.containerLabel,
            onDismiss = { showImport = false },
            onImport = {
                showImport = false
                onImport(it)
            }
        )
    }

    val editing = state.selectedEntry
    if (showEdit && editing != null) {
        PbrEditDialog(
            entry = editing,
            onDismiss = { showEdit = false },
            onApply = {
                showEdit = false
                onEdit(it)
            }
        )
    }
}

@Composable
private fun PbrChip(label: String, selected: Boolean, onClick: () -> Unit) {
    Text(
        text = label,
        color = if (selected) XDColors.ChipText else XDColors.TextDim,
        fontSize = 13.sp,
        fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
        modifier = Modifier
            .background(
                if (selected) XDColors.Accent else XDColors.Panel,
                RoundedCornerShape(16.dp)
            )
            .border(1.dp, XDColors.PanelLight, RoundedCornerShape(16.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 6.dp)
    )
}

@Composable
private fun PbrMonRow(entry: PbrEntry, selected: Boolean, onClick: () -> Unit) {
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
        Box(
            contentAlignment = Alignment.Center,
            modifier = Modifier
                .size(40.dp)
                .background(XDColors.pastelFor(entry.species), CircleShape)
                .border(
                    width = if (entry.shiny) 2.dp else 1.dp,
                    color = if (entry.shiny) XDColors.Shiny else Color.White,
                    shape = CircleShape
                )
        ) {
            Text(
                text = entry.speciesName.take(1).uppercase(),
                color = XDColors.ChipText,
                fontWeight = FontWeight.Bold
            )
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = entry.nickname.ifEmpty { entry.speciesName },
                    color = if (selected) XDColors.TextAccent else XDColors.TextPrimary,
                    fontWeight = FontWeight.SemiBold
                )
                if (entry.shiny) {
                    Text("  ★", color = XDColors.Shiny)
                }
                if (!entry.checksumOk) {
                    Text("  ⚠", color = XDColors.Accent)
                }
            }
            Text(
                text = "${entry.speciesName} · ${entry.natureName} · ${genderMark(entry.gender)}",
                color = XDColors.TextDim,
                fontSize = 12.sp
            )
        }
        Column(horizontalAlignment = Alignment.End) {
            Text("Lv ${entry.level}", color = XDColors.TextPrimary, fontSize = 13.sp)
            Text(entry.itemName, color = XDColors.TextDim, fontSize = 12.sp)
        }
    }
}

@Composable
private fun PbrMonDetail(entry: PbrEntry, onEdit: () -> Unit, onRemove: () -> Unit) {
    PbrStatBlock(evs = entry.evs, ivs = entry.ivs)
    Spacer(Modifier.height(6.dp))
    val evTotal = entry.evs.sum()
    Text(
        text = "EV total $evTotal / 510    ·    ability ${entry.abilityName}    ·    OT ${entry.otName}",
        color = if (evTotal > 510) XDColors.Accent else XDColors.TextDim,
        fontSize = 12.sp
    )
    if (!entry.checksumOk) {
        Text(
            "⚠ This record's own checksum does not match — it was probably written by " +
                "another tool. Editing it will rewrite the checksum.",
            color = XDColors.Accent,
            fontSize = 12.sp
        )
    }
    Spacer(Modifier.height(8.dp))
    Text("MOVES", color = XDColors.TextAccent, fontSize = 12.sp, fontWeight = FontWeight.Bold)
    if (entry.moveNames.isEmpty()) {
        Text("· (none)", color = XDColors.TextDim, fontSize = 14.sp)
    }
    entry.moveNames.forEach {
        Text("· $it", color = XDColors.TextPrimary, fontSize = 14.sp)
    }
    Spacer(Modifier.height(8.dp))
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        TextButton(onClick = onEdit) { Text("Edit", color = XDColors.TextAccent) }
        TextButton(onClick = onRemove) { Text("Remove", color = XDColors.Accent) }
    }
}

@Composable
private fun PbrStatBlock(evs: List<Int>, ivs: List<Int>) {
    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        PbrTeamRepo.STAT_LABELS.forEachIndexed { i, label ->
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text(
                    label,
                    color = XDColors.TextAccent,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold
                )
                Text("${evs.getOrElse(i) { 0 }}", color = XDColors.TextPrimary, fontSize = 15.sp)
                Text("${ivs.getOrElse(i) { 0 }}", color = XDColors.TextDim, fontSize = 11.sp)
            }
        }
    }
    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
        Text("EV / IV", color = XDColors.TextDim, fontSize = 9.sp)
    }
}

@Composable
private fun PbrImportDialog(
    containerLabel: String,
    onDismiss: () -> Unit,
    onImport: (String) -> Unit
) {
    var text by remember { mutableStateOf("") }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = Color(0xFF183454),
        title = { Text("Import into $containerLabel", color = XDColors.TextPrimary) },
        text = {
            Column {
                Text(
                    "Paste a Showdown team export or a pokepast.es link. This replaces " +
                        "everything currently in $containerLabel.",
                    color = XDColors.TextDim,
                    fontSize = 13.sp
                )
                Spacer(Modifier.height(8.dp))
                OutlinedTextField(
                    value = text,
                    onValueChange = { text = it },
                    minLines = 6,
                    maxLines = 10,
                    colors = pbrFieldColors(),
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

@Composable
private fun PbrEditDialog(
    entry: PbrEntry,
    onDismiss: () -> Unit,
    onApply: (PbrEditForm) -> Unit
) {
    var nickname by remember(entry.offset) { mutableStateOf(entry.nickname) }
    var level by remember(entry.offset) { mutableStateOf(entry.level.toString()) }
    var item by remember(entry.offset) {
        mutableStateOf(if (entry.itemName == "—") "" else entry.itemName)
    }
    var ability by remember(entry.offset) { mutableStateOf(entry.abilityName) }
    var nature by remember(entry.offset) { mutableStateOf(entry.natureName) }
    var moves by remember(entry.offset) { mutableStateOf(entry.moveNames.joinToString(", ")) }
    var evs by remember(entry.offset) { mutableStateOf(PbrTeamRepo.spreadText(entry.evs)) }
    var ivs by remember(entry.offset) { mutableStateOf(PbrTeamRepo.spreadText(entry.ivs)) }
    var shiny by remember(entry.offset) { mutableStateOf(entry.shiny) }

    val badChars = PbrTeamRepo.unencodableIn(nickname)

    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = Color(0xFF183454),
        title = { Text("Edit ${entry.speciesName}", color = XDColors.TextPrimary) },
        text = {
            Column(
                modifier = Modifier
                    .heightIn(max = 420.dp)
                    .verticalScroll(rememberScrollState())
            ) {
                PbrField("Nickname", nickname) { nickname = it }
                if (badChars.isNotEmpty()) {
                    Text(
                        "These characters have no Gen 4 glyph and will be dropped: $badChars",
                        color = XDColors.Accent,
                        fontSize = 11.sp
                    )
                }
                PbrField("Level (1-100)", level) { level = it }
                PbrField("Held item (blank = none)", item) { item = it }
                PbrField("Ability", ability) { ability = it }
                PbrField("Nature", nature) { nature = it }
                PbrField("Moves (comma separated)", moves) { moves = it }
                PbrField("EVs", evs) { evs = it }
                PbrField("IVs", ivs) { ivs = it }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Shiny", color = XDColors.TextDim, fontSize = 13.sp)
                    Spacer(Modifier.width(10.dp))
                    Switch(
                        checked = shiny,
                        onCheckedChange = { shiny = it },
                        colors = SwitchDefaults.colors(
                            checkedThumbColor = XDColors.Accent,
                            checkedTrackColor = XDColors.PanelLight,
                            uncheckedThumbColor = XDColors.TextDim,
                            uncheckedTrackColor = XDColors.Panel
                        )
                    )
                }
                Text(
                    "Nature, ability slot and shininess are all carried by the PID, so " +
                        "changing any of them re-rolls it. Everything else leaves the PID " +
                        "alone.",
                    color = XDColors.TextDim,
                    fontSize = 11.sp
                )
            }
        },
        confirmButton = {
            TextButton(
                onClick = {
                    onApply(
                        PbrEditForm(
                            nickname = nickname,
                            level = level,
                            item = item,
                            ability = ability,
                            nature = nature,
                            moves = moves,
                            evs = evs,
                            ivs = ivs,
                            shiny = shiny
                        )
                    )
                }
            ) { Text("Apply", color = XDColors.Accent) }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel", color = XDColors.TextDim) }
        }
    )
}

@Composable
private fun PbrField(label: String, value: String, onValueChange: (String) -> Unit) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        label = { Text(label, color = XDColors.TextDim, fontSize = 12.sp) },
        singleLine = true,
        colors = pbrFieldColors(),
        modifier = Modifier.fillMaxWidth()
    )
    Spacer(Modifier.height(6.dp))
}

@Composable
private fun pbrFieldColors() = OutlinedTextFieldDefaults.colors(
    focusedTextColor = XDColors.TextPrimary,
    unfocusedTextColor = XDColors.TextPrimary,
    focusedBorderColor = XDColors.Accent,
    unfocusedBorderColor = XDColors.PanelLight,
    cursorColor = XDColors.Accent
)

private fun genderMark(gender: Int): String = when (gender) {
    Bk4Factory.GENDER_MALE -> "♂"
    Bk4Factory.GENDER_FEMALE -> "♀"
    else -> "—"
}
