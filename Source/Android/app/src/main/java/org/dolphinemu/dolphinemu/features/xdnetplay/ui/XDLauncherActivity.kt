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
                    onPickXdFolder = { pickXdFolder.launch(null) },
                    onPickEmeraldRom = { pickEmeraldRom.launch(arrayOf("*/*")) },
                    onTeamEditor = { TeamEditorActivity.launch(this) },
                    onBattle = { NetplaySetupActivity.launch(this) },
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
        controllerMapped = try {
            AutoMapper.isMapped() || AutoMapper.autoMap()
        } catch (_: Exception) {
            false
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

        @JvmStatic
        fun launch(context: Context) {
            context.startActivity(Intent(context, XDLauncherActivity::class.java))
        }
    }
}
