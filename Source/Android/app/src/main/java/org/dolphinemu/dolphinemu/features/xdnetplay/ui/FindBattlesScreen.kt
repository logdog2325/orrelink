// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
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
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.dolphinemu.dolphinemu.features.xdnetplay.LobbySession

/**
 * Lobby browser screen in the Pokemon Box "Sea" design language (see XDTheme.kt):
 * floating navy panels on the blue gradient, yellow accent for selection and
 * warnings only.
 */
@Composable
fun FindBattlesScreen(
    initialized: Boolean,
    state: FindBattlesState,
    onBack: () -> Unit,
    onRefresh: () -> Unit,
    onToggleXdOnly: (Boolean) -> Unit,
    onJoin: (LobbySession) -> Unit,
    onPasswordSubmit: (LobbySession, String) -> Unit,
    onPasswordDismiss: () -> Unit,
    onPublishEnabledChange: (Boolean) -> Unit,
    onPublishNameChange: (String) -> Unit,
    onPublishRegionChange: (String) -> Unit,
    onPublishPasswordChange: (String) -> Unit,
    onHostPublic: () -> Unit
) {
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
                    Text("Find Battles", color = XDColors.TextPrimary)
                    Spacer(Modifier.weight(1f))
                    TextButton(onClick = onBack) { Text("Back", color = XDColors.TextDim) }
                }
            }
            Spacer(Modifier.height(12.dp))

            // Filter chips + refresh
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                FilterChip(label = "XD battles", selected = state.xdOnly) { onToggleXdOnly(true) }
                FilterChip(label = "All games", selected = !state.xdOnly) { onToggleXdOnly(false) }
                Spacer(Modifier.weight(1f))
                Button(
                    onClick = onRefresh,
                    enabled = initialized && !state.loading,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = XDColors.Accent,
                        contentColor = XDColors.ChipText,
                        disabledContainerColor = XDColors.PanelLight.copy(alpha = 0.5f),
                        disabledContentColor = XDColors.ChipText
                    )
                ) { Text(if (state.loading) "Loading" else "Refresh") }
            }
            Spacer(Modifier.height(14.dp))

            // Session list
            SeaPanel(title = "OPEN BATTLES") {
                when {
                    !initialized -> {
                        Text("Setting things up...", color = XDColors.TextDim)
                    }

                    state.loading -> {
                        Text("Scanning the lobby...", color = XDColors.TextDim)
                    }

                    state.error != null -> {
                        Text(state.error, color = XDColors.Accent, fontSize = 14.sp)
                        Spacer(Modifier.height(4.dp))
                        Text(
                            "Tap Refresh to try again.",
                            color = XDColors.TextDim,
                            fontSize = 12.sp
                        )
                    }

                    state.visibleSessions.isEmpty() -> {
                        Text(
                            text = if (state.xdOnly) {
                                "No open XD battles right now."
                            } else {
                                "No open sessions right now."
                            },
                            color = XDColors.TextDim
                        )
                        Spacer(Modifier.height(4.dp))
                        Text(
                            text = "Host a public battle below, or tap Refresh in a bit.",
                            color = XDColors.TextDim,
                            fontSize = 12.sp
                        )
                    }

                    else -> {
                        state.visibleSessions.forEach { session ->
                            SessionRow(
                                session = session,
                                enabled = !state.joining,
                                onClick = { onJoin(session) }
                            )
                        }
                        Spacer(Modifier.height(6.dp))
                        val count = state.visibleSessions.size
                        Text(
                            text = if (count == 1) "1 battle found - tap to join"
                            else "$count battles found - tap to join",
                            color = XDColors.TextDim,
                            fontSize = 12.sp
                        )
                    }
                }
            }
            Spacer(Modifier.height(14.dp))

            // Join / error status
            if (state.status != null) {
                SeaPanel(title = "STATUS") {
                    Text(
                        text = state.status,
                        color = if (state.joining) XDColors.TextPrimary else XDColors.Accent,
                        fontSize = 13.sp
                    )
                }
                Spacer(Modifier.height(14.dp))
            }

            // Publish panel (host side)
            SeaPanel(title = "HOST A PUBLIC BATTLE") {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(
                            "List my hosted battles in the lobby",
                            color = XDColors.TextPrimary,
                            fontSize = 14.sp
                        )
                        Text(
                            "Your battle is visible while you host and unlisted when the room closes.",
                            color = XDColors.TextDim,
                            fontSize = 11.sp
                        )
                    }
                    Switch(
                        checked = state.publishEnabled,
                        onCheckedChange = onPublishEnabledChange,
                        colors = SwitchDefaults.colors(
                            checkedThumbColor = XDColors.Accent,
                            checkedTrackColor = XDColors.PanelLight,
                            uncheckedThumbColor = XDColors.TextDim,
                            uncheckedTrackColor = XDColors.Panel
                        )
                    )
                }
                Spacer(Modifier.height(10.dp))

                OutlinedTextField(
                    value = state.publishName,
                    onValueChange = onPublishNameChange,
                    label = { Text("Battle name", color = XDColors.TextDim, fontSize = 12.sp) },
                    singleLine = true,
                    colors = seaFieldColors(),
                    modifier = Modifier.fillMaxWidth()
                )
                Spacer(Modifier.height(10.dp))

                Text(
                    "REGION",
                    color = XDColors.TextAccent,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold
                )
                Spacer(Modifier.height(6.dp))
                Row(
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    modifier = Modifier.horizontalScroll(rememberScrollState())
                ) {
                    state.regions.forEach { (code, label) ->
                        FilterChip(
                            label = "$label ($code)",
                            selected = code == state.publishRegion
                        ) { onPublishRegionChange(code) }
                    }
                }
                Spacer(Modifier.height(10.dp))

                OutlinedTextField(
                    value = state.publishPassword,
                    onValueChange = onPublishPasswordChange,
                    label = {
                        Text("Password (optional)", color = XDColors.TextDim, fontSize = 12.sp)
                    },
                    singleLine = true,
                    visualTransformation = PasswordVisualTransformation(),
                    colors = seaFieldColors(),
                    modifier = Modifier.fillMaxWidth()
                )
                Spacer(Modifier.height(12.dp))

                Button(
                    onClick = onHostPublic,
                    enabled = initialized && !state.joining && state.publishName.isNotBlank(),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = XDColors.Accent,
                        contentColor = XDColors.ChipText,
                        disabledContainerColor = XDColors.PanelLight.copy(alpha = 0.5f),
                        disabledContentColor = XDColors.ChipText
                    ),
                    modifier = Modifier.fillMaxWidth()
                ) { Text("Host now") }
            }
            Spacer(Modifier.height(12.dp))

            if (state.buildVersion.isNotEmpty()) {
                Text(
                    text = "Only battles hosted on build ${state.buildVersion} are shown.",
                    color = XDColors.TextDim,
                    fontSize = 10.sp
                )
            }
            Spacer(Modifier.height(16.dp))
        }
    }

    state.passwordPrompt?.let { session ->
        PasswordDialog(
            session = session,
            onDismiss = onPasswordDismiss,
            onSubmit = { password -> onPasswordSubmit(session, password) }
        )
    }
}

