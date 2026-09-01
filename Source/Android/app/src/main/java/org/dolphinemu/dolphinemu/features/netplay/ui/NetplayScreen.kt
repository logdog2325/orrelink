// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.netplay.ui

import android.content.Intent
import android.content.res.Configuration
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Remove
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuAnchorType
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SheetValue
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.adaptive.currentWindowAdaptiveInfo
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.sp
import androidx.window.core.layout.WindowSizeClass
import coil.compose.AsyncImage
import coil.request.ImageRequest
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.emptyFlow
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.features.netplay.model.GameDigestProgress
import org.dolphinemu.dolphinemu.features.netplay.model.JoinAddress
import org.dolphinemu.dolphinemu.features.netplay.model.JoinInfoType
import org.dolphinemu.dolphinemu.features.netplay.model.NetplayMessage
import org.dolphinemu.dolphinemu.features.netplay.model.NetworkMode
import org.dolphinemu.dolphinemu.features.netplay.model.Player
import org.dolphinemu.dolphinemu.features.netplay.model.SaveTransferProgress
import org.dolphinemu.dolphinemu.features.netplay.model.TraversalState
import org.dolphinemu.dolphinemu.features.xdnetplay.BattleStyleBridge
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.EmeraldSave
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.Gen3Text
import org.dolphinemu.dolphinemu.features.xdnetplay.ui.BattleStyleDropdown
import org.dolphinemu.dolphinemu.model.GameFile
import org.dolphinemu.dolphinemu.ui.theme.DolphinScaffold
import org.dolphinemu.dolphinemu.ui.theme.DolphinTheme
import org.dolphinemu.dolphinemu.ui.theme.MenuSpacer
import org.dolphinemu.dolphinemu.ui.theme.OutlinedBox
import org.dolphinemu.dolphinemu.ui.theme.PreviewTheme
import org.dolphinemu.dolphinemu.ui.theme.ReadOnlyTextField
import org.dolphinemu.dolphinemu.ui.theme.bottomFadeOverlay
import org.dolphinemu.dolphinemu.ui.theme.rememberSheetState
import org.dolphinemu.dolphinemu.utils.CoilUtils
import java.util.Locale

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun NetplayScreen(
    onBackClicked: () -> Unit,
    isHosting: Boolean,
    onSetHostName: (String) -> Unit = {},
    hostTrainerName: String = "",
    connectionLost: Flow<Unit>,
    fatalTraversalError: Flow<TraversalState.Failure>,
    messages: List<NetplayMessage>,
    onSendMessage: (String) -> Unit,
    game: String,
    onStartGame: () -> Unit,
    onGameSelected: (GameFile) -> Unit,
    gameFiles: List<GameFile>,
    notAllPlayersHaveGame: Flow<Unit>,
    onConfirmStartGame: () -> Unit,
    hostInputAuthorityEnabled: Boolean,
    networkMode: NetworkMode,
    onNetworkModeChanged: (NetworkMode) -> Unit,
    buffer: Int,
    onBufferChanged: (Int) -> Unit,
    autoBuffer: Boolean,
    onAutoBufferChanged: (Boolean) -> Unit,
    clientBuffer: Int,
    onClientBufferChanged: (Int) -> Unit,
    players: List<Player>,
    saveTransferProgress: SaveTransferProgress?,
    gameDigestProgress: GameDigestProgress?,
    joinAddresses: Map<JoinInfoType, JoinAddress>,
    onSubmitTeam: (String, String, Int, Boolean) -> Unit,
    onSubmitSaveBundle: (Int, Boolean) -> Unit,
    initialTeamText: String,
    initialTrainerName: String,
    initialModelId: Int,
    initialUseMySave: Boolean,
    modelOptions: List<BattleStyleBridge.StyleOption>,
    /**
     * True when THIS device's Format pick carries team rules (any of the six
     * community formats). Drives only the non-blocking paste-time note in the
     * Submit Team sheet — the room is governed by the HOST's key, enforced
     * host-side in shared core.
     */
    orreFormatLocal: Boolean,
    /** Display name of the local Format pick, for the note's wording. */
    localFormatName: String = "",
    /**
     * Shared core's legality check of a Showdown draft against the local
     * Format pick (FormatBridge.validateShowdown): "" = no complaint, else
     * one reason. Injected so previews need no native library.
     */
    validateTeamForFormat: (String) -> String,
) {
    val scrollState = rememberScrollState()
    // XD Netplay: joiner's "Submit Team" sheet. Every field opens pre-filled
    // with the last-submitted values (the initial* parameters, config-backed
    // via NetplayViewModel.submitPrefill) and is stored back on a successful
    // submit, so nothing has to be retyped next session.
    var showSubmitTeam by rememberSaveable { mutableStateOf(false) }
    var teamDraft by rememberSaveable { mutableStateOf(initialTeamText) }
    // In-game name: the stored one, else the netplay nickname (already cut
    // down to what a Gen 3 save can hold) so the common case is zero typing.
    var nameDraft by rememberSaveable { mutableStateOf(initialTrainerName) }
    // Cosmetic trainer-model pick, travelling as a "Model:" header in the same
    // TeamData payload as the team. 0 = "No preference": no header is sent and
    // the host's Guest-model fallback dropdown decides.
    var modelDraft by rememberSaveable { mutableIntStateOf(initialModelId) }
    // "Use my save": submit the party straight from this player's own save as
    // a bundle -- real mon bytes, real trainer identity -- instead of the
    // Showdown paste. The name and team fields do not apply in that mode (the
    // save's real trainer name always wins; see NetplaySession), so both grey
    // out. The model pick stays meaningful either way.
    var useMySave by rememberSaveable { mutableStateOf(initialUseMySave) }
    // Opt-in: ask the host to raise every under-level mon to Lv. 100 (host
    // applies it only in a level-100 format; only ever raises).
    var raiseTo100 by rememberSaveable { mutableStateOf(false) }
    if (showSubmitTeam) {
        AlertDialog(
            title = { Text("Submit Team") },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    Text(
                        "Paste a Showdown team export, or a pokepast.es link. " +
                            "The host writes it into the save you'll play with."
                    )
                    Spacer(Modifier.height(12.dp))
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(stringResource(R.string.xd_submit_use_save))
                            Text(
                                stringResource(R.string.xd_submit_use_save_hint),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                        Switch(checked = useMySave, onCheckedChange = { useMySave = it })
                    }
                    if (useMySave) {
                        Spacer(Modifier.height(4.dp))
                        Text(
                            stringResource(R.string.xd_submit_privacy_note),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                    Spacer(Modifier.height(12.dp))
                    OutlinedTextField(
                        value = nameDraft,
                        // Reject unencodable keystrokes as they are typed, and
                        // stop at the Gen 3 limit of seven characters.
                        onValueChange = {
                            nameDraft = Gen3Text.sanitize(it, EmeraldSave.TRAINER_NAME_LEN)
                        },
                        // A bundle always plays under the save's own trainer
                        // name (renaming would split the trainer from the
                        // mons' OT copies and make the party disobedient), so
                        // the field greys out rather than promising a rename
                        // that cannot happen.
                        enabled = !useMySave,
                        singleLine = true,
                        label = { Text("In-game name (max 7)") },
                        supportingText = {
                            Text(
                                if (useMySave) stringResource(R.string.xd_submit_name_ignored)
                                else "Shown to your opponent in the battle."
                            )
                        },
                        modifier = Modifier.fillMaxWidth()
                    )
                    Spacer(Modifier.height(12.dp))
                    BattleStyleDropdown(
                        label = stringResource(R.string.xd_style_submit_model_label),
                        options = modelOptions,
                        selectedId = modelDraft,
                        defaultLabel = stringResource(R.string.xd_style_no_preference),
                        onSelected = { modelDraft = it },
                        modifier = Modifier.fillMaxWidth(),
                        supportingText = stringResource(R.string.xd_style_submit_model_hint)
                    )
                    Spacer(Modifier.height(8.dp))
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Column(Modifier.weight(1f)) {
                            Text(stringResource(R.string.xd_submit_raise_100))
                            Text(
                                stringResource(R.string.xd_submit_raise_100_hint),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                        Switch(checked = raiseTo100, onCheckedChange = { raiseTo100 = it })
                    }
                    Spacer(Modifier.height(12.dp))
                    OutlinedTextField(
                        value = teamDraft,
                        onValueChange = { teamDraft = it },
                        // Not part of a bundle submission; the draft is kept.
                        enabled = !useMySave,
                        modifier = Modifier.fillMaxWidth().height(220.dp)
                    )
                    // Paste-time format note, shown immediately while the
                    // local Format pick carries team rules. NEVER blocking: Send
                    // stays enabled — the note exists so a refusal from the
                    // host's gate is no surprise. (A pokepast.es link parses
                    // to nothing and gets no note; the host still enforces on
                    // the fetched text. The bundle path has no draft to check
                    // here — the host's gate judges the sent party.)
                    if (orreFormatLocal && !useMySave) {
                        val formatReason = remember(teamDraft) {
                            validateTeamForFormat(teamDraft)
                        }
                        if (formatReason.isNotEmpty()) {
                            Spacer(Modifier.height(4.dp))
                            Text(
                                stringResource(
                                    R.string.xd_format_note, localFormatName, formatReason
                                ),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.error
                            )
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        if (useMySave) {
                            onSubmitSaveBundle(modelDraft, raiseTo100)
                        } else {
                            onSubmitTeam(teamDraft.trim(), nameDraft.trim(), modelDraft, raiseTo100)
                        }
                        showSubmitTeam = false
                    },
                    enabled = useMySave || teamDraft.isNotBlank()
                ) {
                    Text("Send")
                }
            },
            dismissButton = {
                TextButton(onClick = { showSubmitTeam = false }) {
                    Text(stringResource(R.string.cancel))
                }
            },
            onDismissRequest = { showSubmitTeam = false },
        )
    }

    DolphinScaffold(
        title = {
            Text(stringResource(R.string.netplay_title))
        },
        navigationIcon = {
            IconButton(onClick = onBackClicked) {
                Icon(
                    imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                    contentDescription = "Back",
                )
            }
        },
        floatingActionButton = {
            if (isHosting) {
                ExtendedFloatingActionButton(onClick = onStartGame) {
                    Text(stringResource(R.string.netplay_start))
                }
            } else {
                // XD Netplay: a joiner hands their own team to the host, which
                // writes it into the save it syncs at start. Sits where the
                // host's Start button is, since only one of them ever shows.
                ExtendedFloatingActionButton(onClick = { showSubmitTeam = true }) {
                    Text("Submit Team")
                }
            }
        },
    ) { innerPadding ->
        val modifier = Modifier
            .fillMaxSize()
            .consumeWindowInsets(innerPadding)

        // State which must live above the landscape/portrait split.
        var showChat by rememberSaveable { mutableStateOf(false) }
        var showGamePicker by rememberSaveable { mutableStateOf(false) }
        var selectedJoinInfoType by rememberSaveable {
            mutableStateOf(joinAddresses.keys.firstOrNull() ?: JoinInfoType.EXTERNAL)
        }

        val isLandscape =
            LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
        val isWidthAtLeastMedium = currentWindowAdaptiveInfo().windowSizeClass
            .isWidthAtLeastBreakpoint(WindowSizeClass.WIDTH_DP_MEDIUM_LOWER_BOUND)
        val useWideLayout = isLandscape && isWidthAtLeastMedium

        if (useWideLayout) {
            LandscapeContent(
                isHosting = isHosting,
                onSetHostName = onSetHostName,
                hostTrainerName = hostTrainerName,
                messages = messages,
                onSendMessage = onSendMessage,
                showChat = showChat,
                onShowChatChanged = { showChat = it },
                game = game,
                gameFiles = gameFiles,
                onGameSelected = onGameSelected,
                showGamePicker = showGamePicker,
                onShowGamePickerChanged = { showGamePicker = it },
                players = players,
                hostInputAuthorityEnabled = hostInputAuthorityEnabled,
                networkMode = networkMode,
                onNetworkModeChanged = onNetworkModeChanged,
                buffer = buffer,
                onBufferChanged = onBufferChanged,
                autoBuffer = autoBuffer,
                onAutoBufferChanged = onAutoBufferChanged,
                clientBuffer = clientBuffer,
                onClientBufferChanged = onClientBufferChanged,
                joinAddresses = joinAddresses,
                selectedJoinInfoType = selectedJoinInfoType,
                onSelectedJoinInfoTypeChanged = { selectedJoinInfoType = it },
                scrollState = scrollState,
                contentPadding = innerPadding,
                modifier = modifier
            )
        } else {
            PortraitContent(
                isHosting = isHosting,
                onSetHostName = onSetHostName,
                hostTrainerName = hostTrainerName,
                messages = messages,
                onSendMessage = onSendMessage,
                showChat = showChat,
                onShowChatChanged = { showChat = it },
                game = game,
                gameFiles = gameFiles,
                onGameSelected = onGameSelected,
                showGamePicker = showGamePicker,
                onShowGamePickerChanged = { showGamePicker = it },
                players = players,
                hostInputAuthorityEnabled = hostInputAuthorityEnabled,
                networkMode = networkMode,
                onNetworkModeChanged = onNetworkModeChanged,
                buffer = buffer,
                onBufferChanged = onBufferChanged,
                autoBuffer = autoBuffer,
                onAutoBufferChanged = onAutoBufferChanged,
                clientBuffer = clientBuffer,
                onClientBufferChanged = onClientBufferChanged,
                joinAddresses = joinAddresses,
                selectedJoinInfoType = selectedJoinInfoType,
                onSelectedJoinInfoTypeChanged = { selectedJoinInfoType = it },
                scrollState = scrollState,
                contentPadding = innerPadding,
                modifier = modifier
            )
        }

        var showConnectionLostDialog by rememberSaveable { mutableStateOf(false) }
        LaunchedEffect(Unit) {
            connectionLost.collect { showConnectionLostDialog = true }
        }

        var traversalError by rememberSaveable { mutableStateOf<TraversalState.Failure?>(null) }
        LaunchedEffect(Unit) {
            fatalTraversalError.collect { traversalError = it }
        }

        var showNotAllPlayersHaveGame by rememberSaveable { mutableStateOf(false) }
        LaunchedEffect(Unit) {
            notAllPlayersHaveGame.collect { showNotAllPlayersHaveGame = true }
        }

        var dismissSaveTransferProgressDialog by rememberSaveable { mutableStateOf(false) }
        if (saveTransferProgress == null) {
            dismissSaveTransferProgressDialog = false
        }

        var dismissGameDigestDialog by rememberSaveable { mutableStateOf(false) }
        if (gameDigestProgress == null) {
            dismissGameDigestDialog = false
        }

        val currentTraversalError = traversalError

        when {
            showConnectionLostDialog -> {
                AlertDialog(
                    text = { Text(stringResource(R.string.netplay_connection_lost)) },
                    confirmButton = {
                        TextButton(onClick = onBackClicked) {
                            Text(stringResource(R.string.ok))
                        }
                    },
                    onDismissRequest = onBackClicked,
                )
            }

            currentTraversalError != null -> {
                AlertDialog(
                    text = { Text(currentTraversalError.message(LocalContext.current)) },
                    confirmButton = {
                        TextButton(onClick = onBackClicked) {
                            Text(stringResource(R.string.ok))
                        }
                    },
                    onDismissRequest = onBackClicked,
                )
            }

            saveTransferProgress != null && !dismissSaveTransferProgressDialog -> {
                SaveTransferProgressDialog(
                    saveTransferProgress = saveTransferProgress,
                    onDismiss = { dismissSaveTransferProgressDialog = true },
                )
            }

            gameDigestProgress != null && !dismissGameDigestDialog -> {
                GameDigestProgressDialog(
                    gameDigestProgress = gameDigestProgress,
                    onDismiss = { dismissGameDigestDialog = true },
                )
            }

            showNotAllPlayersHaveGame -> {
                AlertDialog(
                    title = { Text(stringResource(R.string.netplay_start_warning_title)) },
                    text = { Text(stringResource(R.string.netplay_start_warning_not_all_players_have_game)) },
                    confirmButton = {
                        TextButton(onClick = {
                            showNotAllPlayersHaveGame = false
                            onConfirmStartGame()
                        }) {
                            Text(stringResource(R.string.yes))
                        }
                    },
                    dismissButton = {
                        TextButton(onClick = { showNotAllPlayersHaveGame = false }) {
                            Text(stringResource(R.string.no))
                        }
                    },
                    onDismissRequest = { showNotAllPlayersHaveGame = false },
                )
            }
        }
    }
}

@Composable
private fun PortraitContent(
    isHosting: Boolean,
    onSetHostName: (String) -> Unit = {},
    hostTrainerName: String = "",
    messages: List<NetplayMessage>,
    onSendMessage: (String) -> Unit,
    showChat: Boolean,
    onShowChatChanged: (Boolean) -> Unit,
    game: String,
    gameFiles: List<GameFile>,
    onGameSelected: (GameFile) -> Unit,
    showGamePicker: Boolean,
    onShowGamePickerChanged: (Boolean) -> Unit,
    players: List<Player>,
    hostInputAuthorityEnabled: Boolean,
    networkMode: NetworkMode,
    onNetworkModeChanged: (NetworkMode) -> Unit,
    buffer: Int,
    onBufferChanged: (Int) -> Unit,
    autoBuffer: Boolean,
    onAutoBufferChanged: (Boolean) -> Unit,
    clientBuffer: Int,
    onClientBufferChanged: (Int) -> Unit,
    joinAddresses: Map<JoinInfoType, JoinAddress>,
    selectedJoinInfoType: JoinInfoType,
    onSelectedJoinInfoTypeChanged: (JoinInfoType) -> Unit,
    scrollState: ScrollState,
    contentPadding: PaddingValues,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier
            .verticalScroll(scrollState)
            .padding(contentPadding)
    ) {
        Chat(
            messages = messages,
            onSendMessage = onSendMessage,
            showBottomSheet = showChat,
            onShowBottomSheetChanged = onShowChatChanged,
            modifier = Modifier
                .fillMaxWidth()
                .height(200.dp)
                .padding(horizontal = DolphinTheme.scaffoldPadding)
        )

        MenuSpacer()

        PlayersAndSettings(
            game = game,
            gameFiles = gameFiles,
            onGameSelected = onGameSelected,
            showGamePicker = showGamePicker,
            onShowGamePickerChanged = onShowGamePickerChanged,
            players = players,
            hostInputAuthorityEnabled = hostInputAuthorityEnabled,
            networkMode = networkMode,
            onNetworkModeChanged = onNetworkModeChanged,
            buffer = buffer,
            onBufferChanged = onBufferChanged,
            autoBuffer = autoBuffer,
            onAutoBufferChanged = onAutoBufferChanged,
            clientBuffer = clientBuffer,
            onClientBufferChanged = onClientBufferChanged,
            isHosting = isHosting,
            onSetHostName = onSetHostName,
            hostTrainerName = hostTrainerName,
            joinAddresses = joinAddresses,
            selectedJoinInfoType = selectedJoinInfoType,
            onSelectedJoinInfoTypeChanged = onSelectedJoinInfoTypeChanged,
            modifier = Modifier
                .padding(horizontal = DolphinTheme.scaffoldPadding),
        )

        if (isHosting) {
            Spacer(modifier = Modifier.height(DolphinTheme.fabClearancePadding))
        }
    }
}

@Composable
private fun LandscapeContent(
    isHosting: Boolean,
    onSetHostName: (String) -> Unit = {},
    hostTrainerName: String = "",
    messages: List<NetplayMessage>,
    onSendMessage: (String) -> Unit,
    showChat: Boolean,
    onShowChatChanged: (Boolean) -> Unit,
    game: String,
    gameFiles: List<GameFile>,
    onGameSelected: (GameFile) -> Unit,
    showGamePicker: Boolean,
    onShowGamePickerChanged: (Boolean) -> Unit,
    players: List<Player>,
    hostInputAuthorityEnabled: Boolean,
    networkMode: NetworkMode,
    onNetworkModeChanged: (NetworkMode) -> Unit,
    buffer: Int,
    onBufferChanged: (Int) -> Unit,
    autoBuffer: Boolean,
    onAutoBufferChanged: (Boolean) -> Unit,
    clientBuffer: Int,
    onClientBufferChanged: (Int) -> Unit,
    joinAddresses: Map<JoinInfoType, JoinAddress>,
    selectedJoinInfoType: JoinInfoType,
    onSelectedJoinInfoTypeChanged: (JoinInfoType) -> Unit,
    scrollState: ScrollState,
    contentPadding: PaddingValues,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
    ) {
        Chat(
            messages = messages,
            onSendMessage = onSendMessage,
            showBottomSheet = showChat,
            onShowBottomSheetChanged = onShowChatChanged,
            modifier = Modifier
                .padding(contentPadding)
                .padding(
                    start = DolphinTheme.scaffoldPadding,
                    end = DolphinTheme.scaffoldPadding / 2,
                )
                .weight(1f)
                .fillMaxHeight()
        )

        val scrollState = rememberScrollState()
        Column(
            modifier = Modifier
                .weight(1f)
                .bottomFadeOverlay(scrollState, contentPadding.calculateBottomPadding())
                .verticalScroll(scrollState)
                .padding(contentPadding)
                .padding(
                    start = DolphinTheme.scaffoldPadding / 2,
                    end = DolphinTheme.scaffoldPadding
                )
        ) {
            PlayersAndSettings(
                game = game,
                gameFiles = gameFiles,
                onGameSelected = onGameSelected,
                showGamePicker = showGamePicker,
                onShowGamePickerChanged = onShowGamePickerChanged,
                players = players,
                hostInputAuthorityEnabled = hostInputAuthorityEnabled,
                networkMode = networkMode,
                onNetworkModeChanged = onNetworkModeChanged,
                buffer = buffer,
                onBufferChanged = onBufferChanged,
                autoBuffer = autoBuffer,
                onAutoBufferChanged = onAutoBufferChanged,
                clientBuffer = clientBuffer,
                onClientBufferChanged = onClientBufferChanged,
                isHosting = isHosting,
                onSetHostName = onSetHostName,
                hostTrainerName = hostTrainerName,
                joinAddresses = joinAddresses,
                selectedJoinInfoType = selectedJoinInfoType,
                onSelectedJoinInfoTypeChanged = onSelectedJoinInfoTypeChanged,
                modifier = Modifier
            )

            if (isHosting) {
                Spacer(modifier = Modifier.height(DolphinTheme.fabClearancePadding))
            }
        }
    }
}

