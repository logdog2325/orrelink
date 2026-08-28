// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.ui

import android.content.Context
import android.content.Intent
import android.os.Bundle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.asFlow
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.dolphinemu.dolphinemu.features.netplay.NetplayManager
import org.dolphinemu.dolphinemu.features.netplay.ui.NetplayActivity
import org.dolphinemu.dolphinemu.features.netplay.ui.NetplaySetupActivity
import org.dolphinemu.dolphinemu.features.settings.model.NativeConfig
import org.dolphinemu.dolphinemu.features.settings.model.StringSetting
import org.dolphinemu.dolphinemu.features.xdnetplay.LobbySession
import org.dolphinemu.dolphinemu.features.xdnetplay.NetPlayIndexBridge
import org.dolphinemu.dolphinemu.features.xdnetplay.FormatBridge
import org.dolphinemu.dolphinemu.features.xdnetplay.XdMatchmaker
import org.dolphinemu.dolphinemu.services.GameFileCacheManager
import org.dolphinemu.dolphinemu.ui.main.ThemeProvider
import org.dolphinemu.dolphinemu.ui.theme.DolphinTheme
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import org.dolphinemu.dolphinemu.utils.ThemeHelper

/**
 * Public lobby browser — the Android counterpart of desktop Dolphin's NetPlay
 * Session Browser, styled for the XD Netplay fork.
 *
 * Joining mirrors desktop NetPlayBrowser::accept(): the tapped session's method,
 * port and server id (traversal code or IP) are written to the NetPlay config
 * keys that NetplaySession.nativeJoin() reads, then the join is performed
 * directly through NetplayManager (same code path NetplaySetupViewModel uses)
 * and NetplayActivity is launched on success.
 *
 * Publishing needs no new native code: Core's NetPlayServer::SetupIndex()
 * publishes automatically while hosting whenever NetPlay/UseIndex,
 * NetPlay/IndexName and NetPlay/IndexRegion are set, keeps the entry fresh from
 * the server ping loop, and unlists it when hosting stops. The publish panel
 * here just writes those keys, then hands off to the normal host flow.
 */
class FindBattlesActivity : AppCompatActivity(), ThemeProvider {
    override var themeId: Int = 0

