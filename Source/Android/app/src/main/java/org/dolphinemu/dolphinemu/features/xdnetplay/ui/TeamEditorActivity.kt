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
import androidx.core.content.FileProvider
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.dolphinemu.dolphinemu.features.settings.model.StringSetting
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.EmeraldSave
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.Gen3Data
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.Gen3Mon
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.MonFactory
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.SaveNaming
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.ShowdownParser
import org.dolphinemu.dolphinemu.ui.main.ThemeProvider
import org.dolphinemu.dolphinemu.ui.theme.DolphinTheme
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import org.dolphinemu.dolphinemu.utils.ThemeHelper
import org.json.JSONObject
import java.io.File
import java.net.URL

class TeamEditorActivity : AppCompatActivity(), ThemeProvider {
    override var themeId: Int = 0

    private lateinit var repo: TeamRepo
    private var uiState by mutableStateOf(TeamEditorState())

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeHelper.setTheme(this)
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)

        repo = TeamRepo(this)
        reload(TeamRole.HOST)

        setContent {
            DolphinTheme {
                TeamEditorScreen(
                    state = uiState,
                    names = if (uiState.ready) repo.displayNames() else null,
                    onRoleChange = { reload(it) },
                    onSelect = { uiState = uiState.copy(selected = it) },
                    onImport = { importShowdown(it) },
                    onRemoveSelected = { removeSelected() },
                    onSave = { saveTeam() },
                    onShare = { shareSave() },
                    onBack = { finish() }
                )
            }
        }
    }

    private fun reload(role: TeamRole) {
        uiState = try {
            val loaded = repo.load(role)
            TeamEditorState(
                role = role,
                ready = true,
                trainer = loaded.trainerLabel,
                party = loaded.party,
                messages = loaded.messages
            )
        } catch (e: Exception) {
            TeamEditorState(
                role = role,
                ready = false,
                messages = listOf("Could not open a save: ${e.message ?: "unknown error"}")
            )
        }
    }

    private fun importShowdown(text: String) {
        val pokepaste = Regex("^https?://pokepast\\.es/[A-Za-z0-9]+")
            .find(text.trim())?.value
        if (pokepaste == null) {
            applyImport(repo.importShowdown(text))
            return
        }
        uiState = uiState.copy(messages = listOf("Fetching $pokepaste…"))
        lifecycleScope.launch(Dispatchers.IO) {
            val result = try {
                repo.importShowdown(URL("$pokepaste/raw").readText())
            } catch (e: Exception) {
                listOf("Could not fetch paste: ${e.message ?: "network error"}")
            }
            withContext(Dispatchers.Main) { applyImport(result) }
        }
    }

    private fun applyImport(result: List<String>) {
        uiState = uiState.copy(
            party = repo.party.toList(),
            dirty = true,
            selected = 0,
            messages = result
        )
    }

    private fun removeSelected() {
        repo.removeAt(uiState.selected)
        uiState = uiState.copy(
            party = repo.party.toList(),
            dirty = true,
            selected = 0,
            messages = listOf("Removed from party")
        )
    }

    private fun saveTeam() {
        val messages = repo.saveToDisk()
        uiState = uiState.copy(dirty = false, messages = messages)
    }

    private fun shareSave() {
        val uri = repo.shareUri() ?: run {
            uiState = uiState.copy(messages = listOf("Save the team first, then share"))
            return
        }
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "application/octet-stream"
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivity(Intent.createChooser(send, "Send team save to the host"))
    }

    companion object {
        @JvmStatic
        fun launch(context: Context) {
            context.startActivity(Intent(context, TeamEditorActivity::class.java))
        }
    }
}

enum class TeamRole(val deviceNumber: Int, val label: String, val templateAsset: String) {
    HOST(1, "Host (GBA 2)", "xdnetplay/EMERALD-2.sav"),
    GUEST(2, "Guest (GBA 3)", "xdnetplay/EMERALD-3.sav")
}

data class TeamEditorState(
    val role: TeamRole = TeamRole.HOST,
    val ready: Boolean = false,
    val trainer: String = "",
    val party: List<Gen3Mon> = emptyList(),
    val selected: Int = 0,
    val dirty: Boolean = false,
    val messages: List<String> = emptyList()
)

/** Loads, edits, verifies, and writes the Emerald save Dolphin uses for netplay. */
class TeamRepo(private val context: Context) {
    data class Loaded(val trainerLabel: String, val party: List<Gen3Mon>, val messages: List<String>)

    private val data: Gen3Data by lazy { Gen3Data.load(context) }
    private val names: DisplayNames by lazy { DisplayNames.load(context) }

    private var save: EmeraldSave? = null
    private var savePath: File? = null
    var party: MutableList<Gen3Mon> = mutableListOf()
        private set

