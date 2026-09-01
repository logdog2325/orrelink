// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.netplay.model

import android.util.Base64
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.asFlow
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.DelicateCoroutinesApi
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.dolphinemu.dolphinemu.features.netplay.NetplaySession
import org.dolphinemu.dolphinemu.features.settings.model.BooleanSetting
import org.dolphinemu.dolphinemu.features.settings.model.IntSetting
import org.dolphinemu.dolphinemu.features.settings.model.NativeConfig
import org.dolphinemu.dolphinemu.features.settings.model.StringSetting
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.EmeraldSave
import org.dolphinemu.dolphinemu.model.GameFile
import org.dolphinemu.dolphinemu.services.GameFileCacheManager
import org.dolphinemu.dolphinemu.utils.NetworkHelper

class NetplayViewModel(
    private val netplaySession: NetplaySession,
    private val networkHelper: NetworkHelper,
) : ViewModel() {

    private val isTraversal = StringSetting.NETPLAY_TRAVERSAL_CHOICE.string == "traversal"

    val launchGame = netplaySession.launchGame

    val isHosting = netplaySession.isHosting

    private val _joinAddresses = MutableStateFlow(
        buildMap {
            if (isHosting) {
                if (isTraversal) {
                    put(JoinInfoType.ROOM_ID, JoinAddress.Loading)
                }
                else {
                    // Map order is the chip order, and the first entry is the default selection.
                    // A DIRECT host's useful address is almost always the LAN one -- the external
                    // address only works with a port forward -- so Local leads. Traversal keeps
                    // the room code first for the same reason.
                    put(JoinInfoType.LOCAL, getLocalIp())
                }
                put(JoinInfoType.EXTERNAL, JoinAddress.Loading)
                if (isTraversal) {
                    put(JoinInfoType.LOCAL, getLocalIp())
                }
            }
        }
    )
    val joinAddresses = _joinAddresses.asStateFlow()

    val connectionLost = netplaySession.connectionLost

    val fatalTraversalError = netplaySession.fatalTraversalError

    val players = netplaySession.players
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(), emptyList())

    val messages = netplaySession.messages
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(), emptyList())

    val game = netplaySession.game
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(), "")

    val hostInputAuthority = netplaySession.hostInputAuthorityEnabled
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(), false)

    private val _networkMode = MutableStateFlow(
        NetworkMode.fromConfigValue(StringSetting.NETPLAY_NETWORK_MODE.string)
    )
    val networkMode = _networkMode.asStateFlow()

    private val _buffer = MutableStateFlow(IntSetting.NETPLAY_BUFFER_SIZE.int)
    val buffer = _buffer.asStateFlow()

    private val _autoBuffer = MutableStateFlow(BooleanSetting.NETPLAY_AUTO_BUFFER.boolean)
    val autoBuffer = _autoBuffer.asStateFlow()

    private val _clientBuffer = MutableStateFlow(IntSetting.NETPLAY_CLIENT_BUFFER_SIZE.int)
    val clientBuffer = _clientBuffer.asStateFlow()

    val gameFiles = GameFileCacheManager.getGameFiles().asFlow()
        .map { it.toList() }
        .stateIn(
            viewModelScope,
            SharingStarted.WhileSubscribed(),
            GameFileCacheManager.getGameFiles().value?.toList() ?: emptyList()
        )

    val saveTransferProgress = netplaySession.saveTransferProgress

    val gameDigestProgress = netplaySession.gameDigestProgress

    init {
        // The host's buffer can now move on its own (automatic sizing decides on
        // the netplay thread and broadcasts MessageID::PadBuffer). Mirror every
        // broadcast value into the field so it always shows what is actually
        // live, not just what someone last typed.
        netplaySession.padBuffer
            .onEach { _buffer.value = it }
            .launchIn(viewModelScope)

        if (netplaySession.isHosting) {
            setInitialGame()
            if (isTraversal) {
                collectTraversalState()
            } else {
                fetchExternalIp()
            }
        }
    }

    private val _notAllPlayersHaveGame = Channel<Unit>(Channel.CONFLATED)
    val notAllPlayersHaveGame = _notAllPlayersHaveGame.receiveAsFlow()

    fun startGame() {
        if (netplaySession.doAllPlayersHaveGame()) {
            netplaySession.startGame()
        } else {
            _notAllPlayersHaveGame.trySend(Unit)
        }
    }

    fun confirmStartGame() {
        netplaySession.startGame()
    }

    /** Default in-game name offered in the submit sheet: this player's netplay
     *  nickname, cut down to what a Gen 3 save can actually hold. */
    val defaultTrainerName: String
        get() = EmeraldSave.sanitizeTrainerName(netplaySession.nickName)

    /** What the Submit Team sheet opens pre-filled with. */
    data class SubmitPrefill(
        val teamText: String,
        val trainerName: String,
        val modelId: Int,
        val useMySave: Boolean,
    )

    /**
     * The last-submitted Submit Team sheet state, from the MAIN_XD_SUBMIT_*
     * config keys (stored by [submitTeam]/[submitSaveBundle] on each
     * successful submit), so a joiner does not retype team, name and model
     * every session. The team text round-trips through base64 because a
     * Showdown export is multi-line and Dolphin's INI config layer is
     * line-based; the same encoding is read/written by the desktop dialog.
     * A never-stored (or unsanitizable) name falls back to the netplay
     * nickname, exactly what the sheet always pre-filled.
     */
    fun submitPrefill(): SubmitPrefill {
        val storedName = EmeraldSave.sanitizeTrainerName(StringSetting.MAIN_XD_SUBMIT_NAME.string)
        return SubmitPrefill(
            teamText = decodeStoredTeamText(StringSetting.MAIN_XD_SUBMIT_TEAM_B64.string),
            trainerName = storedName.ifEmpty { defaultTrainerName },
            modelId = IntSetting.MAIN_XD_SUBMIT_MODEL.int,
            useMySave = BooleanSetting.MAIN_XD_SUBMIT_USE_SAVE.boolean,
        )
    }

    /**
     * XD Netplay: hand this player's own team to the host, which writes it into
     * the GBA save it syncs when the battle starts. A pokepast.es link is
     * resolved HERE, on the submitting device, so the host only ever parses
     * plain text it was handed -- it never fetches a URL a stranger chose.
     *
     * [trainerName] is the in-game name to play under. It is sanitized to at
     * most seven Gen 3 characters; anything that survives nothing (an all-emoji
     * name, say) is sent as "", which the host reads as "keep your own name"
     * rather than as a reason to reject the team.
     *
     * [modelId] is this player's cosmetic trainer-model pick for the battle;
     * 0 (or anything non-positive) means "no preference" — no Model: header is
     * sent and the host's Guest-model fallback dropdown decides. The host
     * validates the id against its own table, so a stale pick can never be
     * applied, and it can never reject the team.
     */
    fun submitTeam(text: String, trainerName: String, modelId: Int, raiseToLevel100: Boolean = false) {
        val trimmed = text.trim()
        if (trimmed.isEmpty()) {
            return
        }
        val name = EmeraldSave.sanitizeTrainerName(trainerName)
        if (name.isEmpty() && trainerName.isNotBlank()) {
            netplaySession.showLocalMessage(
                "That in-game name has no Gen 3 equivalent — sending the team without it."
            )
        }
        val pokepaste = Regex("^https?://pokepast\\.es/[A-Za-z0-9]+").find(trimmed)?.value
        if (pokepaste == null) {
            storeSubmitDrafts(trimmed, name, modelId, useMySave = false)
            netplaySession.submitTeam(trimmed, name, modelId, raiseToLevel100)
            return
        }
        viewModelScope.launch(Dispatchers.IO) {
            val body = try {
                java.net.URL("$pokepaste/raw").readText()
            } catch (e: Exception) {
                null
            }
            withContext(Dispatchers.Main) {
                if (body == null) {
                    netplaySession.showLocalMessage(
                        "Could not fetch that paste (network error)."
                    )
                } else {
                    // Store what the user typed (the link, not the fetched
                    // body) so next session's sheet prefills the same way.
                    storeSubmitDrafts(trimmed, name, modelId, useMySave = false)
                    netplaySession.submitTeam(body, name, modelId, raiseToLevel100)
                }
            }
        }
    }

    /** The host's current in-game trainer name (GBA port 2 save), for prefilling. */
    val hostTrainerName = MutableStateFlow("")

    init {
        viewModelScope.launch(Dispatchers.IO) {
            val current = netplaySession.hostTrainerName()
            withContext(Dispatchers.Main) { hostTrainerName.value = current }
        }
    }

    /** Host only: rename the trainer in the GBA port 2 save (the one the room syncs). */
    fun setHostTrainerName(name: String) {
        viewModelScope.launch(Dispatchers.IO) {
            val status = netplaySession.setHostTrainerName(name)
            val current = netplaySession.hostTrainerName()
            withContext(Dispatchers.Main) {
                hostTrainerName.value = current
                netplaySession.showLocalMessage(status)
            }
        }
    }

    /**
     * XD Netplay "Use my save": submit the party from this player's OWN local
     * save -- real mon bytes, real trainer identity -- instead of a Showdown
     * paste. The extraction and refusals (FireRed/LeafGreen save, empty
     * party, no save at all) live native-side; a refusal surfaces as a local
     * chat line and nothing is sent. The in-game-name field does not apply
     * here (the save's real trainer name always wins; see NetplaySession), so
     * only [modelId] rides along.
     */
    fun submitSaveBundle(modelId: Int, raiseToLevel100: Boolean = false) {
        viewModelScope.launch(Dispatchers.IO) {
            val error = netplaySession.submitSaveBundle(modelId, raiseToLevel100)
            withContext(Dispatchers.Main) {
                if (error.isEmpty()) {
                    // The team/name drafts were not part of this submission;
                    // leave their stored values alone.
                    storeSubmitDrafts(null, null, modelId, useMySave = true)
                } else {
                    netplaySession.showLocalMessage(
                        "Could not submit the team from your save — $error."
                    )
                }
            }
        }
    }

    /**
     * Persist the Submit Team sheet's state on a successful submit, so the
     * sheet opens pre-filled next session ([submitPrefill]). A null
     * [teamText]/[trainerName] leaves that stored value untouched (the bundle
     * path, which uses neither).
     */
    private fun storeSubmitDrafts(
        teamText: String?,
        trainerName: String?,
        modelId: Int,
        useMySave: Boolean,
    ) {
        if (teamText != null) {
            StringSetting.MAIN_XD_SUBMIT_TEAM_B64.setString(
                NativeConfig.LAYER_BASE,
                Base64.encodeToString(teamText.toByteArray(Charsets.UTF_8), Base64.NO_WRAP)
            )
        }
        if (trainerName != null) {
            StringSetting.MAIN_XD_SUBMIT_NAME.setString(NativeConfig.LAYER_BASE, trainerName)
        }
        IntSetting.MAIN_XD_SUBMIT_MODEL.setInt(NativeConfig.LAYER_BASE, modelId)
        BooleanSetting.MAIN_XD_SUBMIT_USE_SAVE.setBoolean(NativeConfig.LAYER_BASE, useMySave)
        NativeConfig.save(NativeConfig.LAYER_BASE)
    }

    /** Base64 -> Showdown text; "" on a corrupt stored value (prefill is best-effort). */
    private fun decodeStoredTeamText(b64: String): String =
        try {
            String(Base64.decode(b64, Base64.DEFAULT), Charsets.UTF_8)
        } catch (e: IllegalArgumentException) {
            ""
        }

    fun sendMessage(message: String) {
        val trimmedMessage = message.trim()
        if (trimmedMessage.isEmpty()) {
            return
        }

        netplaySession.sendMessage(trimmedMessage)
    }

    fun setNetworkMode(mode: NetworkMode) {
        _networkMode.value = mode
        StringSetting.NETPLAY_NETWORK_MODE.setString(NativeConfig.LAYER_BASE, mode.configValue)
        netplaySession.setHostInputAuthority(mode.isHostInputAuthority)
    }

    /**
     * The host typed/stepped a buffer value. Manual always wins: the automatic
     * sizer switches off rather than overwriting this a few seconds later, and
     * the switch in the UI flips with it so the reason is visible.
     */
    fun setBuffer(value: Int) {
        _buffer.value = value
        IntSetting.NETPLAY_BUFFER_SIZE.setInt(NativeConfig.LAYER_BASE, value)
        if (_autoBuffer.value) {
            _autoBuffer.value = false
            BooleanSetting.NETPLAY_AUTO_BUFFER.setBoolean(NativeConfig.LAYER_BASE, false)
        }
        netplaySession.adjustServerPadBufferSize(value)
    }

    fun setAutoBuffer(enabled: Boolean) {
        _autoBuffer.value = enabled
        BooleanSetting.NETPLAY_AUTO_BUFFER.setBoolean(NativeConfig.LAYER_BASE, enabled)
        netplaySession.setAutoPadBuffer(enabled)
    }

    fun setClientBuffer(value: Int) {
        _clientBuffer.value = value
        IntSetting.NETPLAY_CLIENT_BUFFER_SIZE.setInt(NativeConfig.LAYER_BASE, value)
        netplaySession.adjustClientPadBufferSize(value)
    }

    fun changeGame(gameFile: GameFile) {
        StringSetting.NETPLAY_GAME.setString(NativeConfig.LAYER_BASE, gameFile.getGameId())
        netplaySession.changeGame(gameFile)
    }

    private fun getLocalIp(): JoinAddress {
        val localIp = networkHelper.getLocalIpString()
            ?: return JoinAddress.Unknown { _joinAddresses.value += JoinInfoType.LOCAL to getLocalIp() }
        val port = netplaySession.getPort()
        return JoinAddress.Loaded("$localIp:$port")
    }

    private fun fetchExternalIp() {
        _joinAddresses.value += JoinInfoType.EXTERNAL to JoinAddress.Loading
        viewModelScope.launch(Dispatchers.IO) {
            val ip = netplaySession.getExternalIpAddress()
            val port = netplaySession.getPort()
            val address = if (ip != null) JoinAddress.Loaded("$ip:$port")
            else JoinAddress.Unknown { fetchExternalIp() }
            _joinAddresses.value += JoinInfoType.EXTERNAL to address
        }
    }

    private fun collectTraversalState() {
        val retry = { netplaySession.reconnectTraversal() }
        netplaySession.traversalState.onEach { state ->
            when (state) {
                is TraversalState.Connecting -> {
                    _joinAddresses.value += mapOf(
                        JoinInfoType.ROOM_ID to JoinAddress.Loading,
                        JoinInfoType.EXTERNAL to JoinAddress.Loading,
                    )
                }
                is TraversalState.Connected -> {
                    _joinAddresses.value += mapOf(
                        JoinInfoType.ROOM_ID to JoinAddress.Loaded(state.hostCode),
                        JoinInfoType.EXTERNAL to JoinAddress.Loaded(state.externalAddress),
                    )
                }
                is TraversalState.Failure -> {
                    _joinAddresses.value += mapOf(
                        JoinInfoType.ROOM_ID to JoinAddress.Unknown(retry),
                        JoinInfoType.EXTERNAL to JoinAddress.Unknown(retry),
                    )
                }
            }
        }.launchIn(viewModelScope)
    }

    private fun setInitialGame() {
        val game = gameFiles.value
            .find { it.getGameId() == StringSetting.NETPLAY_GAME.string }
            ?: gameFiles.value.firstOrNull()

        if (game != null) {
            changeGame(game)
        }
    }

    @OptIn(DelicateCoroutinesApi::class)
    override fun onCleared() {
        super.onCleared()
        // Closing the netplay session is a bit slow for the main thread so launch in
        // GlobalScope and allow the activity and view model to finish immediately.
        GlobalScope.launch {
            netplaySession.close()
        }
    }

    class Factory(
        private val session: NetplaySession,
        private val networkHelper: NetworkHelper,
    ) : ViewModelProvider.Factory {
        @Suppress("UNCHECKED_CAST")
        override fun <T : ViewModel> create(modelClass: Class<T>): T {
            return NetplayViewModel(session, networkHelper) as T
        }
    }
}
