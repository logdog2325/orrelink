// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.ui

import android.content.Context
import android.content.Intent
import android.os.Bundle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AppCompatActivity
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.dolphinemu.dolphinemu.features.netplay.ui.NetplaySetupActivity
import org.dolphinemu.dolphinemu.features.settings.model.StringSetting
import org.dolphinemu.dolphinemu.features.settings.ui.MenuTag
import org.dolphinemu.dolphinemu.features.settings.ui.SettingsActivity
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
    }

    companion object {
        const val XD_GAME_ID = "GXXE01"

        @JvmStatic
        fun launch(context: Context) {
            context.startActivity(Intent(context, XDLauncherActivity::class.java))
        }
    }
}
