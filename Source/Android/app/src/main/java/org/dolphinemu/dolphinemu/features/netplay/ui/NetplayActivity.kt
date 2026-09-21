// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.netplay.ui

import android.content.Context
import android.content.Intent
import android.os.Bundle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.runtime.collectAsState
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.flowWithLifecycle
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import org.dolphinemu.dolphinemu.activities.EmulationActivity
import org.dolphinemu.dolphinemu.features.netplay.NetplayManager
import org.dolphinemu.dolphinemu.features.netplay.model.NetplayViewModel
import org.dolphinemu.dolphinemu.features.settings.model.IntSetting
import org.dolphinemu.dolphinemu.features.xdnetplay.BattleStyleBridge
import org.dolphinemu.dolphinemu.features.xdnetplay.FormatBridge
import org.dolphinemu.dolphinemu.ui.main.ThemeProvider
import org.dolphinemu.dolphinemu.ui.theme.DolphinTheme
import org.dolphinemu.dolphinemu.utils.NetworkHelper
import org.dolphinemu.dolphinemu.utils.ThemeHelper

class NetplayActivity : AppCompatActivity(), ThemeProvider {
    override var themeId: Int = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeHelper.setTheme(this)
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)

        val session = NetplayManager.activeSession
        if (session == null) {
            finish()
            return
        }

        val viewModel = ViewModelProvider(this, NetplayViewModel.Factory(session, NetworkHelper))[NetplayViewModel::class.java]

        viewModel.launchGame
            .flowWithLifecycle(lifecycle, Lifecycle.State.STARTED)
            .onEach { EmulationActivity.launch(this, it, false) }
            .launchIn(lifecycleScope)

        // Static native table for the joiner's cosmetic model picker in the
        // Submit Team sheet; fetched once, outside composition. The pick
        // travels with the team submission, so no extra session state exists.
        val modelOptions = BattleStyleBridge.modelTable()

        // HOST only: the host's own Submit Team sheet opens on the stored
        // "Your model" pick, and the room's Music & Location dialog needs its
        // two tables and stored picks. Read once, like the launcher does; the
        // screen tracks whatever the host applies after that. A joiner never
        // sees any of it, so nothing is fetched for one.
        val isHosting = viewModel.isHosting
        val musicOptions =
            if (isHosting) BattleStyleBridge.musicTable() else emptyList()
        val venueOptions =
            if (isHosting) BattleStyleBridge.venueTable() else emptyList()
        val initialHostModelId =
            if (isHosting) BattleStyleBridge.getSelection(BattleStyleBridge.SELECTION_HOST_MODEL)
            else 0
        val initialMusicId =
            if (isHosting) BattleStyleBridge.getSelection(BattleStyleBridge.SELECTION_MUSIC) else 0
        val initialVenueId =
            if (isHosting) BattleStyleBridge.getSelection(BattleStyleBridge.SELECTION_VENUE) else 0

        // Last-submitted Submit Team sheet state (config-backed), so the sheet
        // opens pre-filled instead of empty. Read once: while this activity
        // lives, the sheet's own drafts already hold anything newer.
        val submitPrefill = viewModel.submitPrefill()

        // THIS device's Format pick, read once for the sheet's non-blocking
        // paste-time note. Only advisory here: the room is governed by the
        // HOST's key, enforced host-side in shared core (TeamInjector).
        val localFormat = IntSetting.MAIN_XD_FORMAT.int
        val orreFormatLocal = FormatBridge.hasTeamRules(localFormat)
        val localFormatName = FormatBridge.displayName(localFormat)

        setContent {
            DolphinTheme {
                val hostActionResult = viewModel.hostActionResult.collectAsState().value
                NetplayScreen(
                    onBackClicked = { finish() },
                    isHosting = viewModel.isHosting,
                    connectionLost = viewModel.connectionLost,
                    fatalTraversalError = viewModel.fatalTraversalError,
                    messages = viewModel.messages.collectAsState().value,
                    onSendMessage = viewModel::sendMessage,
                    onSubmitTeam = viewModel::submitTeam,
                    onSubmitSaveBundle = viewModel::submitSaveBundle,
                    onSetHostName = viewModel::setHostTrainerName,
                    hostTrainerName = viewModel.hostTrainerName.collectAsState().value,
                    initialTeamText = submitPrefill.teamText,
                    initialTrainerName = submitPrefill.trainerName,
                    initialModelId = submitPrefill.modelId,
                    initialUseMySave = submitPrefill.useMySave,
                    modelOptions = modelOptions,
                    orreFormatLocal = orreFormatLocal,
                    localFormatName = localFormatName,
                    validateTeamForFormat = FormatBridge::validateShowdown,
                    onSubmitHostTeam = viewModel::submitHostTeam,
                    initialHostModelId = initialHostModelId,
                    musicOptions = musicOptions,
                    venueOptions = venueOptions,
                    initialMusicId = initialMusicId,
                    initialVenueId = initialVenueId,
                    onSetMusicAndLocation = viewModel::setMusicAndLocation,
                    // Only the host has controls that grey out on it, so only a
                    // host subscribes (the flow polls while subscribed).
                    gameRunning = isHosting && viewModel.gameRunning.collectAsState().value,
                    hostResultText = hostActionResult?.text ?: "",
                    hostResultOk = hostActionResult?.ok ?: true,
                    game = viewModel.game.collectAsState().value,
                    onStartGame = viewModel::startGame,
                    onGameSelected = viewModel::changeGame,
                    gameFiles = viewModel.gameFiles.collectAsState().value,
                    notAllPlayersHaveGame = viewModel.notAllPlayersHaveGame,
                    onConfirmStartGame = viewModel::confirmStartGame,
                    players = viewModel.players.collectAsState().value,
                    hostInputAuthorityEnabled = viewModel.hostInputAuthority.collectAsState().value,
                    networkMode = viewModel.networkMode.collectAsState().value,
                    onNetworkModeChanged = viewModel::setNetworkMode,
                    buffer = viewModel.buffer.collectAsState().value,
                    onBufferChanged = viewModel::setBuffer,
                    autoBuffer = viewModel.autoBuffer.collectAsState().value,
                    onAutoBufferChanged = viewModel::setAutoBuffer,
                    clientBuffer = viewModel.clientBuffer.collectAsState().value,
                    onClientBufferChanged = viewModel::setClientBuffer,
                    saveTransferProgress = viewModel.saveTransferProgress.collectAsState().value,
                    gameDigestProgress = viewModel.gameDigestProgress.collectAsState().value,
                    joinAddresses = viewModel.joinAddresses.collectAsState().value,
                )
            }
        }
    }

    override fun setTheme(themeId: Int) {
        super.setTheme(themeId)
        this.themeId = themeId
    }

    override fun onResume() {
        ThemeHelper.setCorrectTheme(this)
        super.onResume()
    }

    companion object {
        @JvmStatic
        fun launch(context: Context) {
            context.startActivity(Intent(context, NetplayActivity::class.java))
        }
    }
}
