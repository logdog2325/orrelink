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
import org.dolphinemu.dolphinemu.features.netplay.ui.NetplaySetupActivity
import java.io.File
import java.security.MessageDigest
import org.dolphinemu.dolphinemu.features.settings.model.IntSetting
import org.dolphinemu.dolphinemu.features.settings.model.NativeConfig
import org.dolphinemu.dolphinemu.features.settings.model.StringSetting
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.SaveNaming
import org.dolphinemu.dolphinemu.features.xdnetplay.input.AutoMapper
import org.dolphinemu.dolphinemu.features.settings.ui.MenuTag
import org.dolphinemu.dolphinemu.features.settings.ui.SettingsActivity
import org.dolphinemu.dolphinemu.model.GameFileCache
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
                contentResolver.takePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION
                )
                StringSetting.MAIN_GBA_BIOS_PATH.setString(NativeConfig.LAYER_BASE, uri.toString())
                NativeConfig.save(NativeConfig.LAYER_BASE)
                refreshChecks()
            }
        }

    private val pickEmeraldRom =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri != null) {
                contentResolver.takePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION
                )
                // Same ROM in both linkable slots, mirroring the desktop XD Netplay config.
                StringSetting.MAIN_GBA_ROM_2.setString(NativeConfig.LAYER_BASE, uri.toString())
                StringSetting.MAIN_GBA_ROM_3.setString(NativeConfig.LAYER_BASE, uri.toString())
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
                    onPickXdFolder = { pickXdFolder.launch(null) },
                    onPickGbaBios = { pickGbaBios.launch(arrayOf("*/*")) },
                    onPickEmeraldRom = { pickEmeraldRom.launch(arrayOf("*/*")) },
                    onTeamEditor = { TeamEditorActivity.launch(this) },
                    onBattle = { NetplaySetupActivity.launch(this) },
                    onFindBattles = { FindBattlesActivity.launch(this) },
                    onOpenSettings = {
                        SettingsActivity.launch(this, MenuTag.SETTINGS)
                    },
                    onOpenDolphin = {
                        startActivity(Intent(this, MainActivity::class.java))
                    }
                )
            }
        }
    }

    override fun onResume() {
        super.onResume()
        if (DirectoryInitialization.areDolphinDirectoriesReady()) {
            initialized = true
            refreshChecks()
        }
    }

    private fun refreshChecks() {
        emeraldRomSet = StringSetting.MAIN_GBA_ROM_2.string.isNotEmpty() ||
            StringSetting.MAIN_GBA_ROM_3.string.isNotEmpty()
        xdGameFound = GameFileCacheManager.getGameFileByGameId(XD_GAME_ID) != null
        teamSavesReady = if (emeraldRomSet) seedTeamSaves() else false
        if (emeraldRomSet) {
            configureGbaPorts()
        }
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
     * XD's GBA-vs-GBA mode needs controller ports 2 and 3 to be integrated
     * GBAs. Only upgrades ports that are still Disabled so deliberate
     * configurations survive.
     */
    private fun configureGbaPorts() {
        var changed = false
        for (setting in listOf(IntSetting.MAIN_SI_DEVICE_1, IntSetting.MAIN_SI_DEVICE_2)) {
            if (setting.int == 0) {
                setting.setInt(NativeConfig.LAYER_BASE, 13)
                changed = true
            }
        }
        if (changed) {
            NativeConfig.save(NativeConfig.LAYER_BASE)
        }
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
