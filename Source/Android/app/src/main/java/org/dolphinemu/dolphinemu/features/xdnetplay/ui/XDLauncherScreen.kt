// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.material3.Switch
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.features.xdnetplay.BattleStyleBridge

/**
 * What the Save Files card shows for one GBA socket. Assembled by
 * XDLauncherActivity.describeSaveSlot from the save file itself (game +
 * trainer are parsed from disk, so the row cannot disagree with what would
 * actually boot) plus the ImportedSave config key and the .preimport backup.
 */
data class SaveSlotUiState(
    /** e.g. "Team editor save (default) — Emerald, trainer LOGAN". */
    val stateText: String,
    /** True once an import happened (backup exists or config key set). */
    val restoreEnabled: Boolean
)

@Composable
fun XDLauncherScreen(
    initialized: Boolean,
    xdGameFound: Boolean,
    emeraldRomSet: Boolean,
    teamSavesReady: Boolean,
    controllerMapped: Boolean,
    biosLinkReady: Boolean,
    statusMessage: String?,
    saveSlotPort2: SaveSlotUiState?,
    saveSlotPort3: SaveSlotUiState?,
    saveFilesMessage: String?,
    saveFilesMessageIsError: Boolean,
    onImportSave: (Int) -> Unit,
    onRestoreDefaultSave: (Int) -> Unit,
    onPickXdFolder: () -> Unit,
    onPickGbaBios: () -> Unit,
    onPickEmeraldRom: () -> Unit,
    onTeamEditor: () -> Unit,
    onPlayXd: () -> Unit,
    onBattle: () -> Unit,
    cheatsEnabled: Boolean,
    onCheatsChanged: (Boolean) -> Unit,
    modelOptions: List<BattleStyleBridge.StyleOption>,
    musicOptions: List<BattleStyleBridge.StyleOption>,
    venueOptions: List<BattleStyleBridge.StyleOption>,
    hostModelId: Int,
    guestModelId: Int,
    musicId: Int,
    venueId: Int,
    onHostModelChanged: (Int) -> Unit,
    onGuestModelChanged: (Int) -> Unit,
    onMusicChanged: (Int) -> Unit,
    onVenueChanged: (Int) -> Unit,
    onSearchForMatch: () -> Unit,
    searching: Boolean,
    onFindBattles: () -> Unit,
    onOpenSettings: () -> Unit,
    onOpenDolphin: () -> Unit,
    onShareDetectLog: () -> Unit
) {
    Scaffold { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 24.dp, vertical = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Spacer(Modifier.height(24.dp))
            Text(
                text = "XD NETPLAY",
                style = MaterialTheme.typography.displaySmall,
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.primary
            )
            Text(
                text = "Pokémon XD GBA link battles, online",
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(Modifier.height(28.dp))

            if (!initialized) {
                CircularProgressIndicator()
                Spacer(Modifier.height(12.dp))
                Text("Setting things up…", style = MaterialTheme.typography.bodyMedium)
            } else {
                ReadinessCard(
                    xdGameFound = xdGameFound,
                    emeraldRomSet = emeraldRomSet,
                    teamSavesReady = teamSavesReady,
                    controllerMapped = controllerMapped,
                    biosLinkReady = biosLinkReady,
                    onPickXdFolder = onPickXdFolder,
                    onPickGbaBios = onPickGbaBios,
                    onPickEmeraldRom = onPickEmeraldRom,
                    onOpenSettings = onOpenSettings
                )
                if (statusMessage != null) {
                    Spacer(Modifier.height(12.dp))
                    Text(
                        text = statusMessage,
                        color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
                Spacer(Modifier.height(12.dp))
                SaveFilesCard(
                    port2 = saveSlotPort2,
                    port3 = saveSlotPort3,
                    message = saveFilesMessage,
                    messageIsError = saveFilesMessageIsError,
                    onImport = onImportSave,
                    onRestore = onRestoreDefaultSave
                )
                Spacer(Modifier.height(24.dp))

                FilledTonalButton(
                    onClick = onTeamEditor,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text("Team Editor", style = MaterialTheme.typography.titleMedium)
                }
                Spacer(Modifier.height(12.dp))
                Button(
                    onClick = onPlayXd,
                    enabled = xdGameFound && biosLinkReady,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text("Boot Pokémon XD (solo)", style = MaterialTheme.typography.titleMedium)
                }
                Spacer(Modifier.height(12.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(Modifier.weight(1f)) {
                        Text("\$XD OU Fixes cheat", style = MaterialTheme.typography.bodyLarge)
                        Text(
                            "Off by default. When you host, this applies to both players.",
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                    Switch(checked = cheatsEnabled, onCheckedChange = onCheatsChanged)
                }
                Spacer(Modifier.height(12.dp))
                // Cosmetic battle-style selectors, host-side. The guest picks
                // its OWN model in the room's Submit Team sheet; the "Guest
                // model" dropdown here is only the fallback when they never do
                // (mirroring the socket-3 team fallback). All of it is
                // assembled into one synced AR code by shared core at Start —
                // "Game default" means the code is genuinely absent.
                BattleStyleCard(
                    modelOptions = modelOptions,
                    musicOptions = musicOptions,
                    venueOptions = venueOptions,
                    hostModelId = hostModelId,
                    guestModelId = guestModelId,
                    musicId = musicId,
                    venueId = venueId,
                    onHostModelChanged = onHostModelChanged,
                    onGuestModelChanged = onGuestModelChanged,
                    onMusicChanged = onMusicChanged,
                    onVenueChanged = onVenueChanged
                )
                Spacer(Modifier.height(12.dp))
                // The headline action: no codes to trade, no lobby to read.
                // Joins whoever is already waiting, or becomes the room that
                // the next person's search finds.
                Button(
                    onClick = onSearchForMatch,
                    enabled = xdGameFound && biosLinkReady && !searching,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    if (searching) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(18.dp),
                            strokeWidth = 2.dp,
                            color = MaterialTheme.colorScheme.onPrimary
                        )
                        Spacer(Modifier.width(12.dp))
                    }
                    Text("Search for Match", style = MaterialTheme.typography.titleMedium)
                }
                Spacer(Modifier.height(12.dp))
                Button(
                    onClick = onBattle,
                    enabled = xdGameFound && biosLinkReady,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text("Battle  —  Host or Join", style = MaterialTheme.typography.titleMedium)
                }
                Spacer(Modifier.height(12.dp))
                OutlinedButton(
                    onClick = onFindBattles,
                    enabled = xdGameFound,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text("Find Battles  —  public lobby", style = MaterialTheme.typography.titleMedium)
                }
                Spacer(Modifier.height(24.dp))

                Row {
                    OutlinedButton(onClick = onOpenSettings) { Text("Settings") }
                    Spacer(Modifier.width(12.dp))
                    TextButton(onClick = onOpenDolphin) { Text("Full Dolphin") }
                    Spacer(Modifier.width(12.dp))
                    TextButton(onClick = onShareDetectLog) { Text("Share log") }
                }
            }
            Spacer(Modifier.height(16.dp))
        }
    }
}

@Composable
private fun BattleStyleCard(
    modelOptions: List<BattleStyleBridge.StyleOption>,
    musicOptions: List<BattleStyleBridge.StyleOption>,
    venueOptions: List<BattleStyleBridge.StyleOption>,
    hostModelId: Int,
    guestModelId: Int,
    musicId: Int,
    venueId: Int,
    onHostModelChanged: (Int) -> Unit,
    onGuestModelChanged: (Int) -> Unit,
    onMusicChanged: (Int) -> Unit,
    onVenueChanged: (Int) -> Unit
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            Text(
                text = stringResource(R.string.xd_style_section_title),
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold
            )
            Text(
                text = stringResource(R.string.xd_style_section_hint),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            BattleStyleDropdown(
                label = stringResource(R.string.xd_style_your_model),
                options = modelOptions,
                selectedId = hostModelId,
                defaultLabel = stringResource(R.string.xd_style_game_default),
                onSelected = onHostModelChanged,
                modifier = Modifier.fillMaxWidth()
            )
            BattleStyleDropdown(
                label = stringResource(R.string.xd_style_guest_model),
                options = modelOptions,
                selectedId = guestModelId,
                defaultLabel = stringResource(R.string.xd_style_game_default),
                onSelected = onGuestModelChanged,
                modifier = Modifier.fillMaxWidth(),
                supportingText = stringResource(R.string.xd_style_guest_model_hint)
            )
            BattleStyleDropdown(
                label = stringResource(R.string.xd_style_music),
                options = musicOptions,
                selectedId = musicId,
                defaultLabel = stringResource(R.string.xd_style_game_default),
                onSelected = onMusicChanged,
                modifier = Modifier.fillMaxWidth()
            )
            BattleStyleDropdown(
                label = stringResource(R.string.xd_style_venue),
                options = venueOptions,
                selectedId = venueId,
                defaultLabel = stringResource(R.string.xd_style_game_default),
                onSelected = onVenueChanged,
                modifier = Modifier.fillMaxWidth()
            )
        }
    }
}

@Composable
private fun ReadinessCard(
    xdGameFound: Boolean,
    emeraldRomSet: Boolean,
    teamSavesReady: Boolean,
    controllerMapped: Boolean,
    biosLinkReady: Boolean,
    onPickXdFolder: () -> Unit,
    onPickGbaBios: () -> Unit,
    onPickEmeraldRom: () -> Unit,
    onOpenSettings: () -> Unit
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text(
                text = "Checklist",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold
            )
            CheckRow(
                ok = xdGameFound,
                okText = "Pokémon XD found",
                missingText = "Pokémon XD not found — choose the folder with your ISO",
                fixLabel = "Choose folder…",
                onFix = onPickXdFolder
            )
            CheckRow(
                ok = emeraldRomSet,
                okText = "Emerald ROM configured",
                missingText = "Emerald ROM not set",
                fixLabel = "Choose ROM…",
                onFix = onPickEmeraldRom
            )
            CheckRow(
                ok = teamSavesReady,
                okText = "Team saves installed (XD requires progressed saves)",
                missingText = "Team saves install automatically once the Emerald ROM is set",
                fixLabel = "",
                onFix = null
            )
            CheckRow(
                ok = controllerMapped,
                okText = "Controller mapped",
                missingText = "No gamepad detected — connect one, or map manually",
                fixLabel = "Input settings…",
                onFix = onOpenSettings
            )
            CheckRow(
                ok = biosLinkReady,
                okText = "Official GBA BIOS configured",
                missingText = "Official GBA BIOS required — the game cannot detect the GBA without it",
                fixLabel = "Choose BIOS…",
                onFix = onPickGbaBios
            )
            CheckRow(
                ok = true,
                okText = "XD OU rules bundled",
                missingText = "",
                fixLabel = "",
                onFix = null
            )
        }
    }
}

