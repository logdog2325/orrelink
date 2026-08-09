// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.ui

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuAnchorType
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.features.xdnetplay.BattleStyleBridge

/**
 * One cosmetic battle-style selector: a dropdown over a BattleStyleBridge
 * table with a default entry (id 0 — "Game default" / "No preference") first,
 * then the tested-safe options, then an "Experimental" divider and the
 * experimental ones. The tables arrive tested-safe-first from native code, so
 * the divider goes at the first tier change.
 *
 * Purely cosmetic choices, so the picker never blocks anything: an id that is
 * no longer in the table (say, after an update) just renders as the default
 * label until the user picks again.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BattleStyleDropdown(
    label: String,
    options: List<BattleStyleBridge.StyleOption>,
    selectedId: Int,
    defaultLabel: String,
    onSelected: (Int) -> Unit,
    modifier: Modifier = Modifier,
    supportingText: String? = null,
) {
    var expanded by remember { mutableStateOf(false) }
    val selectedName = options.firstOrNull { it.id == selectedId }?.name ?: defaultLabel

    ExposedDropdownMenuBox(
        expanded = expanded,
        onExpandedChange = { expanded = it },
        modifier = modifier,
    ) {
        OutlinedTextField(
            value = selectedName,
            onValueChange = {},
            readOnly = true,
            singleLine = true,
            label = { Text(label) },
            supportingText = supportingText?.let { { Text(it) } },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded) },
            modifier = Modifier
                .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable)
                .fillMaxWidth()
        )

        ExposedDropdownMenu(
            expanded = expanded,
            onDismissRequest = { expanded = false },
        ) {
            DropdownMenuItem(
                text = { Text(defaultLabel) },
                onClick = {
                    onSelected(0)
                    expanded = false
                },
                contentPadding = ExposedDropdownMenuDefaults.ItemContentPadding,
            )
            var dividerShown = false
            options.forEach { option ->
                if (option.experimental && !dividerShown) {
                    dividerShown = true
                    HorizontalDivider()
                    DropdownMenuItem(
                        text = {
                            Text(
                                text = stringResource(R.string.xd_style_experimental),
                                style = MaterialTheme.typography.labelMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        },
                        onClick = {},
                        enabled = false,
                        contentPadding = ExposedDropdownMenuDefaults.ItemContentPadding,
                    )
                }
                DropdownMenuItem(
                    text = { Text(option.name) },
                    onClick = {
                        onSelected(option.id)
                        expanded = false
                    },
                    contentPadding = ExposedDropdownMenuDefaults.ItemContentPadding,
                )
            }
        }
    }
}
