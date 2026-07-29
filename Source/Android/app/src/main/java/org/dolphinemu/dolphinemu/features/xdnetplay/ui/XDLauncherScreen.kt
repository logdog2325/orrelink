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
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

@Composable
fun XDLauncherScreen(
    initialized: Boolean,
    xdGameFound: Boolean,
    emeraldRomSet: Boolean,
    teamSavesReady: Boolean,
    controllerMapped: Boolean,
    biosLinkReady: Boolean,
    statusMessage: String?,
    onPickXdFolder: () -> Unit,
    onPickGbaBios: () -> Unit,
    onPickEmeraldRom: () -> Unit,
    onTeamEditor: () -> Unit,
    onPlayXd: () -> Unit,
    onBattle: () -> Unit,
    cheatsEnabled: Boolean,
    onCheatsChanged: (Boolean) -> Unit,
    onFindBattles: () -> Unit,
    onOpenSettings: () -> Unit,
    onOpenDolphin: () -> Unit
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
                    enabled = xdGameFound,
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
                Button(
                    onClick = onBattle,
                    enabled = xdGameFound,
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
                }
            }
            Spacer(Modifier.height(16.dp))
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
                ok = true,
                okText = if (biosLinkReady) "GBA BIOS: using your official dump"
                         else "GBA BIOS: none needed — the bundled one works (optional)",
                missingText = "",
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
