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
    pbrFound: Boolean,
    statusMessage: String?,
    onImportNand: () -> Unit,
    onPickPbrFolder: () -> Unit,
    onPlayPbr: () -> Unit,
    onBack: () -> Unit
) {
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
            text = "Pokémon Battle Revolution, online (UPA)",
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
                        ok = pbrFound,
                        okText = "Patched PBR found (RPBE01)",
                        missingText = "Patched PBR not found — choose the folder with your UPA ISO",
                        fixLabel = "Choose folder…",
                        onFix = onPickPbrFolder
                    )
                    CheckRow(
                        ok = nandImported,
                        okText = "Wii NAND imported",
                        missingText = "Import the UPA nand.bin (has keys appended)",
                        fixLabel = "Import NAND…",
                        onFix = onImportNand
                    )
                    CheckRow(
                        ok = true,
                        okText = "Graphics + black-screen fixes bundled",
                        missingText = "",
                        fixLabel = "",
                        onFix = null
                    )
                }
            }

            Spacer(Modifier.height(12.dp))
            Text(
                text = "No SSL certs, no DNS change, no 1-week wait — the UPA ISO connects directly. " +
                    "For teams, drop the Restorer save's PbrSaveData in the game's Wii save folder " +
                    "(or edit with PKHeX).",
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
                Text("Import Wii NAND (nand.bin)", style = MaterialTheme.typography.titleMedium)
            }
            Spacer(Modifier.height(12.dp))
            Button(
                onClick = onPlayPbr,
                enabled = pbrFound && nandImported,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Play PBR Online", style = MaterialTheme.typography.titleMedium)
            }
            Spacer(Modifier.height(24.dp))

            Row {
                TextButton(onClick = onBack) { Text("Back") }
            }
        }
        Spacer(Modifier.height(16.dp))
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
            tint = if (ok) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
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