    private fun romPath(role: TeamRole): String? {
        val preferred = when (role) {
            TeamRole.HOST -> StringSetting.MAIN_GBA_ROM_2.string
            TeamRole.GUEST -> StringSetting.MAIN_GBA_ROM_3.string
        }
        val fallback = when (role) {
            TeamRole.HOST -> StringSetting.MAIN_GBA_ROM_3.string
            TeamRole.GUEST -> StringSetting.MAIN_GBA_ROM_2.string
        }
        return listOf(preferred, fallback).firstOrNull { it.isNotEmpty() }
    }

    fun load(role: TeamRole): Loaded {
        val rom = romPath(role)
            ?: throw IllegalStateException("no Emerald ROM configured (Settings → GBA)")
        val savesDir = File(DirectoryInitialization.getUserDirectory(), "GBA" + File.separator + "Saves")
        savesDir.mkdirs()
        val path = File(SaveNaming.deriveSavePath(savesDir.path, rom, role.deviceNumber))
        savePath = path

        val messages = mutableListOf<String>()
        val bytes = if (path.exists()) {
            messages += "Loaded ${path.name}"
            path.readBytes()
        } else {
            messages += "No save yet — starting from the bundled template"
            context.assets.open(role.templateAsset).use { it.readBytes() }
        }

        val parsed = EmeraldSave(bytes)
        val mismatches = parsed.verifyAllChecksums()
        if (mismatches.isNotEmpty()) {
            messages += "Warning: ${mismatches.size} section checksum(s) invalid in source save"
        }
        save = parsed
        party = parsed.readParty().filter { !it.isEmpty() }.toMutableList()
        return Loaded("${parsed.trainerName}  ·  ${path.name}", party.toList(), messages)
    }

    fun importShowdown(text: String): List<String> {
        val s = save ?: return listOf("No save loaded")
        val messages = mutableListOf<String>()
        val sets = ShowdownParser.parseTeam(text)
        if (sets.isEmpty()) return listOf("Nothing recognizable in that paste")

        val built = mutableListOf<Gen3Mon>()
        for (set in sets) {
            if (built.size == 6) {
                messages += "Skipped ${set.species}: party is full"
                continue
            }
            try {
                built += MonFactory.build(set, data, s.trainerName, s.trainerId)
            } catch (e: Exception) {
                messages += "Skipped ${set.species}: ${e.message ?: "could not build"}"
            }
        }
        if (built.isNotEmpty()) {
            party = built
            messages.add(0, "Imported ${built.size} Pokémon (replaced party)")
        }
        return messages
    }

    fun removeAt(index: Int) {
        if (index in party.indices) party.removeAt(index)
    }

    fun saveToDisk(): List<String> {
        val s = save ?: return listOf("No save loaded")
        val path = savePath ?: return listOf("No save path")
        val messages = mutableListOf<String>()
        return try {
            s.writeParty(party)
            val out = s.toBytes()

            // Independent verification of our own output before it touches disk.
            val reparsed = EmeraldSave(out)
            val bad = reparsed.verifyAllChecksums()
            check(bad.isEmpty()) { "output failed checksum verification: $bad" }
            check(reparsed.readParty().filter { !it.isEmpty() }.size == party.size) {
                "output party did not round-trip"
            }

            if (path.exists()) {
                path.copyTo(File(path.path + ".bak"), overwrite = true)
                messages += "Backed up previous save to ${path.name}.bak"
            }
            val tmp = File(path.path + ".tmp")
            tmp.writeBytes(out)
            check(tmp.renameTo(path)) { "could not replace ${path.name}" }
            messages.add(0, "Saved ${party.size} Pokémon to ${path.name} ✓ verified")
            messages
        } catch (e: Exception) {
            listOf("NOT saved — ${e.message ?: "verification failed"}")
        }
    }

    fun shareUri() = savePath?.takeIf { it.exists() }?.let {
        FileProvider.getUriForFile(context, context.packageName + ".xdshare", it)
    }

    fun displayNames() = names
}

/** Reverse id → display-name maps, read straight from the bundled data asset. */
class DisplayNames private constructor(
    val species: Map<Int, String>,
    val items: Map<Int, String>,
    val moves: Map<Int, String>
) {
    companion object {
        fun load(context: Context): DisplayNames {
            val root = JSONObject(
                context.assets.open("xdnetplay/gen3data.json").bufferedReader().readText()
            )
            fun reverse(key: String, idField: String?): Map<Int, String> {
                val obj = root.getJSONObject(key)
                val out = HashMap<Int, String>()
                for (name in obj.keys()) {
                    val id = if (idField == null) {
                        obj.getInt(name)
                    } else {
                        obj.getJSONObject(name).getInt(idField)
                    }
                    if (!out.containsKey(id)) {
                        out[id] = name.replaceFirstChar { it.uppercase() }
                    }
                }
                return out
            }
            return DisplayNames(
                species = reverse("species", "id"),
                items = reverse("items", null),
                moves = reverse("moves", null)
            )
        }
    }
}