    private var initialized by mutableStateOf(false)
    private var uiState by mutableStateOf(FindBattlesState())

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeHelper.setTheme(this)
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)

        DirectoryInitialization.start(this)
        AfterDirectoryInitializationRunner().runWithLifecycle(this) {
            initialized = true
            GameFileCacheManager.startLoad()
            loadPublishPrefs()
            refresh()
        }

        setContent {
            DolphinTheme {
                FindBattlesScreen(
                    initialized = initialized,
                    state = uiState,
                    onBack = { finish() },
                    onRefresh = ::refresh,
                    onToggleXdOnly = { uiState = uiState.copy(xdOnly = it) },
                    onJoin = { join(it, password = null) },
                    onPasswordSubmit = { session, password -> join(session, password) },
                    onPasswordDismiss = { uiState = uiState.copy(passwordPrompt = null) },
                    onPublishEnabledChange = ::setPublishEnabled,
                    onPublishNameChange = ::setPublishName,
                    onPublishRegionChange = ::setPublishRegion,
                    onPublishPasswordChange = ::setPublishPassword,
                    onHostPublic = ::hostPublicBattle
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

    /** Fetches the session list off the main thread; errors land in state, never throw. */
    private fun refresh() {
        if (!initialized || uiState.loading) return
        uiState = uiState.copy(loading = true, error = null)

        lifecycleScope.launch {
            val (sessions, error) = withContext(Dispatchers.IO) {
                try {
                    val result = NetPlayIndexBridge.listSessions()
                    if (result == null) {
                        null to friendlyListError(NetPlayIndexBridge.lastError)
                    } else {
                        result to null
                    }
                } catch (e: Throwable) {
                    null to "Could not read the lobby: ${e.message ?: "unknown error"}"
                }
            }
            uiState = uiState.copy(
                loading = false,
                sessions = sessions ?: emptyList(),
                error = error
            )
        }
    }

    private fun friendlyListError(code: String): String = when (code) {
        "NO_RESPONSE" ->
            "Could not reach the lobby server. Check your internet connection and try again."
        "BAD_JSON" ->
            "The lobby server sent an unexpected response. Try again in a moment."
        else ->
            "The lobby server reported an error: $code"
    }

    /**
     * Tap-to-join. For password sessions the lobby stores an encrypted server id;
     * decrypting it with the entered password (checksum-verified) is the whole
     * "authentication", after which the join is a normal traversal/direct join.
     */
    private fun join(session: LobbySession, password: String?) {
        if (uiState.joining) return

        var serverId = session.serverId
        if (session.hasPassword) {
            if (password.isNullOrEmpty()) {
                uiState = uiState.copy(passwordPrompt = session)
                return
            }
            val decrypted = NetPlayIndexBridge.decryptServerId(session.serverId, password)
            if (decrypted == null) {
                uiState = uiState.copy(
                    passwordPrompt = null,
                    status = "Wrong password for \"${session.name}\"."
                )
                return
            }
            serverId = decrypted
        }

        // Same config handoff as desktop NetPlayBrowser::accept();
        // NetplaySession.nativeJoin() reads exactly these keys.
        XdMatchmaker.applyJoinConfig(session, serverId)

        uiState = uiState.copy(
            joining = true,
            passwordPrompt = null,
            status = "Joining \"${session.name}\"..."
        )

        lifecycleScope.launch {
            var errorForwarding: Job? = null
            try {
                // NetplaySession's UI callbacks need the game list; wait like
                // NetplaySetupViewModel.connect() does.
                GameFileCacheManager.isLoading().asFlow().first { it == false }

                val netplaySession = NetplayManager.createSession()
                errorForwarding = netplaySession.connectionErrors
                    .onEach { uiState = uiState.copy(status = it) }
                    .launchIn(this)

                if (netplaySession.join()) {
                    uiState = uiState.copy(status = null)
                    NetplayActivity.launch(this@FindBattlesActivity)
                } else {
                    uiState = uiState.copy(
                        status = "Could not connect to \"${session.name}\". " +
                            "It may have just closed - refresh and try again."
                    )
                }
            } finally {
                errorForwarding?.cancel()
                uiState = uiState.copy(joining = false)
            }
        }
    }

    // ---- Publishing (host side) ----

    private fun loadPublishPrefs() {
        // Same "XD [OC] <nickname>" shape the auto-matcher publishes, so a
        // hand-published room reads identically in the lobby. sessionName()
        // reads the Format pick, so the prefill carries the "[Orre] " tag
        // exactly when a room hosted right now would.
        val nickname = StringSetting.NETPLAY_NICKNAME.string
        val defaultName = XdMatchmaker.sessionName(nickname)
        // A stored name matching ANY auto-shape is an auto-name from an
        // earlier publish, not something the user typed — recompute it, so a
        // format tag stuck in config from a previous pick can never mislabel
        // a room under a different pick. Hand-typed names pass through
        // untouched.
        val autoNames = XdMatchmaker.autoNames(nickname)
        uiState = uiState.copy(
            publishEnabled = NativeConfig.getBoolean(
                NativeConfig.LAYER_ACTIVE, FILE, SECTION, KEY_USE_INDEX, false
            ),
            publishName = NativeConfig.getString(
                NativeConfig.LAYER_ACTIVE, FILE, SECTION, KEY_INDEX_NAME, ""
            ).let { if (it.isEmpty() || it in autoNames) defaultName else it },
            publishRegion = NativeConfig.getString(
                NativeConfig.LAYER_ACTIVE, FILE, SECTION, KEY_INDEX_REGION, ""
            ).ifEmpty { "NA" },
            publishPassword = NativeConfig.getString(
                NativeConfig.LAYER_ACTIVE, FILE, SECTION, KEY_INDEX_PASSWORD, ""
            ),
            regions = NetPlayIndexBridge.regions(),
            buildVersion = NetPlayIndexBridge.localVersion
        )
    }

    private fun setPublishEnabled(enabled: Boolean) {
        uiState = uiState.copy(publishEnabled = enabled)
        NativeConfig.setBoolean(NativeConfig.LAYER_BASE, FILE, SECTION, KEY_USE_INDEX, enabled)
        NativeConfig.save(NativeConfig.LAYER_BASE)
    }

    private fun setPublishName(name: String) {
        uiState = uiState.copy(publishName = name)
        NativeConfig.setString(NativeConfig.LAYER_BASE, FILE, SECTION, KEY_INDEX_NAME, name)
    }

    private fun setPublishRegion(region: String) {
        uiState = uiState.copy(publishRegion = region)
        NativeConfig.setString(NativeConfig.LAYER_BASE, FILE, SECTION, KEY_INDEX_REGION, region)
    }

    private fun setPublishPassword(password: String) {
        uiState = uiState.copy(publishPassword = password)
        NativeConfig.setString(NativeConfig.LAYER_BASE, FILE, SECTION, KEY_INDEX_PASSWORD, password)
    }

    /**
     * Ensures the index config is set, then hands off to the standard host flow.
     * When the user taps Host there, NetPlayServer::SetupIndex() publishes the
     * session (immediately for direct, on traversal-code assignment for traversal)
     * and removes it when the room closes.
     */
    private fun hostPublicBattle() {
        val state = uiState
        XdMatchmaker.applyHostConfig(
            name = state.publishName,
            region = state.publishRegion,
            password = state.publishPassword
        )

        uiState = state.copy(publishEnabled = true)
        NetplaySetupActivity.launch(this)
    }

    companion object {
        // Aliases of the single definition in XdMatchmaker, so the publish
        // panel below and the auto-matcher can never drift onto different keys.
        private const val FILE = XdMatchmaker.FILE
        private const val SECTION = XdMatchmaker.SECTION
        private const val KEY_USE_INDEX = XdMatchmaker.KEY_USE_INDEX
        private const val KEY_INDEX_NAME = XdMatchmaker.KEY_INDEX_NAME
        private const val KEY_INDEX_REGION = XdMatchmaker.KEY_INDEX_REGION
        private const val KEY_INDEX_PASSWORD = XdMatchmaker.KEY_INDEX_PASSWORD

        @JvmStatic
        fun launch(context: Context) {
            context.startActivity(Intent(context, FindBattlesActivity::class.java))
        }
    }
}

data class FindBattlesState(
    val loading: Boolean = false,
    val joining: Boolean = false,
    /** Friendly list-area error (network/server); null when the last refresh worked. */
    val error: String? = null,
    /** One-line status under the list (join progress, wrong password, ...). */
    val status: String? = null,
    val sessions: List<LobbySession> = emptyList(),
    /** Default on: only Pokemon XD (GXXE01) battles. Off: every session on the index. */
    val xdOnly: Boolean = true,
    /** Session awaiting a password entry, or null. */
    val passwordPrompt: LobbySession? = null,
    val publishEnabled: Boolean = false,
    val publishName: String = "",
    val publishRegion: String = "NA",
    val publishPassword: String = "",
    /** (code, label) pairs from NetPlayIndex::GetRegions(). */
    val regions: List<Pair<String, String>> = emptyList(),
    /** This build's version string; only same-version sessions are listed. */
    val buildVersion: String = ""
) {
    val visibleSessions: List<LobbySession>
        get() = if (xdOnly) sessions.filter { it.isXdBattle } else sessions
}
