// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.ui

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.features.xdnetplay.UpdateCheckBridge
import org.dolphinemu.dolphinemu.features.xdnetplay.UpdateCheckResult
import org.dolphinemu.dolphinemu.ui.main.ThemeProvider
import org.dolphinemu.dolphinemu.ui.theme.DolphinTheme
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import org.dolphinemu.dolphinemu.utils.ThemeHelper

/**
 * App entry hub: pick XD or PBR. Each button opens that mode's own launcher,
 * which force-applies that mode's config -- so the two never step on each other
 * and the XD launcher stays exactly as it was.
 */
class PokemonHubActivity : AppCompatActivity(), ThemeProvider {
    override var themeId: Int = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeHelper.setTheme(this)
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)

        // Start directory init here so whichever mode the user opens is ready.
        DirectoryInitialization.start(this)

        val ctx = this
        setContent {
            DolphinTheme {
                PokemonHubScreen(
                    onXd = { XDLauncherActivity.launch(ctx) },
                    onPbr = { PbrLauncherActivity.launch(ctx) }
                )
            }
        }
    }
}

@Composable
private fun PokemonHubScreen(onXd: () -> Unit, onPbr: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val localVersion = remember { UpdateCheckBridge.localVersion }
    var checking by remember { mutableStateOf(false) }
    var updateResult by remember { mutableStateOf<UpdateCheckResult?>(null) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = 24.dp, vertical = 16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text(
            text = "POKÉMON BATTLES",
            style = MaterialTheme.typography.displaySmall,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.primary
        )
        Text(
            text = "Choose a mode",
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Spacer(Modifier.height(48.dp))

        Button(onClick = onXd, modifier = Modifier.fillMaxWidth()) {
            Column(
                modifier = Modifier.padding(vertical = 8.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Text("XD Netplay", style = MaterialTheme.typography.titleLarge)
                Text("Pokémon XD GBA-link battles, online", style = MaterialTheme.typography.bodySmall)
            }
        }
        Spacer(Modifier.height(16.dp))
        Button(onClick = onPbr, modifier = Modifier.fillMaxWidth()) {
            Column(
                modifier = Modifier.padding(vertical = 8.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Text("PBR Online", style = MaterialTheme.typography.titleLarge)
                Text("Pokémon Battle Revolution, online", style = MaterialTheme.typography.bodySmall)
            }
        }
        Spacer(Modifier.height(32.dp))

        OutlinedButton(
            onClick = {
                checking = true
                scope.launch {
                    // Blocking HTTP: never on the main thread.
                    val result = withContext(Dispatchers.IO) { UpdateCheckBridge.check() }
                    checking = false
                    updateResult = result
                }
            },
            enabled = !checking,
            modifier = Modifier.fillMaxWidth()
        ) {
            Text(
                stringResource(
                    if (checking) R.string.xd_update_checking else R.string.xd_update_check
                )
            )
        }
        Spacer(Modifier.height(8.dp))
        Text(
            text = stringResource(R.string.xd_update_current_version, localVersion),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }

    updateResult?.let { result ->
        UpdateResultDialog(
            result = result,
            onDismiss = { updateResult = null },
            onDownload = {
                updateResult = null
                // A handheld build is not guaranteed to have anything registered for http.
                try {
                    context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(result.htmlUrl)))
                } catch (_: ActivityNotFoundException) {
                    Toast.makeText(context, R.string.xd_update_no_browser, Toast.LENGTH_LONG).show()
                }
            }
        )
    }
}

@Composable
private fun UpdateResultDialog(
    result: UpdateCheckResult,
    onDismiss: () -> Unit,
    onDownload: () -> Unit
) {
    val details = listOfNotNull(
        result.releaseName.takeIf { it.isNotEmpty() },
        result.publishedAt.takeIf { it.isNotEmpty() }
    ).joinToString(" — ")

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.xd_update_title)) },
        text = {
            Column {
                Text(result.message)
                if (details.isNotEmpty()) {
                    Spacer(Modifier.height(8.dp))
                    Text(details, style = MaterialTheme.typography.bodySmall)
                }
            }
        },
        confirmButton = {
            // Nothing is downloaded here; the release page is where a build gets picked.
            if (result.canOpenReleasePage) {
                TextButton(onClick = onDownload) {
                    Text(stringResource(R.string.xd_update_download))
                }
            } else {
                TextButton(onClick = onDismiss) {
                    Text(stringResource(R.string.xd_update_close))
                }
            }
        },
        dismissButton = {
            if (result.canOpenReleasePage) {
                TextButton(onClick = onDismiss) {
                    Text(stringResource(R.string.xd_update_close))
                }
            }
        }
    )
}