/**
 * Which Gen 3 save each GBA socket boots from. The bundled team-editor save
 * stays the default; importing your own cartridge save is opt-in and per port
 * (the shared SaveImport core does the validation and the no-destruction
 * bookkeeping — see SaveImportBridge). The caption is a permanent fixture
 * rather than a popup because it is the one fact every importer must know
 * BEFORE picking a file: netplay syncs the HOST's saves, so a joiner's import
 * never reaches the room — joiners submit their team instead. Mirrors the
 * desktop launcher's "Save Files" box (XDLauncherDialog).
 */
@Composable
private fun SaveFilesCard(
    port2: SaveSlotUiState?,
    port3: SaveSlotUiState?,
    message: String?,
    messageIsError: Boolean,
    onImport: (Int) -> Unit,
    onRestore: (Int) -> Unit
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            Text(
                text = stringResource(R.string.xd_saves_section_title),
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold
            )
            Text(
                text = stringResource(R.string.xd_saves_host_note),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            SaveSlotRow(
                label = stringResource(R.string.xd_saves_port2_label),
                state = port2,
                deviceNumber = 1,
                onImport = onImport,
                onRestore = onRestore
            )
            SaveSlotRow(
                label = stringResource(R.string.xd_saves_port3_label),
                state = port3,
                deviceNumber = 2,
                onImport = onImport,
                onRestore = onRestore
            )
            if (message != null) {
                Text(
                    text = message,
                    style = MaterialTheme.typography.bodySmall,
                    color = if (messageIsError) {
                        MaterialTheme.colorScheme.error
                    } else {
                        MaterialTheme.colorScheme.primary
                    }
                )
            }
        }
    }
}