@Composable
private fun FilterChip(label: String, selected: Boolean, onClick: () -> Unit) {
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
private fun SessionRow(session: LobbySession, enabled: Boolean, onClick: () -> Unit) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .background(Color.Transparent, RoundedCornerShape(10.dp))
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 8.dp, vertical = 8.dp)
    ) {
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = session.name.ifEmpty { "(unnamed)" },
                    color = XDColors.TextPrimary,
                    fontWeight = FontWeight.SemiBold
                )
                if (session.hasPassword) {
                    Spacer(Modifier.width(6.dp))
                    Badge(text = "LOCKED", accent = true)
                }
                if (session.inGame) {
                    Spacer(Modifier.width(6.dp))
                    Badge(text = "IN GAME", accent = false)
                }
            }
            Text(
                text = "${session.game.ifEmpty { "Unknown game" }} · ${session.region}",
                color = XDColors.TextDim,
                fontSize = 12.sp
            )
        }
        Column(horizontalAlignment = Alignment.End) {
            Text(
                text = "${session.playerCount}P",
                color = XDColors.TextPrimary,
                fontSize = 13.sp,
                fontWeight = FontWeight.Bold
            )
            Text(
                text = if (session.isTraversal) "room code" else "direct",
                color = XDColors.TextDim,
                fontSize = 10.sp
            )
        }
    }
}

/** Tiny outlined pill; accent (yellow) marks locked sessions, plain marks in-game. */
@Composable
private fun Badge(text: String, accent: Boolean) {
    val color = if (accent) XDColors.Accent else XDColors.TextDim
    Text(
        text = text,
        color = color,
        fontSize = 9.sp,
        fontWeight = FontWeight.Bold,
        modifier = Modifier
            .border(1.dp, color, RoundedCornerShape(6.dp))
            .padding(horizontal = 5.dp, vertical = 1.dp)
    )
}

@Composable
private fun PasswordDialog(
    session: LobbySession,
    onDismiss: () -> Unit,
    onSubmit: (String) -> Unit
) {
    var password by remember { mutableStateOf("") }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = Color(0xFF183454),
        title = { Text("Password required", color = XDColors.TextPrimary) },
        text = {
            Column {
                Text(
                    "\"${session.name}\" is locked. Enter its password to join.",
                    color = XDColors.TextDim,
                    fontSize = 13.sp
                )
                Spacer(Modifier.height(8.dp))
                OutlinedTextField(
                    value = password,
                    onValueChange = { password = it },
                    singleLine = true,
                    visualTransformation = PasswordVisualTransformation(),
                    colors = seaFieldColors(),
                    modifier = Modifier.fillMaxWidth()
                )
            }
        },
        confirmButton = {
            TextButton(onClick = { onSubmit(password) }, enabled = password.isNotEmpty()) {
                Text("Join", color = XDColors.Accent)
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel", color = XDColors.TextDim) }
        }
    )
}

@Composable
private fun seaFieldColors() = OutlinedTextFieldDefaults.colors(
    focusedTextColor = XDColors.TextPrimary,
    unfocusedTextColor = XDColors.TextPrimary,
    focusedBorderColor = XDColors.Accent,
    unfocusedBorderColor = XDColors.PanelLight,
    cursorColor = XDColors.Accent
)
