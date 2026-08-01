// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.ui

import android.content.Context
import android.content.Intent
import android.os.Bundle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
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
import org.dolphinemu.dolphinemu.features.xdnetplay.LobbySession
import org.dolphinemu.dolphinemu.features.xdnetplay.NetPlayIndexBridge
import org.dolphinemu.dolphinemu.features.xdnetplay.XdMatchmaker
import java.io.File
import java.security.MessageDigest
import org.dolphinemu.dolphinemu.features.settings.model.BooleanSetting
import org.dolphinemu.dolphinemu.features.settings.model.IntSetting
import org.dolphinemu.dolphinemu.features.settings.model.NativeConfig
import org.dolphinemu.dolphinemu.features.settings.model.StringSetting
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.SaveNaming
import org.dolphinemu.dolphinemu.features.xdnetplay.input.AutoMapper
import org.dolphinemu.dolphinemu.features.settings.ui.MenuTag
import org.dolphinemu.dolphinemu.features.settings.ui.SettingsActivity
import org.dolphinemu.dolphinemu.model.GameFileCache
import org.dolphinemu.dolphinemu.activities.EmulationActivity
import org.dolphinemu.dolphinemu.services.GameFileCacheManager
import org.dolphinemu.dolphinemu.ui.main.MainActivity
import org.dolphinemu.dolphinemu.ui.main.ThemeProvider
import org.dolphinemu.dolphinemu.ui.theme.DolphinTheme
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import org.dolphinemu.dolphinemu.utils.ThemeHelper

class XDLauncherActivity : AppCompatActivity(), ThemeProvider {
    override var themeId: Int = 0

    private var initialized by mutableStateOf(false)
    private var xdGameFound by mutableStateOf(false)
    private var emeraldRomSet by mutableStateOf(false)
    private var teamSavesReady by mutableStateOf(false)
    private var controllerMapped by mutableStateOf(false)
    private var biosLinkReady by mutableStateOf(false)
    private var statusMessage by mutableStateOf<String?>(null)
    private var cheatsEnabled by mutableStateOf(false)

    /** True from the moment "Search for Match" is tapped until it joins, hosts or gives up. */
    private var searching by mutableStateOf(false)

    /**
     * Reads a picked file (extracting the first ROM out of a zip if needed),
     * validates it, and writes it to the app's private GBA folder as a real,
     * memory-mappable file. Returns the absolute path, or null on failure.
     */
    private fun importToPrivateFile(
        uri: android.net.Uri,
        destName: String,
        validate: (ByteArray) -> Boolean
    ): String? {
        return try {
            var bytes = contentResolver.openInputStream(uri)?.use { it.readBytes() } ?: return null
            if (bytes.size >= 4 && bytes[0].toInt() == 0x50 && bytes[1].toInt() == 0x4B) {
                bytes = extractFirstZipEntry(bytes) ?: return null
            }
            if (!validate(bytes)) return null
            val dest = File(DirectoryInitialization.getUserDirectory(), "GBA/$destName")
            dest.parentFile?.mkdirs()
            dest.writeBytes(bytes)
            dest.absolutePath
        } catch (_: Exception) {
            null
        }
    }

    private fun extractFirstZipEntry(zip: ByteArray): ByteArray? {
        java.util.zip.ZipInputStream(zip.inputStream()).use { zis ->
            var entry = zis.nextEntry
            while (entry != null) {
                if (!entry.isDirectory) {
                    val out = zis.readBytes()
                    if (out.isNotEmpty()) return out
                }
                entry = zis.nextEntry
            }
        }
        return null
    }

    /** A GBA ROM carries a fixed 0x96 byte at header offset 0xB2. */
    private fun looksLikeGbaRom(bytes: ByteArray): Boolean =
        bytes.size >= 0xC0 && (bytes[0xB2].toInt() and 0xFF) == 0x96