/**
 * XD Netplay, host side: the joiner sets its in-game name in the Submit Team
 * sheet; the host sets its own here. Written straight into the GBA port 2
 * save the room syncs at start (with the party's OT names re-stamped).
 */
@Composable
private fun HostTrainerNameSection(onSetHostName: (String) -> Unit, hostTrainerName: String) {
    // Prefilled with the save's current name; re-seeded when that loads or changes.
    var name by rememberSaveable(hostTrainerName) { mutableStateOf(hostTrainerName) }
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = DolphinTheme.scaffoldPadding)
    ) {
        Text(
            stringResource(R.string.xd_host_name_title),
            style = MaterialTheme.typography.titleMedium
        )
        Text(
            stringResource(R.string.xd_host_name_hint),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Spacer(Modifier.height(8.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            OutlinedTextField(
                value = name,
                onValueChange = { name = it.take(7) },
                singleLine = true,
                label = { Text(stringResource(R.string.xd_host_name_label)) },
                modifier = Modifier.weight(1f)
            )
            Spacer(Modifier.width(8.dp))
            Button(
                onClick = { onSetHostName(name.trim()) },
                enabled = name.isNotBlank()
            ) {
                Text(stringResource(R.string.xd_host_name_set))
            }
        }
    }
}

@Composable
private fun PlayersAndSettings(
    game: String,
    gameFiles: List<GameFile>,
    onGameSelected: (GameFile) -> Unit,
    showGamePicker: Boolean,
    onShowGamePickerChanged: (Boolean) -> Unit,
    players: List<Player>,
    hostInputAuthorityEnabled: Boolean,
    networkMode: NetworkMode,
    onNetworkModeChanged: (NetworkMode) -> Unit,
    buffer: Int,
    onBufferChanged: (Int) -> Unit,
    autoBuffer: Boolean,
    onAutoBufferChanged: (Boolean) -> Unit,
    clientBuffer: Int,
    onClientBufferChanged: (Int) -> Unit,
    isHosting: Boolean,
    onSetHostName: (String) -> Unit = {},
    hostTrainerName: String = "",
    joinAddresses: Map<JoinInfoType, JoinAddress>,
    selectedJoinInfoType: JoinInfoType,
    onSelectedJoinInfoTypeChanged: (JoinInfoType) -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier
    ) {
        GamePicker(
            game = game,
            gameFiles = gameFiles,
            onGameSelected = onGameSelected,
            showGamePicker = showGamePicker,
            onShowGamePickerChanged = onShowGamePickerChanged,
            isHosting = isHosting,
        )

        if (isHosting) {
            MenuSpacer()

            HostTrainerNameSection(
                onSetHostName = onSetHostName,
                hostTrainerName = hostTrainerName
            )

            MenuSpacer()

            JoinAddressSection(
                joinAddresses = joinAddresses,
                selectedType = selectedJoinInfoType,
                onSelectedTypeChanged = onSelectedJoinInfoTypeChanged,
            )
        }

        MenuSpacer()

        OutlinedBox(
            label = { Text(stringResource(R.string.netplay_players_label)) },
        ) {
            PlayersTable(
                rows = buildList {
                    add(
                        listOf(
                            stringResource(R.string.netplay_players_name),
                            stringResource(R.string.netplay_players_ping),
                            stringResource(R.string.netplay_players_mapping),
                        )
                    )
                    addAll(players.map { listOf(it.name, it.ping.toString(), it.mapping) })
                    repeat(4 - players.size) { add(listOf("", "", "")) }
                },
                modifier = Modifier
                    .fillMaxWidth()
            )
        }

        if (isHosting) {
            MenuSpacer()

            NetworkModeDropdown(
                networkMode = networkMode,
                onNetworkModeChanged = onNetworkModeChanged,
            )
        }

        if (isHosting && !hostInputAuthorityEnabled) {
            MenuSpacer()

            // Host-side automatic buffer sizing. Only the host has one to size,
            // and only in fixed delay -- under host input authority each client
            // owns its own buffer, so the server leaves it alone.
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(Modifier.weight(1f)) {
                    Text(
                        stringResource(R.string.netplay_auto_buffer),
                        style = MaterialTheme.typography.bodyLarge
                    )
                    Text(
                        stringResource(R.string.netplay_auto_buffer_description),
                        style = MaterialTheme.typography.bodySmall
                    )
                }
                Switch(checked = autoBuffer, onCheckedChange = onAutoBufferChanged)
            }

            BufferInput(
                value = buffer,
                onValueChange = onBufferChanged,
                label = stringResource(R.string.netplay_buffer),
            )
        }

        if (!isHosting && hostInputAuthorityEnabled) {
            MenuSpacer()

            BufferInput(
                value = clientBuffer,
                onValueChange = onClientBufferChanged,
                label = stringResource(R.string.netplay_client_buffer),
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun Chat(
    messages: List<NetplayMessage>,
    onSendMessage: (String) -> Unit,
    showBottomSheet: Boolean,
    onShowBottomSheetChanged: (Boolean) -> Unit,
    modifier: Modifier,
) {
    val context = LocalContext.current

    fun LazyListScope.messages() {
        items(messages.size) { index ->
            val message = messages[index]
            Text(
                text = message.message(context),
                color = message.color(),
                style = MaterialTheme.typography.bodyMedium.copy(lineHeight = 18.sp),
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 2.dp)
            )
        }
    }

    var draftMessage by remember { mutableStateOf("") }
    val submitMessage = {
        onSendMessage(draftMessage)
        draftMessage = ""
    }

    val bottomSheetState = rememberSheetState(
        skipPartiallyExpanded = true,
        initialValue = if (showBottomSheet) SheetValue.Expanded else SheetValue.Hidden,
    )

    if (showBottomSheet) {
        ModalBottomSheet(
            onDismissRequest = { onShowBottomSheetChanged(false) },
            sheetState = bottomSheetState,
            modifier = Modifier
                .statusBarsPadding()
        ) {
            LazyColumn(
                reverseLayout = true,
                contentPadding = PaddingValues(bottom = 4.dp),
                modifier = Modifier
                    .weight(1f, fill = false)
                    .padding(horizontal = DolphinTheme.scaffoldPadding)
            ) {
                messages()
            }

            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(8.dp)
            ) {
                OutlinedTextField(
                    value = draftMessage,
                    onValueChange = { draftMessage = it },
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
                    keyboardActions = KeyboardActions(onSend = { submitMessage() }),
                    modifier = Modifier
                        .weight(1f)
                )
                IconButton(
                    onClick = submitMessage,
                    enabled = draftMessage.isNotBlank(),
                ) {
                    Icon(
                        imageVector = Icons.AutoMirrored.Filled.Send,
                        contentDescription = stringResource(R.string.netplay_chat_send),
                    )
                }
            }
        }
    }

    OutlinedBox(
        onClick = { onShowBottomSheetChanged(true) },
        label = { Text(stringResource(R.string.netplay_chat_label)) },
        fadeContentTop = true,
        modifier = modifier
    ) {
        LazyColumn(
            reverseLayout = true,
            userScrollEnabled = false,
            modifier = Modifier
                .fillMaxSize()
        ) {
            messages()
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun GamePicker(
    game: String,
    gameFiles: List<GameFile>,
    onGameSelected: (GameFile) -> Unit,
    showGamePicker: Boolean,
    onShowGamePickerChanged: (Boolean) -> Unit,
    isHosting: Boolean,
) {
    val bottomSheetState = rememberSheetState(
        skipPartiallyExpanded = true,
        initialValue = if (showGamePicker) SheetValue.Expanded else SheetValue.Hidden,
    )

    if (showGamePicker) {
        ModalBottomSheet(
            onDismissRequest = { onShowGamePickerChanged(false) },
            sheetState = bottomSheetState,
            modifier = Modifier.statusBarsPadding()
        ) {
            GameList(
                gameFiles = gameFiles,
                onGameSelected = { gameFile ->
                    onGameSelected(gameFile)
                    onShowGamePickerChanged(false)
                },
                contentPadding = PaddingValues(
                    start = DolphinTheme.scaffoldPadding,
                    end = DolphinTheme.scaffoldPadding,
                    bottom = 16.dp
                ),
            )
        }
    }

    ReadOnlyTextField(
        value = game,
        label = stringResource(R.string.netplay_game_label),
        onClick = if (isHosting) {
            { onShowGamePickerChanged(true) }
        } else {
            null
        },
        modifier = Modifier.fillMaxWidth()
    )
}

@Composable
private fun GameList(
    gameFiles: List<GameFile>,
    onGameSelected: (GameFile) -> Unit,
    contentPadding: PaddingValues = PaddingValues(),
) {
    LazyVerticalGrid(
        columns = GridCells.Adaptive(minSize = 120.dp),
        contentPadding = contentPadding,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        items(gameFiles, key = { it.getPath() }) { gameFile ->
            GameGridItem(
                gameFile = gameFile,
                onClick = { onGameSelected(gameFile) },
            )
        }
    }
}

@Composable
private fun GameGridItem(
    gameFile: GameFile,
    onClick: () -> Unit,
) {
    Card(
        onClick = onClick,
    ) {
        Column {
            AsyncImage(
                model = ImageRequest.Builder(LocalContext.current)
                    .data(gameFile)
                    .error(R.drawable.no_banner)
                    .build(),
                contentDescription = gameFile.getTitle(),
                contentScale = ContentScale.Crop,
                imageLoader = CoilUtils.imageLoader,
                modifier = Modifier
                    .fillMaxWidth()
                    .aspectRatio(0.7f)
            )
            Text(
                text = gameFile.getTitle(),
                style = MaterialTheme.typography.bodySmall,
                maxLines = 2,
                minLines = 2,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier
                    .padding(8.dp)
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun JoinAddressSection(
    joinAddresses: Map<JoinInfoType, JoinAddress>,
    selectedType: JoinInfoType,
    onSelectedTypeChanged: (JoinInfoType) -> Unit,
) {
    val address = joinAddresses[selectedType] ?: joinAddresses.values.first()

    @Suppress("UnusedBoxWithConstraintsScope")
    BoxWithConstraints(modifier = Modifier.fillMaxWidth()) {
        if (maxWidth > 392.dp) {
            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                JoinInfoDropdown(
                    joinAddresses = joinAddresses,
                    selectedType = selectedType,
                    onSelectedTypeChanged = onSelectedTypeChanged,
                    modifier = Modifier.weight(0.39f),
                )
                AddressRow(
                    joinInfoType = selectedType,
                    address = address,
                    modifier = Modifier.weight(0.61f),
                )
            }
        } else {
            Column(modifier = Modifier.fillMaxWidth()) {
                JoinInfoDropdown(
                    joinAddresses = joinAddresses,
                    selectedType = selectedType,
                    onSelectedTypeChanged = onSelectedTypeChanged,
                    modifier = Modifier.fillMaxWidth(),
                )
                MenuSpacer()
                AddressRow(
                    joinInfoType = selectedType,
                    address = address,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun JoinInfoDropdown(
    joinAddresses: Map<JoinInfoType, JoinAddress>,
    selectedType: JoinInfoType,
    onSelectedTypeChanged: (JoinInfoType) -> Unit,
    modifier: Modifier = Modifier,
) {
    var expanded by remember { mutableStateOf(false) }

    ExposedDropdownMenuBox(
        expanded = expanded,
        onExpandedChange = { expanded = it },
        modifier = modifier,
    ) {
        OutlinedTextField(
            value = stringResource(selectedType.labelId),
            onValueChange = {},
            readOnly = true,
            label = { Text(stringResource(R.string.netplay_host_address_label)) },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded) },
            modifier = Modifier
                .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable)
                .fillMaxWidth()
        )

        ExposedDropdownMenu(
            expanded = expanded,
            onDismissRequest = { expanded = false },
        ) {
            joinAddresses.keys.forEach { type ->
                DropdownMenuItem(
                    text = { Text(stringResource(type.labelId)) },
                    onClick = {
                        onSelectedTypeChanged(type)
                        expanded = false
                    },
                    contentPadding = ExposedDropdownMenuDefaults.ItemContentPadding,
                )
            }
        }
    }
}

@Composable
private fun AddressRow(
    joinInfoType: JoinInfoType,
    address: JoinAddress,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current

    ReadOnlyTextField(
        value = when (address) {
            is JoinAddress.Loading -> stringResource(R.string.netplay_address_loading)
            is JoinAddress.Loaded -> address.address
            is JoinAddress.Unknown -> stringResource(R.string.netplay_address_unknown)
        },
        label = stringResource(
            if (joinInfoType == JoinInfoType.ROOM_ID) R.string.netplay_code_label
            else R.string.netplay_address_label
        ),
        onClick = when (address) {
            is JoinAddress.Loaded -> {
                {
                    val intent = Intent(Intent.ACTION_SEND).apply {
                        type = "text/plain"
                        putExtra(Intent.EXTRA_TEXT, address.address)
                    }
                    context.startActivity(Intent.createChooser(intent, null))
                }
            }

            is JoinAddress.Unknown -> address.retry
            is JoinAddress.Loading -> null
        },
        textStyle = if (address is JoinAddress.Loading) {
            LocalTextStyle.current.copy(color = MaterialTheme.colorScheme.onSurfaceVariant)
        } else {
            null
        },
        trailingIcon = {
            when (address) {
                is JoinAddress.Loaded -> Icon(
                    imageVector = Icons.Filled.Share,
                    contentDescription = stringResource(R.string.netplay_address_share),
                )

                is JoinAddress.Unknown -> Icon(
                    imageVector = Icons.Filled.Refresh,
                    contentDescription = stringResource(R.string.netplay_address_retry),
                )

                is JoinAddress.Loading -> CircularProgressIndicator(
                    modifier = Modifier.size(24.dp),
                    strokeWidth = 2.dp,
                )
            }
        },
        modifier = modifier,
    )
}

/**
 * A table arranged into columns sized to wrap the largest item. Except the
 * first column which takes up the remaining space left by the other columns.
 * The first row is treated as the column titles.
 */
@Composable
private fun PlayersTable(
    rows: List<List<String>>,
    modifier: Modifier = Modifier,
) {
    rows.zipWithNext { a, b -> if (a.size != b.size) throw IllegalArgumentException("Rows must all contain the same number of elements.") }
    val maxWidths = remember { List(rows.first().size) { mutableIntStateOf(0) } }
    val density = LocalDensity.current

    Column(
        verticalArrangement = Arrangement.spacedBy(6.dp),
        modifier = modifier
    ) {
        rows.forEachIndexed { rowIndex, row ->
            Row(
                horizontalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                row.forEachIndexed { itemIndex, text ->
                    Box(
                        modifier = Modifier
                            .then(
                                when {
                                    itemIndex == 0 -> Modifier.weight(1f)

                                    maxWidths[itemIndex].intValue > 0 -> Modifier
                                        .width(with(density) { maxWidths[itemIndex].intValue.toDp() })

                                    else -> Modifier
                                }
                            )
                            .onGloballyPositioned { coordinates ->
                                val width = coordinates.size.width
                                if (width > maxWidths[itemIndex].intValue) {
                                    maxWidths[itemIndex].intValue = width
                                }
                            }
                    ) {
                        Text(
                            text = text,
                            fontWeight = if (rowIndex == 0) FontWeight.Medium else FontWeight.Normal,
                            style = MaterialTheme.typography.bodyMedium,
                        )
                    }
                }
            }
            if (rowIndex == 0) {
                HorizontalDivider()
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun NetworkModeDropdown(
    networkMode: NetworkMode,
    onNetworkModeChanged: (NetworkMode) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }

    ExposedDropdownMenuBox(
        expanded = expanded,
        onExpandedChange = { expanded = it },
    ) {
        OutlinedTextField(
            value = stringResource(networkMode.labelId),
            onValueChange = {},
            readOnly = true,
            label = { Text(stringResource(R.string.netplay_network_mode_label)) },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) },
            colors = ExposedDropdownMenuDefaults.outlinedTextFieldColors(),
            modifier = Modifier
                .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable)
                .fillMaxWidth(),
        )
        ExposedDropdownMenu(
            expanded = expanded,
            onDismissRequest = { expanded = false },
        ) {
            // No golf mode for now since it requires in game UI.
            listOf(NetworkMode.FAIR_INPUT_DELAY, NetworkMode.HOST_INPUT_AUTHORITY).forEach { mode ->
                DropdownMenuItem(
                    text = { Text(stringResource(mode.labelId)) },
                    onClick = {
                        onNetworkModeChanged(mode)
                        expanded = false
                    },
                )
            }
        }
    }
}

@Composable
private fun BufferInput(
    value: Int,
    onValueChange: (Int) -> Unit,
    label: String,
) {
    val range = 0..99
    var maybeEmptyValue by remember(value) {
        mutableStateOf("$value")
    }

    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
    ) {
        OutlinedTextField(
            value = TextFieldValue(
                text = maybeEmptyValue,
                selection = TextRange(maybeEmptyValue.length)
            ),
            onValueChange = { newValue ->
                if (newValue.text.isEmpty()) {
                    maybeEmptyValue = newValue.text
                    return@OutlinedTextField
                }
                newValue.text.toIntOrNull()?.let {
                    if (it in range) {
                        onValueChange(it)
                    }
                }
            },
            label = { Text(label) },
            textStyle = LocalTextStyle.current.copy(
                textAlign = TextAlign.Center
            ),
            keyboardOptions = KeyboardOptions(
                keyboardType = KeyboardType.Number,
            ),
            singleLine = true,
            modifier = Modifier
                .weight(1f)
        )

        Spacer(modifier = Modifier.width(12.dp))

        Button(
            onClick = {
                if (maybeEmptyValue.isEmpty()) {
                    maybeEmptyValue = "0"
                    onValueChange(0)
                } else {
                    val newValue = value - 1
                    if (newValue in range) {
                        onValueChange(newValue)
                    }
                }
            },
            shape = RoundedCornerShape(
                topStartPercent = 50,
                topEndPercent = 0,
                bottomEndPercent = 0,
                bottomStartPercent = 50,
            ),
            modifier = Modifier
                .height(60.dp)
                .padding(top = 8.dp)
        ) {
            Icon(Icons.Filled.Remove, contentDescription = "Back")
        }

        Spacer(modifier = Modifier.width(2.dp))

        Button(
            onClick = {
                if (maybeEmptyValue.isEmpty()) {
                    maybeEmptyValue = "0"
                    onValueChange(0)
                } else {
                    val newValue = value + 1
                    if (newValue in range) {
                        onValueChange(newValue)
                    }
                }
            },
            shape = RoundedCornerShape(
                topStartPercent = 0,
                topEndPercent = 50,
                bottomEndPercent = 50,
                bottomStartPercent = 0,
            ),
            modifier = Modifier
                .height(60.dp)
                .padding(top = 8.dp)
        ) {
            Icon(Icons.Filled.Add, contentDescription = "Back")
        }
    }
}

@Composable
private fun SaveTransferProgressDialog(
    saveTransferProgress: SaveTransferProgress,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        title = { Text(saveTransferProgress.title) },
        text = {
            Column(
                verticalArrangement = Arrangement.spacedBy(12.dp),
                modifier = Modifier.verticalScroll(rememberScrollState()),
            ) {
                saveTransferProgress.playerProgresses.forEachIndexed { index, playerProgress ->
                    SaveTransferProgressRow(
                        playerProgress = playerProgress,
                        totalSize = saveTransferProgress.totalSize,
                    )

                    if (index < saveTransferProgress.playerProgresses.lastIndex) {
                        HorizontalDivider()
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text(stringResource(R.string.netplay_save_transfer_progress_close))
            }
        },
        onDismissRequest = onDismiss,
    )
}

@Composable
private fun SaveTransferProgressRow(
    playerProgress: SaveTransferProgress.PlayerProgress,
    totalSize: Long,
) {
    fun formatMib(bytes: Long) = String.format(Locale.US, "%.2f", bytes / 1024f / 1024f)
    val progressFraction = (playerProgress.progress.toFloat() / totalSize).coerceIn(0f, 1f)

    Column(
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        LinearProgressIndicator(
            progress = { progressFraction },
            modifier = Modifier.fillMaxWidth(),
        )
        Row(
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(
                text = playerProgress.name,
                modifier = Modifier.weight(1f),
            )
            Spacer(modifier = Modifier.width(16.dp))
            Text(
                text = stringResource(
                    R.string.netplay_transfer_progress,
                    formatMib(playerProgress.progress),
                    formatMib(totalSize)
                ),
            )
        }
    }
}

@Composable
private fun GameDigestProgressDialog(
    gameDigestProgress: GameDigestProgress,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        title = { Text(gameDigestProgress.title) },
        text = {
            Column(
                verticalArrangement = Arrangement.spacedBy(12.dp),
                modifier = Modifier.verticalScroll(rememberScrollState()),
            ) {
                gameDigestProgress.playerProgresses.forEachIndexed { index, playerProgress ->
                    GameDigestPlayerRow(playerProgress)
                    if (index < gameDigestProgress.playerProgresses.lastIndex) {
                        HorizontalDivider()
                    }
                }
                if (gameDigestProgress.matches != null) {
                    Spacer(modifier = Modifier.height(4.dp))
                    Text(
                        text = stringResource(
                            if (gameDigestProgress.matches) {
                                R.string.netplay_game_digest_match
                            } else {
                                R.string.netplay_game_digest_mismatch
                            }
                        ),
                        style = MaterialTheme.typography.bodyLarge,
                        fontWeight = FontWeight.Medium,
                    )
                }
            }
        },
        confirmButton = {
            if (gameDigestProgress.matches != null) {
                TextButton(onClick = onDismiss) {
                    Text(stringResource(R.string.netplay_game_digest_close))
                }
            }
        },
        onDismissRequest = { onDismiss() },
    )
}

@Composable
private fun GameDigestPlayerRow(
    playerProgress: GameDigestProgress.PlayerProgress,
) {
    Column(
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        LinearProgressIndicator(
            progress = { playerProgress.progress / 100f },
            modifier = Modifier.fillMaxWidth(),
        )
        Row(
            modifier = Modifier.fillMaxWidth(),
        ) {
            if (playerProgress.result == null) {
                Text(
                    text = playerProgress.name,
                )
                Spacer(modifier = Modifier.weight(1f))
                Text(
                    text = "${playerProgress.progress}%",
                )
            } else {
                Text(
                    text = "${playerProgress.name}:\u00A0${playerProgress.result}",
                )
            }
        }
    }
}

@Composable
private fun NetplayMessage.color(): Color {
    val isDark = isSystemInDarkTheme()
    return when (this) {
        is NetplayMessage.Chat -> Color.Unspecified
        is NetplayMessage.GameChanged -> if (isDark) Color(0xFFCE93D8) else Color(0xFF8E24AA)
        is NetplayMessage.HostInputAuthorityChanged -> if (isDark) Color(0xFF90CAF9) else Color(
            0xFF1565C0
        )

        is NetplayMessage.BufferChanged -> if (isDark) Color(0xFF80CBC4) else Color(0xFF00897B)
        is NetplayMessage.Desync -> if (isDark) Color(0xFFEF9A9A) else Color(0xFFC62828)
    }
}

@Preview
@Composable
private fun NetplayScreenPreview() {
    PreviewTheme(darkTheme = false) {
        PreviewNetplayScreen()
    }
}

@Preview(uiMode = Configuration.UI_MODE_NIGHT_YES)
@Composable
private fun NetplayScreenDarkPreview() {
    PreviewTheme(darkTheme = true) {
        PreviewNetplayScreen()
    }
}

@Preview(widthDp = 891, heightDp = 411)
@Composable
private fun LandscapeNetplayScreenPreview() {
    PreviewTheme(darkTheme = false) {
        PreviewNetplayScreen()
    }
}

@Preview(
    widthDp = 891,
    heightDp = 411,
    uiMode = Configuration.UI_MODE_NIGHT_YES
)

@Composable
private fun LandscapeNetplayScreenDarkPreview() {
    PreviewTheme(darkTheme = true) {
        PreviewNetplayScreen()
    }
}

@Composable
private fun PreviewNetplayScreen() {
    NetplayScreen(
        onBackClicked = {},
        connectionLost = emptyFlow(),
        fatalTraversalError = emptyFlow(),
        players = listOf(
            Player(
                pid = 1,
                name = "Player 1",
                revision = "123",
                ping = 2,
                isHost = true,
                mapping = "m1"
            ),
            Player(
                pid = 2,
                name = "Player 2",
                revision = "123",
                ping = 23,
                isHost = false,
                mapping = "m2"
            ),
        ),
        messages = buildList {
            repeat(5) {
                add(NetplayMessage.Chat("Hello"))
            }
        },
        onSendMessage = {},
        game = "Game name",
        isHosting = true,
        onStartGame = {},
        onGameSelected = {},
        gameFiles = emptyList(),
        notAllPlayersHaveGame = emptyFlow(),
        onConfirmStartGame = {},
        hostInputAuthorityEnabled = true,
        networkMode = NetworkMode.HOST_INPUT_AUTHORITY,
        onNetworkModeChanged = {},
        buffer = 5,
        onBufferChanged = {},
        autoBuffer = true,
        onAutoBufferChanged = {},
        clientBuffer = 10,
        onClientBufferChanged = {},
        saveTransferProgress = null,
        gameDigestProgress = null,
        joinAddresses = mapOf(
            JoinInfoType.EXTERNAL to JoinAddress.Loaded("203.0.113.1:2626"),
            JoinInfoType.LOCAL to JoinAddress.Loaded("192.168.1.5:2626"),
        ),
        onSubmitTeam = { _, _, _, _ -> },
        onSubmitSaveBundle = { _, _ -> },
        initialTeamText = "",
        initialTrainerName = "PLAYER",
        initialModelId = 0,
        initialUseMySave = false,
        modelOptions = emptyList(),
        orreFormatLocal = false,
        validateTeamForFormat = { "" },
//        saveTransferProgress = SaveTransferProgress(
//            title = "Title",
//            totalSize = 1024L,
//            playerProgresses = listOf(
//                SaveTransferProgress.PlayerProgress(
//                    playerId = 1,
//                    name = "Player 1",
//                    progress = 256,
//                ),
//                SaveTransferProgress.PlayerProgress(
//                    playerId = 2,
//                    name = "Player 2",
//                    progress = 512,
//                ),
//            ),
//        ),
    )
}