/**
 * One GBA socket in the Save Files card: label, live state line, and the
 * import/restore actions. [deviceNumber] is 1 (port 2) or 2 (port 3), the
 * same numbering as TeamRole.deviceNumber and the shared core. Both buttons
 * stay visible so the restore action is discoverable before any import; it
 * only enables once an import actually happened.
 */
@Composable
private fun SaveSlotRow(
    label: String,
    state: SaveSlotUiState?,
    deviceNumber: Int,
    onImport: (Int) -> Unit,
    onRestore: (Int) -> Unit
) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Medium
        )
        Text(
            text = state?.stateText ?: "",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(verticalAlignment = Alignment.CenterVertically) {
            TextButton(
                onClick = { onImport(deviceNumber) },
                enabled = state != null,
                contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp)
            ) {
                Text(stringResource(R.string.xd_saves_import_button))
            }
            Spacer(Modifier.width(20.dp))
            TextButton(
                onClick = { onRestore(deviceNumber) },
                enabled = state?.restoreEnabled == true,
                contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp)
            ) {
                Text(stringResource(R.string.xd_saves_restore_button))
            }
        }
    }
}

@Composable
private fun CheckRow(
    ok: Boolean,
    okText: String,
    missingText: String,
    fixLabel: String,
    onFix: (() -> Unit)?
) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(
            imageVector = if (ok) Icons.Default.Check else Icons.Default.Close,
            contentDescription = null,
            tint = if (ok) {
                MaterialTheme.colorScheme.primary
            } else {
                MaterialTheme.colorScheme.error
            },
            modifier = Modifier.size(20.dp)
        )
        Spacer(Modifier.width(10.dp))
        Column(modifier = Modifier.fillMaxWidth()) {
            Text(
                text = if (ok) okText else missingText,
                style = MaterialTheme.typography.bodyMedium
            )
            if (!ok && onFix != null) {
                TextButton(
                    onClick = onFix,
                    contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp)
                ) {
                    Text(fixLabel)
                }
            }
        }
    }
}