    private val pickXdFolder =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
            if (uri != null) {
                contentResolver.takePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION
                )
                GameFileCache.addGameFolder(uri.toString())
                GameFileCacheManager.startRescan()
            }
        }

    private val pickGbaBios =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri != null) {
                // Copy to a real file path: mGBA memory-maps these, and content://
                // fds are not reliably mappable.
                val path = importToPrivateFile(uri, "gba_bios.bin") { it.size == 16384 }
                if (path != null) {
                    StringSetting.MAIN_GBA_BIOS_PATH.setString(NativeConfig.LAYER_BASE, path)
                    NativeConfig.save(NativeConfig.LAYER_BASE)
                    statusMessage = null
                } else {
                    statusMessage = "That file isn't a 16 KB GBA BIOS."
                }
                refreshChecks()
            }
        }

    private val pickEmeraldRom =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri != null) {
                // Copy the ROM into private storage as a real, mmappable file.
                // A content:// URI often yields a non-seekable fd that mGBA
                // silently fails to load, so XD never sees the GBA.
                val path = importToPrivateFile(uri, "EMERALD.gba", ::looksLikeGbaRom)
                if (path == null) {
                    statusMessage = "That doesn't look like a GBA ROM. Use a raw .gba (or a .zip containing one)."
                    refreshChecks()
                    return@registerForActivityResult
                }
                statusMessage = null
                // Same ROM in both linkable slots, mirroring the desktop XD Netplay config.
                StringSetting.MAIN_GBA_ROM_2.setString(NativeConfig.LAYER_BASE, path)
                StringSetting.MAIN_GBA_ROM_3.setString(NativeConfig.LAYER_BASE, path)
                NativeConfig.save(NativeConfig.LAYER_BASE)
                refreshChecks()
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        installSplashScreen().setKeepOnScreenCondition {
            !DirectoryInitialization.areDolphinDirectoriesReady()
        }
        ThemeHelper.setTheme(this)
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)

        DirectoryInitialization.start(this)
        AfterDirectoryInitializationRunner().runWithLifecycle(this) {
            initialized = true
            refreshChecks()
            GameFileCacheManager.startLoad()
        }

        GameFileCacheManager.getGameFiles().observe(this) {
            xdGameFound = GameFileCacheManager.getGameFileByGameId(XD_GAME_ID) != null
        }

        setContent {
            DolphinTheme {
                XDLauncherScreen(
                    initialized = initialized,
                    xdGameFound = xdGameFound,
                    emeraldRomSet = emeraldRomSet,
                    teamSavesReady = teamSavesReady,
                    controllerMapped = controllerMapped,
                    biosLinkReady = biosLinkReady,
                    statusMessage = statusMessage,
                    onPickXdFolder = { pickXdFolder.launch(null) },
                    onPickGbaBios = { pickGbaBios.launch(arrayOf("*/*")) },
                    onPickEmeraldRom = { pickEmeraldRom.launch(arrayOf("*/*")) },
                    onTeamEditor = { TeamEditorActivity.launch(this) },
                    onPlayXd = { bootXd() },
                    cheatsEnabled = cheatsEnabled,
                    onCheatsChanged = { on ->
                        cheatsEnabled = on
                        BooleanSetting.MAIN_ENABLE_CHEATS.setBoolean(NativeConfig.LAYER_BASE, on)
                        NativeConfig.save(NativeConfig.LAYER_BASE)
                    },
                    onBattle = { NetplaySetupActivity.launch(this) },
                    onSearchForMatch = { searchForMatch() },
                    searching = searching,
                    onFindBattles = { FindBattlesActivity.launch(this) },
                    onOpenSettings = {
                        SettingsActivity.launch(this, MenuTag.SETTINGS)
                    },
                    onOpenDolphin = {
                        startActivity(Intent(this, MainActivity::class.java))
                    },
                    onShareDetectLog = { shareDetectLog() }
                )
            }
        }
    }

    /** Share the GBA-detection diagnostic logs (GBA/gba_detect_*.log in the
     * app's external files dir, which the stock Files app hides on Android
     * 11+) via the FileProvider. One file per session, newest three retained —
     * all are shared together so a failed-attempt/retry pair arrives intact,
     * and the timestamped names say which session is which. */
    private fun shareDetectLog() {
        val dir = java.io.File(getExternalFilesDir(null), "GBA")
        val logs = (dir.listFiles { f ->
            f.isFile && f.name.startsWith("gba_detect") && f.name.endsWith(".log")
        } ?: emptyArray()).sortedByDescending { it.name }.take(3)
        if (logs.isEmpty()) {
            android.widget.Toast.makeText(
                this, "No detect log yet — boot XD once, then share.",
                android.widget.Toast.LENGTH_LONG).show()
            return
        }
        val uris = ArrayList<android.os.Parcelable>(logs.map {
            androidx.core.content.FileProvider.getUriForFile(this, "$packageName.xdshare", it)
        })
        val send = android.content.Intent(
            android.content.Intent.ACTION_SEND_MULTIPLE).apply {
            type = "text/plain"
            putParcelableArrayListExtra(android.content.Intent.EXTRA_STREAM, uris)
            addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivity(android.content.Intent.createChooser(send, "Share detect logs"))
    }

    /** Boot Pokemon XD directly by game ID, bypassing the game picker (which
     * would otherwise default to whatever ISO sorts first in the folder). */
    private fun bootXd() {
        val xd = GameFileCacheManager.getGameFileByGameId(XD_GAME_ID)
        if (xd != null) {
            // XD arms its GBA detection from the player's own menu navigation;
            // a parked connect screen means the roll missed, and re-entering is
            // the retry. Shown only here on the launcher -- in-game text can't
            // tell a parked entry from innocent menu idling, so it was removed.
            statusMessage =
                "Tip: a GBA links within seconds of entering the connect screen. " +
                "If ~20s pass with nothing, press B and re-enter - each entry is a fresh try."
            EmulationActivity.launch(this, xd.getPath(), false)
        } else {
            statusMessage = "Pokémon XD not found — choose the folder with your ISO first."
        }
    }

    /**
     * One-button matchmaking: look for someone already waiting and join them;
     * if nobody is, host a public room and wait to be found. The desktop twin
     * is XDLauncherDialog::OnSearchForMatch — same selection rule
     * (XdMatchmaker.pickMatch), same published room name.
     *
     * The index call is blocking HTTP, so it runs on Dispatchers.IO; every
     * status update lands back on the main thread through the normal Compose
     * state write.
     */
    private fun searchForMatch() {
        if (searching) return

        val xd = GameFileCacheManager.getGameFileByGameId(XD_GAME_ID)
        if (xd == null) {
            statusMessage = "Pokémon XD not found — choose the folder with your ISO first."
            return
        }
        if (!biosLinkReady) {
            statusMessage =
                "The official GBA BIOS is required — XD cannot detect the GBA without it."
            return
        }

        searching = true
        statusMessage = "Searching for an open battle…"

        lifecycleScope.launch {
            val sessions = withContext(Dispatchers.IO) {
                try {
                    NetPlayIndexBridge.listSessions()
                } catch (_: Throwable) {
                    null
                }
            }

            // "Could not reach the index" is not "nobody is playing". Hosting
            // on a network error would strand the user in an unlistable room.
            if (sessions == null) {
                searching = false
                statusMessage =
                    "Could not reach the session list. Check your connection, then try again."
                return@launch
            }

            val match = XdMatchmaker.pickMatch(sessions)
            if (match != null) {
                joinMatch(match)
            } else {
                statusMessage = "No open battles found — hosting one. " +
                    "Your room is listed publicly; wait here for an opponent."
                XdMatchmaker.applyHostConfig()
                searching = false
                NetplaySetupActivity.launch(this@XDLauncherActivity)
            }
        }
    }

    /**
     * Joins an auto-matched room. Mirrors FindBattlesActivity.join() minus the
     * password path — XdMatchmaker.pickMatch() never returns a password room,
     * so there is no encrypted server id to unwrap here.
     */
    private suspend fun joinMatch(session: LobbySession) {
        statusMessage = "Found \"${session.name}\" — joining…"
        XdMatchmaker.applyJoinConfig(session)
        NativeConfig.save(NativeConfig.LAYER_BASE)

        var errorForwarding: Job? = null
        try {
            // NetplaySession's UI callbacks need the game list; wait like
            // NetplaySetupViewModel.connect() does.
            GameFileCacheManager.isLoading().asFlow().first { it == false }

            val netplaySession = NetplayManager.createSession()
            errorForwarding = netplaySession.connectionErrors
                .onEach { statusMessage = it }
                .launchIn(lifecycleScope)

            if (netplaySession.join()) {
                statusMessage = null
                NetplayActivity.launch(this)
            } else {
                statusMessage = "Could not connect to \"${session.name}\". " +
                    "It may have just closed — tap Search for Match again."
            }
        } finally {
            errorForwarding?.cancel()
            searching = false
        }
    }

    override fun onResume() {
        super.onResume()
        if (DirectoryInitialization.areDolphinDirectoriesReady()) {
            initialized = true
            refreshChecks()
        }
    }

    /**
     * mGBA memory-maps the ROM and BIOS, which content:// fds don't support,
     * and File::Exists() returns false for content:// so GBACore falls back to
     * the joybus-less bundled BIOS. Migrate any stored content:// path to a
     * real file so both actually load — no re-pick needed.
     */
    private fun migrateContentPaths() {
        migrateOne(StringSetting.MAIN_GBA_BIOS_PATH, "gba_bios.bin")
        migrateOne(StringSetting.MAIN_GBA_ROM_2, "EMERALD.gba")
        migrateOne(StringSetting.MAIN_GBA_ROM_3, "EMERALD.gba")
    }

    private fun migrateOne(setting: StringSetting, destName: String) {
        val cur = setting.string
        if (!cur.startsWith("content://")) return
        try {
            val bytes = contentResolver.openInputStream(android.net.Uri.parse(cur))
                ?.use { it.readBytes() } ?: return
            val dest = File(DirectoryInitialization.getUserDirectory(), "GBA/$destName")
            dest.parentFile?.mkdirs()
            dest.writeBytes(bytes)
            setting.setString(NativeConfig.LAYER_BASE, dest.absolutePath)
            NativeConfig.save(NativeConfig.LAYER_BASE)
        } catch (_: Exception) {
            // Permission may have lapsed; the checklist picker re-copies correctly.
        }
    }

    private fun refreshChecks() {
        cheatsEnabled = BooleanSetting.MAIN_ENABLE_CHEATS.boolean
        migrateContentPaths()
        emeraldRomSet = ensureGbaConfig()
        xdGameFound = GameFileCacheManager.getGameFileByGameId(XD_GAME_ID) != null
        teamSavesReady = if (emeraldRomSet) seedTeamSaves() else false
        controllerMapped = try {
            AutoMapper.isMapped() || AutoMapper.autoMap()
        } catch (_: Exception) {
            false
        }
        biosLinkReady = checkOfficialBios()
    }

    /**
     * XD/Colosseum GBA link only works with the official GBA BIOS boot
     * handshake; the open-source BIOS bundled for Game Boy Player use has no
     * joybus code. Verify whichever BIOS Dolphin would actually load.
     */
    private fun checkOfficialBios(): Boolean {
        return try {
            val configured = StringSetting.MAIN_GBA_BIOS_PATH.string
            val bytes: ByteArray? = when {
                configured.startsWith("content://") ->
                    contentResolver.openInputStream(android.net.Uri.parse(configured))
                        ?.use { it.readBytes() }
                configured.isNotEmpty() -> File(configured).takeIf { it.exists() }?.readBytes()
                else -> {
                    val userBios = File(DirectoryInitialization.getUserDirectory(), "GBA/gba_bios.bin")
                    if (userBios.exists()) userBios.readBytes() else null
                }
            }
            if (bytes == null || bytes.size != 16384) return false
            val sha1 = MessageDigest.getInstance("SHA-1").digest(bytes)
                .joinToString("") { "%02x".format(it) }
            sha1 == OFFICIAL_GBA_BIOS_SHA1
        } catch (_: Exception) {
            false
        }
    }

    /**
     * Force the XD GBA-link config on every launch rather than trusting
     * Dolphin's settings to persist (they were observed reverting to blank
     * across restarts): point both linkable GBA slots at the copied Emerald
     * ROM and set ports 2/3 to Emulated GBA (SIDevice 13). Runs before the
     * user can boot XD, so the cores are configured at boot — never a fragile
     * mid-game change. Returns true when a valid Emerald ROM is configured.
     */
    private fun ensureGbaConfig(): Boolean {
        val copied = File(DirectoryInitialization.getUserDirectory(), "GBA/EMERALD.gba")
        val romPath = when {
            copied.exists() -> copied.absolutePath
            else -> {
                val existing = StringSetting.MAIN_GBA_ROM_2.string
                    .ifEmpty { StringSetting.MAIN_GBA_ROM_3.string }
                if (existing.isNotEmpty() && !existing.startsWith("content://") &&
                    File(existing).exists()) existing else null
            }
        } ?: return false

        var changed = false
        if (StringSetting.MAIN_GBA_ROM_2.string != romPath) {
            StringSetting.MAIN_GBA_ROM_2.setString(NativeConfig.LAYER_BASE, romPath); changed = true
        }
        if (StringSetting.MAIN_GBA_ROM_3.string != romPath) {
            StringSetting.MAIN_GBA_ROM_3.setString(NativeConfig.LAYER_BASE, romPath); changed = true
        }
        // XD's GBA-vs-GBA mode is a two-player battle, so it wants a GBA on
        // BOTH socket 2 (SIDevice1) and socket 3 (SIDevice2) — XD is the peer
        // for both. Enable both; each links in turn (the not-yet-probed one
        // just cycles its boot screen until XD asks for it). Solo = control
        // both locally by switching the visible GBA screen.
        if (IntSetting.MAIN_SI_DEVICE_1.int != 13) {
            IntSetting.MAIN_SI_DEVICE_1.setInt(NativeConfig.LAYER_BASE, 13); changed = true
        }
        if (IntSetting.MAIN_SI_DEVICE_2.int != 13) {
            IntSetting.MAIN_SI_DEVICE_2.setInt(NativeConfig.LAYER_BASE, 13); changed = true
        }
        // The 2-year-stable papajefe XDNetplay bundle runs with cheats ON so the
        // $XD OU Fixes AR code (enabled in the bundled GXXE01.ini) actually
        // Cheats are off by default and controlled by the "$XD OU Fixes" switch
        // on the launcher (host's choice syncs to the guest in netplay). Don't
        // force them on here.
        // PBR mode turns the touch overlay on for the Wii Remote pointer. XD is
        // played with the handheld's physical buttons, so put it back.
        if (BooleanSetting.MAIN_SHOW_INPUT_OVERLAY.boolean) {
            BooleanSetting.MAIN_SHOW_INPUT_OVERLAY.setBoolean(NativeConfig.LAYER_BASE, false)
            changed = true
        }
        // When hosting, netplay defaults its game to the FIRST entry in the
        // GameCube library (setInitialGame -> gameFiles.firstOrNull()), which is
        // whatever sorts first (e.g. Call of Duty), not XD. Pin the netplay game
        // to Pokemon XD so the host lands on it automatically; the lobby's game
        // row can still change it.
        if (StringSetting.NETPLAY_GAME.string != XD_GAME_ID) {
            StringSetting.NETPLAY_GAME.setString(NativeConfig.LAYER_BASE, XD_GAME_ID); changed = true
        }
        if (changed) {
            NativeConfig.save(NativeConfig.LAYER_BASE)
        }
        return true
    }

    /**
     * Places the bundled dummy team saves where Dolphin expects the netplay
     * GBA saves, so hosting works out of the box. Never overwrites.
     */
    private fun seedTeamSaves(): Boolean {
        val rom = StringSetting.MAIN_GBA_ROM_2.string
            .ifEmpty { StringSetting.MAIN_GBA_ROM_3.string }
        if (rom.isEmpty()) return false
        return try {
            val savesDir = File(
                DirectoryInitialization.getUserDirectory(),
                "GBA" + File.separator + "Saves"
            )
            savesDir.mkdirs()
            var allPresent = true
            for (role in TeamRole.entries) {
                val target = File(SaveNaming.deriveSavePath(savesDir.path, rom, role.deviceNumber))
                if (!target.exists()) {
                    assets.open(role.templateAsset).use { input ->
                        target.outputStream().use { input.copyTo(it) }
                    }
                }
                allPresent = allPresent && target.exists()
            }
            allPresent
        } catch (_: Exception) {
            // Non-fatal: the team editor can still create saves on demand.
            false
        }
    }

    companion object {
        const val XD_GAME_ID = "GXXE01"
        const val OFFICIAL_GBA_BIOS_SHA1 = "300c20df6731a33952ded8c436f7f186d25d3492"

        @JvmStatic
        fun launch(context: Context) {
            context.startActivity(Intent(context, XDLauncherActivity::class.java))
        }
    }
}
