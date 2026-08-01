// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.ui

import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Info
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

@Composable
fun PbrLauncherScreen(
    initialized: Boolean,
    nandImported: Boolean,
    pbrGameId: String?,
    pbrSupported: Boolean,
    statusMessage: String?,
    onImportNand: () -> Unit,
    pbrSaveInstalled: Boolean,
    onImportPbrSave: () -> Unit,
    onPickPbrFolder: () -> Unit,
    onTeamEditor: () -> Unit,
    onPlayPbr: () -> Unit,
    onBack: () -> Unit
) {
    val playable = pbrGameId != null && pbrSupported
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 24.dp, vertical = 16.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Spacer(Modifier.height(24.dp))
        Text(
            text = "PBR ONLINE",
            style = MaterialTheme.typography.displaySmall,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.primary
        )
        Text(
            text = "Pokémon Battle Revolution, online (Wiimmfi)",
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Spacer(Modifier.height(28.dp))

        if (!initialized) {
            CircularProgressIndicator()
            Spacer(Modifier.height(12.dp))
            Text("Setting things up…", style = MaterialTheme.typography.bodyMedium)
        } else {
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(
                    modifier = Modifier.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    Text(
                        text = "Checklist",
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold
                    )
                    CheckRow(
                        state = when {
                            playable -> RowState.Ok
                            pbrGameId != null -> RowState.Warn
                            else -> RowState.Missing
                        },
                        okText = "Pokémon Battle Revolution found ($pbrGameId) — " +
                            "Wiimmfi patch applied automatically at boot",
                        warnText = "Pokémon Battle Revolution found ($pbrGameId), but that " +
                            "region has no Wiimmfi support. Online play needs the US (RPBE01) " +
                            "or European (RPBP01) disc.",
                        missingText = "Pokémon Battle Revolution not found — choose the folder " +
                            "holding your dump. A clean dump is fine (iso/wbfs/rvz/ciso).",
                        fixLabel = "Choose folder…",
                        onFix = onPickPbrFolder
                    )
                    CheckRow(
                        state = if (nandImported) RowState.Ok else RowState.Warn,
                        okText = "Wii NAND imported",
                        warnText = "No Wii NAND imported. Offline play works without one, but " +
                            "online play needs a NAND backup from YOUR OWN Wii — a shared or " +
                            "generated NAND gets rejected and can get you banned.",
                        missingText = "",
                        fixLabel = "Import NAND…",
                        onFix = onImportNand
                    )
                    CheckRow(
                        state = if (pbrSaveInstalled) RowState.Ok else RowState.Warn,
                        okText = "PBR save installed (bundled Restorer Forever starter save)",
                        warnText = "No PBR save installed yet — open this screen again, or " +
                            "install your own save file.",
                        missingText = "",
                        fixLabel = "Install save…",
                        onFix = onImportPbrSave
                    )
                    CheckRow(
                        state = RowState.Ok,
                        okText = "Graphics + black-screen fixes bundled",
                        warnText = "",
                        missingText = "",
                        fixLabel = "",
                        onFix = null
                    )
                }
            }

            Spacer(Modifier.height(12.dp))
            Text(
                text = "Bring a clean dump in any format — the Wiimmfi redirect is applied in " +
                    "memory at boot, so your file is never modified and a disc that is already " +
                    "patched is left alone. No SSL certs, no DNS change, no 1-week wait. " +
                    "Teams live in the game's own NAND save; the Team Editor below reads and " +
                    "writes it in place, so PKHeX on a PC is optional.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
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

            FilledTonalButton(onClick = onImportNand, modifier = Modifier.fillMaxWidth()) {
                Text(
                    "Import your Wii NAND (online only)",
                    style = MaterialTheme.typography.titleMedium
                )
            }
            Spacer(Modifier.height(12.dp))
            // Not gated on the disc: the editor works on the NAND save, which
            // exists (or does not) independently of whether a dump is present.
            FilledTonalButton(onClick = onTeamEditor, modifier = Modifier.fillMaxWidth()) {
                Text("Team Editor", style = MaterialTheme.typography.titleMedium)
            }
            Spacer(Modifier.height(12.dp))
            // Deliberately not gated on the NAND: without one the game still boots and plays
            // offline, and a NAND is something only the user can supply from their own console.
            Button(
                onClick = onPlayPbr,
                enabled = playable,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Play PBR", style = MaterialTheme.typography.titleMedium)
            }
            Spacer(Modifier.height(24.dp))

            Row {
                TextButton(onClick = onBack) { Text("Back") }
            }
        }
        Spacer(Modifier.height(16.dp))
    }
}

/** Ok = done, Warn = worth fixing but does not block Play, Missing = actually blocks Play. */
private enum class RowState { Ok, Warn, Missing }

@Composable
private fun CheckRow(
    state: RowState,
    okText: String,
    warnText: String,
    missingText: String,
    fixLabel: String,
    onFix: (() -> Unit)?
) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(
            imageVector = when (state) {
                RowState.Ok -> Icons.Default.Check
                RowState.Warn -> Icons.Default.Info
                RowState.Missing -> Icons.Default.Close
            },
            contentDescription = null,
            tint = when (state) {
                RowState.Ok -> MaterialTheme.colorScheme.primary
                RowState.Warn -> MaterialTheme.colorScheme.tertiary
                RowState.Missing -> MaterialTheme.colorScheme.error
            },
            modifier = Modifier.size(20.dp)
        )
        Spacer(Modifier.width(10.dp))
        Column(modifier = Modifier.fillMaxWidth()) {
            Text(
                text = when (state) {
                    RowState.Ok -> okText
                    RowState.Warn -> warnText
                    RowState.Missing -> missingText
                },
                style = MaterialTheme.typography.bodyMedium
            )
            if (state != RowState.Ok && onFix != null) {
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
