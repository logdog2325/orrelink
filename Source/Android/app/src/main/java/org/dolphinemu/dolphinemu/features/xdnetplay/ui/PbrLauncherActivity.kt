// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.io.File
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.activities.EmulationActivity
import org.dolphinemu.dolphinemu.features.settings.model.BooleanSetting
import org.dolphinemu.dolphinemu.features.settings.model.IntSetting
import org.dolphinemu.dolphinemu.features.settings.model.NativeConfig
import org.dolphinemu.dolphinemu.model.GameFileCache
import org.dolphinemu.dolphinemu.services.GameFileCacheManager
import org.dolphinemu.dolphinemu.ui.main.ThemeProvider
import org.dolphinemu.dolphinemu.ui.theme.DolphinTheme
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import org.dolphinemu.dolphinemu.utils.ThemeHelper
import org.dolphinemu.dolphinemu.utils.ThreadUtil
import org.dolphinemu.dolphinemu.utils.WiiUtils

/**
 * Pokémon Battle Revolution online mode (UPA private server). Fully separate
 * from the XD launcher: it drives a different game (RPBE01) and its own config
 * only. The XD launcher force-applies its own setup every time it opens, so
 * nothing here can degrade XD -- see ensurePbrConfig().
 */
class PbrLauncherActivity : AppCompatActivity(), ThemeProvider {
    override var themeId: Int = 0

    private var initialized by mutableStateOf(false)
    private var nandImported by mutableStateOf(false)
    private var pbrFound by mutableStateOf(false)
    private var statusMessage by mutableStateOf<String?>(null)

    private val pickPbrFolder =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
            if (uri != null) {
                contentResolver.takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION
                )
                GameFileCache.addGameFolder(uri.toString())
                GameFileCacheManager.startRescan()
            }
        }

    private val pickNand =
        registerForActivityResult(ActivityResultContracts.GetContent()) { uri ->
            if (uri != null) importNand(uri)
        }

    /** Imports the UPA-provided nand.bin (NAND + appended keys) into the app's
     *  Wii/ dir on a background thread. Success is silent; the native side pops
     *  a panic on failure. */
    private fun importNand(uri: Uri) {
        ThreadUtil.runOnThreadAndShowResult(
            this, R.string.import_in_progress, R.string.do_not_close_app,
            {
                WiiUtils.importNANDBin(uri.toString())
                runOnUiThread { refreshChecks() }
                null
            }
        )
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
            pbrFound = GameFileCacheManager.getGameFileByGameId(PBR_GAME_ID) != null
        }

        setContent {
            DolphinTheme {
                PbrLauncherScreen(
                    initialized = initialized,
                    nandImported = nandImported,
                    pbrFound = pbrFound,
                    statusMessage = statusMessage,
                    onImportNand = { pickNand.launch("*/*") },
                    onPickPbrFolder = { pickPbrFolder.launch(null) },
                    onPlayPbr = { bootPbr() },
                    onBack = { finish() }
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
        ensurePbrConfig()
        nandImported = checkNandImported()
        pbrFound = GameFileCacheManager.getGameFileByGameId(PBR_GAME_ID) != null
    }

    private fun checkNandImported(): Boolean = try {
        File(DirectoryInitialization.getUserDirectory(), "Wii/title").listFiles()?.isNotEmpty() == true
    } catch (_: Exception) {
        false
    }

    /**
     * Config for PBR mode. Deliberately isolated from XD:
     *  - Turn the emulated-GBA SI ports OFF (XD sets them to 13 for its GBA-vs-
     *    GBA link; PBR is a Wii game and must not spin up Emerald cores). The XD
     *    launcher flips them back to 13 whenever it opens, so XD is unaffected.
     *  - Cheats on so RPBE01.ini's on-frame black-screen fix applies (same flag
     *    XD already forces, so no conflict). RPBE01.ini also forces the EFB
     *    graphics fix so battle-pass avatars render.
     * The UPA route needs no SSL certs, no DNS change, and no activation wait --
     * the patched ISO reaches the server directly.
     */
    private fun ensurePbrConfig() {
        var changed = false
        if (IntSetting.MAIN_SI_DEVICE_1.int != 0) {
            IntSetting.MAIN_SI_DEVICE_1.setInt(NativeConfig.LAYER_BASE, 0); changed = true
        }
        if (IntSetting.MAIN_SI_DEVICE_2.int != 0) {
            IntSetting.MAIN_SI_DEVICE_2.setInt(NativeConfig.LAYER_BASE, 0); changed = true
        }
        if (!BooleanSetting.MAIN_ENABLE_CHEATS.boolean) {
            BooleanSetting.MAIN_ENABLE_CHEATS.setBoolean(NativeConfig.LAYER_BASE, true); changed = true
        }
        if (changed) {
            NativeConfig.save(NativeConfig.LAYER_BASE)
        }
    }

    private fun bootPbr() {
        val pbr = GameFileCacheManager.getGameFileByGameId(PBR_GAME_ID)
        if (pbr != null) {
            EmulationActivity.launch(this, pbr.getPath(), false)
        } else {
            statusMessage =
                "Patched Pokémon Battle Revolution (RPBE01) not found — choose the folder with your UPA ISO."
        }
    }

    companion object {
        const val PBR_GAME_ID = "RPBE01"

        @JvmStatic
        fun launch(context: Context) {
            context.startActivity(Intent(context, PbrLauncherActivity::class.java))
        }
    }
}
