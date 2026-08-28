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
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.features.netplay.NetplayManager
import org.dolphinemu.dolphinemu.features.settings.model.IntSetting
import org.dolphinemu.dolphinemu.features.settings.model.StringSetting
import org.dolphinemu.dolphinemu.features.xdnetplay.FormatBridge
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.EmeraldSave
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.Gen3Data
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.Gen3Game
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.Gen3Mon
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.Gen3Text
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.MonFactory
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.SaveNaming
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.ShowdownParser
import org.dolphinemu.dolphinemu.ui.main.ThemeProvider
import org.dolphinemu.dolphinemu.ui.theme.DolphinTheme
import org.dolphinemu.dolphinemu.features.xdnetplay.SaveImportBridge
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
        // The editor displays whatever the socket save holds -- after a killed
        // session that can be the OPPONENT's team. Heal leftovers before the
        // first read; no-op in a live room (the room-wide lock governs there).
        if (DirectoryInitialization.areDolphinDirectoriesReady()) {
            SaveImportBridge.healLeftoverSession()
        }
        reload(TeamRole.HOST)

        setContent {
            DolphinTheme {
                TeamEditorScreen(
                    state = uiState,
                    names = if (uiState.ready) repo.displayNames() else null,
                    onRoleChange = { reload(it) },
                    onSelect = { uiState = uiState.copy(selected = it) },
                    onTrainerNameChange = { setTrainerName(it) },
                    onImport = { importShowdown(it) },
                    onRemoveSelected = { removeSelected() },
                    onSave = { saveTeam() },
                    onShare = { shareSave() },
                    onBack = { finish() }
                )
            }
        }
    }

    /**
     * The trainer-name field. Only characters the Gen 3 charset can hold are
     * accepted, and only seven of them: rejecting the keystroke is how the user
     * finds out, instead of a save-time error about something typed minutes
     * ago. (Straight quotes are folded to the charset's curly forms.)
     */
    private fun setTrainerName(text: String) {
        val filtered = Gen3Text.sanitize(text, EmeraldSave.TRAINER_NAME_LEN)
        uiState = uiState.copy(trainerName = filtered, dirty = true)
    }

    private fun reload(role: TeamRole) {
        // Competitive integrity. While this device is HOSTING a room, the socket-3 save is the
        // opponent's submitted party -- every EV, IV and nature -- written there so netplay can
        // sync it at start. Opening it here would be a scouting tool. The host's own socket-2
        // save stays editable, and the end-of-session cleanup puts the host's team back in
        // socket 3, so this unlocks on its own when the room closes. Joiners are unaffected:
        // their local socket-3 save is their own team; netplay runs them from NetPlayTemp copies.
        if (NetplayManager.activeSession?.isHosting == true) {
            // Both roles lock while a room is open: the guest slot may hold the opponent's
            // submitted team, and the host slot holds the session's privacy DISPOSABLE when the
            // save is an import — an editor save would write the full import back over it and
            // the next Start would sync exactly the file the disposable keeps off the wire.
            uiState = TeamEditorState(
                role = role,
                ready = false,
                messages = listOf(
                    "The team editor is locked while a netplay room is open. Close the room " +
                        "to edit teams."
                )
            )
            return
        }

        uiState = try {
            val loaded = repo.load(role)
            TeamEditorState(
                role = role,
                // An imported FireRed/LeafGreen save is usable for PLAY but not
                // editable here (FRLG keeps the party at different offsets);
                // the repo explains that in loaded.messages. Its lock message
                // is self-contained, so the generic "set your ROM" setup hint
                // would only mislead.
                ready = loaded.editable,
                setupHint = loaded.editable,
                trainer = loaded.trainerLabel,
                trainerName = loaded.trainerName,
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
        val trainerName = uiState.trainerName
        val pokepaste = Regex("^https?://pokepast\\.es/[A-Za-z0-9]+")
            .find(text.trim())?.value
        if (pokepaste == null) {
            applyImport(repo.importShowdown(text, trainerName))
            return
        }
        uiState = uiState.copy(messages = listOf("Fetching $pokepaste…"))
        lifecycleScope.launch(Dispatchers.IO) {
            val result = try {
                repo.importShowdown(URL("$pokepaste/raw").readText(), trainerName)
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

    // True when the file currently behind the editor is the opponent's team. The role check in
    // reload() covers opening it; this covers an editor that was already open on the guest slot
    // when the room started, and is reached again from the back stack.
    private fun guestSlotLocked() =
        NetplayManager.activeSession?.isHosting == true

    private fun saveTeam() {
        if (guestSlotLocked()) {
            uiState = uiState.copy(
                messages = listOf("Not saved — the team editor is locked while a netplay room is open")
            )
            return
        }
        val messages = repo.saveToDisk(uiState.trainerName)
        // Only a clean write clears the dirty flag; a rejected name must keep
        // the Save button live so the user can fix it and try again.
        val failed = messages.any { it.startsWith("NOT saved") }
        uiState = uiState.copy(dirty = failed, messages = messages)
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

    private fun shareSave() {
        if (guestSlotLocked()) {
            uiState = uiState.copy(
                messages = listOf("Not shared — the guest slot is your opponent's while you host")
            )
            return
        }
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
    /**
     * Whether the not-ready panel should append the generic "set your Emerald
     * ROM" setup hint. False when [messages] already fully explains the lock
     * (e.g. an imported FRLG save), where that hint would be misleading.
     */
    val setupHint: Boolean = true,
    val trainer: String = "",
    /** Editable in-game trainer name; max [EmeraldSave.TRAINER_NAME_LEN] chars. */
    val trainerName: String = "",
    val party: List<Gen3Mon> = emptyList(),
    val selected: Int = 0,
    val dirty: Boolean = false,
    val messages: List<String> = emptyList()
)

/** Loads, edits, verifies, and writes the Emerald save Dolphin uses for netplay. */
class TeamRepo(private val context: Context) {
    data class Loaded(
        val trainerLabel: String,
        val trainerName: String,
        val party: List<Gen3Mon>,
        val messages: List<String>,
        /**
         * False for an imported FireRed/LeafGreen save: it will be USED for
         * play exactly as imported, but the editor must not touch it (FRLG
         * stores the party at different offsets, so an Emerald-offset write
         * would corrupt it). [messages] then carries the explanation.
         */
        val editable: Boolean = true
    )

    private val data: Gen3Data by lazy { Gen3Data.load(context) }
    private val names: DisplayNames by lazy { DisplayNames.load(context) }

    private var save: EmeraldSave? = null
    private var savePath: File? = null

    /**
     * Which Gen 3 game wrote the loaded save (an import can put a
     * Ruby/Sapphire or FRLG save here). Ruby/Sapphire shares every section-0/1
     * offset this editor touches, but its section-0 checksum length differs,
     * so every checksum verify/re-stamp must go through this.
     */
    private var game: Gen3Game = Gen3Game.Emerald
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
        game = EmeraldSave.detectGame(parsed)
        if (game == Gen3Game.FireRedLeafGreen) {
            // Never read (or later write) FRLG bytes through Emerald party
            // offsets: drop the handle so saveToDisk/importShowdown fail
            // closed on "No save loaded" even if the UI gate is bypassed.
            save = null
            party = mutableListOf()
            return Loaded(
                trainerLabel = "",
                trainerName = "",
                party = emptyList(),
                messages = listOf(context.getString(R.string.xd_editor_frlg_locked)),
                editable = false
            )
        }
        // Game-aware verify: an imported Ruby/Sapphire save is fully valid
        // against ITS table and must not show Emerald-table false warnings.
        val mismatches = parsed.verifyAllChecksums(game)
        if (mismatches.isNotEmpty()) {
            messages += "Warning: ${mismatches.size} section checksum(s) invalid in source save"
        }
        save = parsed
        party = parsed.readParty().filter { !it.isEmpty() }.toMutableList()
        return Loaded(
            trainerLabel = "ID ${parsed.trainerPublicId}  ·  ${path.name}",
            trainerName = parsed.trainerName,
            party = party.toList(),
            messages = messages
        )
    }

    /**
     * Write [name] into the save's trainer block and re-stamp the party's OT
     * names to match. Throws (leaving the save untouched) if the name is empty,
     * over-long, or has a character the Gen 3 charset cannot hold.
     *
     * Every Pokemon stores its OWN copy of the OT name, so a rename has to
     * re-stamp the party; otherwise the mons read as traded outsiders in game
     * (the disobedience rules kick in above the badge cap). The OT *ID* is
     * untouched, so they remain this save's Pokemon.
     */
    fun applyTrainerName(name: String) {
        val s = save ?: throw IllegalStateException("no save loaded")
        val typed = name.trim()
        if (typed == s.trainerName) return  // unchanged: leave the party's OT alone

        s.setTrainerName(typed)
        val otBytes = Gen3Text.encode(s.trainerName, EmeraldSave.TRAINER_NAME_LEN)
        party.forEach { it.otNameRaw = otBytes.copyOf() }
    }

    fun importShowdown(text: String, trainerName: String): List<String> {
        val s = save ?: return listOf("No save loaded")
        // Commit the typed trainer name FIRST: MonFactory.build stamps each
        // mon's OT from the save's trainer block, so importing before the
        // rename lands would build the whole party under the previous owner.
        try {
            applyTrainerName(trainerName)
        } catch (e: Exception) {
            return listOf("Nothing imported — trainer name: ${e.message ?: "invalid"}")
        }

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
        orreFormatNote()?.let { messages += it }
        return messages
    }

    /**
     * Non-blocking Orre Colosseum note for the CURRENT party, or null when
     * there is nothing to say — including whenever this device's Format pick
     * is Free, where no validation call is made at all. Shown after imports
     * and saves so a later refusal from a host's gate is no surprise; the
     * ruleset itself lives only in shared core (FormatRules, via
     * [FormatBridge]), so this note and that refusal can never disagree.
     * Importing and saving are deliberately never blocked by it: the pick may
     * be for a room someone else will host, and Free rooms take any team.
     */
    private fun orreFormatNote(): String? {
        if (!FormatBridge.isOrreColosseum(IntSetting.MAIN_XD_FORMAT.int)) return null
        val reason = FormatBridge.validateParty(party)
        if (reason.isEmpty()) return null
        return context.getString(R.string.xd_format_note, reason)
    }

    fun removeAt(index: Int) {
        if (index in party.indices) party.removeAt(index)
    }

    fun saveToDisk(trainerName: String): List<String> {
        val s = save ?: return listOf("No save loaded")
        val path = savePath ?: return listOf("No save path")
        val messages = mutableListOf<String>()
        // Name before party: writeParty serializes the mons as they stand, and
        // applyTrainerName is what brings their OT names in line with a rename.
        try {
            applyTrainerName(trainerName)
        } catch (e: Exception) {
            return listOf("NOT saved — trainer name: ${e.message ?: "invalid"}")
        }
        return try {
            s.writeParty(party)
            // setTrainerName/writeParty stamp sections 0/1 with the Emerald
            // length table (they are Emerald/RS-only by construction, and RS
            // section 1 is the same length). RS section 0 is SHORTER, so
            // re-stamp both with the detected game's table — a no-op for
            // Emerald, and the difference between a save Ruby/Sapphire accepts
            // and one it silently rejects.
            s.updateSectionChecksum(0, game)
            s.updateSectionChecksum(1, game)
            val out = s.toBytes()

            // Independent verification of our own output before it touches disk.
            val reparsed = EmeraldSave(out)
            val bad = reparsed.verifyAllChecksums(game)
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
            messages.add(0, "Saved ${party.size} Pokémon as ${s.trainerName} to ${path.name} ✓ verified")
            // The save went through regardless — the format note never blocks.
            orreFormatNote()?.let { messages += it }
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
