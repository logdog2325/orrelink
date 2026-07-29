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
import androidx.lifecycle.lifecycleScope
import java.io.File
import java.net.URL
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.ShowdownParser
import org.dolphinemu.dolphinemu.features.xdnetplay.pbr.Bk4Factory
import org.dolphinemu.dolphinemu.features.xdnetplay.pbr.Bk4Mon
import org.dolphinemu.dolphinemu.features.xdnetplay.pbr.Gen4Data
import org.dolphinemu.dolphinemu.features.xdnetplay.pbr.Gen4Growth
import org.dolphinemu.dolphinemu.features.xdnetplay.pbr.Gen4Text
import org.dolphinemu.dolphinemu.features.xdnetplay.pbr.PbrContainer
import org.dolphinemu.dolphinemu.features.xdnetplay.pbr.PbrSave
import org.dolphinemu.dolphinemu.features.xdnetplay.pbr.PbrSaveFile
import org.dolphinemu.dolphinemu.features.xdnetplay.pbr.loadGen4Data
import org.dolphinemu.dolphinemu.ui.main.ThemeProvider
import org.dolphinemu.dolphinemu.ui.theme.DolphinTheme
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import org.dolphinemu.dolphinemu.utils.ThemeHelper

/**
 * Team editor for Pokémon Battle Revolution — the Gen 4 sibling of
 * [TeamEditorActivity], which does the same job for the XD side's Gen 3 saves.
 *
 * The save is not a file the user picks: it lives inside Dolphin's emulated
 * NAND (see [PbrSaveFile]) and only exists once PBR has created a profile, so
 * the "nothing to edit yet" path is a first-class state rather than an error.
 *
 * Loading decrypts 3.5 MB and bit-counts two 1.8 MB checksum spans, so every
 * disk touch happens on [Dispatchers.IO] — same shape as the pokepast.es fetch
 * in the Gen 3 editor.
 */
class PbrTeamEditorActivity : AppCompatActivity(), ThemeProvider {
    override var themeId: Int = 0

    private lateinit var repo: PbrTeamRepo
    private var uiState by mutableStateOf(PbrEditorState())

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeHelper.setTheme(this)
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)

        repo = PbrTeamRepo(this)

        DirectoryInitialization.start(this)
        AfterDirectoryInitializationRunner().runWithLifecycle(this) { openSave() }

        setContent {
            DolphinTheme {
                PbrTeamEditorScreen(
                    state = uiState,
                    onSelectProfile = { pickProfile(it) },
                    onSelectContainer = { pickContainer(it) },
                    onSelectSlot = { uiState = uiState.copy(selected = it) },
                    onImport = { runImport(it) },
                    onEdit = { runEdit(it) },
                    onRemoveSelected = { removeSelected() },
                    onSave = { writeSave() },
                    onBack = { finish() }
                )
            }
        }
    }

    private fun openSave() {
        uiState = uiState.copy(loading = true, messages = listOf("Reading PbrSaveData…"))
        lifecycleScope.launch(Dispatchers.IO) {
            val loaded = repo.open()
            withContext(Dispatchers.Main) {
                uiState = loaded
            }
        }
    }

    private fun pickProfile(profile: Int) {
        uiState = repo.view(uiState, profile, PbrContainer.Party)
    }

    private fun pickContainer(container: PbrContainer) {
        uiState = repo.view(uiState, uiState.profile, container)
    }

    /**
     * Accepts either a Showdown export pasted inline or a pokepast.es link,
     * which is fetched off the main thread (mirrors TeamEditorActivity).
     */
    private fun runImport(text: String) {
        val pokepaste = Regex("^https?://pokepast\\.es/[A-Za-z0-9]+")
            .find(text.trim())?.value
        if (pokepaste == null) {
            uiState = repo.importShowdown(uiState, text)
            return
        }
        uiState = uiState.copy(messages = listOf("Fetching $pokepaste…"))
        lifecycleScope.launch(Dispatchers.IO) {
            val body = try {
                URL("$pokepaste/raw").readText()
            } catch (_: Exception) {
                null
            }
            withContext(Dispatchers.Main) {
                uiState = if (body == null) {
                    uiState.copy(messages = listOf("Could not fetch that paste (network error)"))
                } else {
                    repo.importShowdown(uiState, body)
                }
            }
        }
    }

    private fun runEdit(form: PbrEditForm) {
        uiState = repo.applyEdit(uiState, form)
    }

    private fun removeSelected() {
        uiState = repo.removeSelected(uiState)
    }

    private fun writeSave() {
        val snapshot = uiState
        uiState = snapshot.copy(messages = listOf("Verifying and writing…"))
        lifecycleScope.launch(Dispatchers.IO) {
            val next = repo.saveToDisk(snapshot)
            withContext(Dispatchers.Main) { uiState = next }
        }
    }

    companion object {
        @JvmStatic
        fun launch(context: Context) {
            context.startActivity(Intent(context, PbrTeamEditorActivity::class.java))
        }
    }
}

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------

/** One profile ("save slot") as the chip row needs it. */
data class PbrProfileInfo(val index: Int, val otName: String, val monCount: Int) {
    val label: String get() = if (otName.isEmpty()) "Slot ${index + 1}" else otName
}

/**
 * A Pokémon flattened for display. Deliberately holds no [Bk4Mon]: the record
 * stays inside the repo so nothing in Compose can mutate a save by accident.
 */
data class PbrEntry(
    val slot: Int,
    val offset: Int,
    val species: Int,
    val speciesName: String,
    val nickname: String,
    val level: Int,
    val itemName: String,
    val abilityName: String,
    val natureName: String,
    val otName: String,
    val gender: Int,
    val shiny: Boolean,
    val checksumOk: Boolean,
    val moveNames: List<String>,
    val evs: List<Int>,
    val ivs: List<Int>
)

data class PbrEditorState(
    val loading: Boolean = true,
    val ready: Boolean = false,
    val fileLabel: String = "",
    val profiles: List<PbrProfileInfo> = emptyList(),
    val profile: Int = 0,
    val containerLabel: String = PbrContainer.Party.label,
    val boxLabels: List<String> = emptyList(),
    val entries: List<PbrEntry> = emptyList(),
    val slotCount: Int = PbrSave.PARTY_SLOTS,
    val selected: Int = -1,
    val dirty: Boolean = false,
    val messages: List<String> = emptyList()
) {
    val selectedEntry: PbrEntry? get() = entries.getOrNull(selected)
}

/** The editable fields of one Pokémon, as free text the repo parses. */
data class PbrEditForm(
    val nickname: String,
    val level: String,
    val item: String,
    val ability: String,
    val nature: String,
    val moves: String,
    val evs: String,
    val ivs: String,
    val shiny: Boolean
)

// ---------------------------------------------------------------------------
// Repository
// ---------------------------------------------------------------------------

/**
 * Owns the decrypted save and every mutation of it. Edits are applied straight
 * to the in-memory image one 136-byte record at a time, so a container the user
 * merely looked at is never rewritten — only slots they actually changed.
 */
class PbrTeamRepo(private val context: Context) {
    private val data: Gen4Data by lazy { loadGen4Data(context) }

    private var save: PbrSave? = null
    private var file: File? = null
    private var container: PbrContainer = PbrContainer.Party

    /** Load the NAND save and build the initial state. Call off the main thread. */
    fun open(): PbrEditorState {
        val located = PbrSaveFile.find()
            ?: return PbrEditorState(
                loading = false,
                ready = false,
                messages = listOf(
                    "No PbrSaveData found in Dolphin's NAND.",
                    "Run Pokémon Battle Revolution once (online, so it creates a " +
                        "profile) and come back — the game writes the save itself.",
                    "Looked in:"
                ) + PbrSaveFile.searchedPaths()
            )

        return try {
            val parsed = PbrSave.load(located.file.readBytes())
            save = parsed
            file = located.file
            container = PbrContainer.Party

            val messages = mutableListOf(
                "Loaded ${located.region} save, partition ${parsed.partition} " +
                    "(save count ${parsed.saveCount})"
            )
            val suspect = parsed.partitions.filterNot { it.valid }
            if (suspect.isNotEmpty()) {
                messages += "Note: partition ${suspect.joinToString { it.index.toString() }} " +
                    "fails its checksums and will not be touched"
            }

            val profiles = (0 until PbrSave.PROFILE_COUNT).map { p ->
                PbrProfileInfo(p, parsed.profileOtName(p), parsed.readAllEntities(p).size)
            }
            val first = profiles.firstOrNull { it.monCount > 0 }?.index ?: 0
            view(
                PbrEditorState(
                    loading = false,
                    ready = true,
                    fileLabel = "${located.region} · ${located.file.name}",
                    profiles = profiles,
                    boxLabels = (0 until PbrSave.BOX_COUNT).map { box ->
                        parsed.boxName(first, box).ifBlank { "Box ${box + 1}" }
                    },
                    messages = messages
                ),
                first,
                PbrContainer.Party
            )
        } catch (e: Exception) {
            PbrEditorState(
                loading = false,
                ready = false,
                messages = listOf(
                    "Could not read ${located.file.name}: ${e.message ?: "unknown error"}",
                    "Nothing was written. The file is left exactly as it was."
                )
            )
        }
    }

    /** Re-read one container of one profile into the UI state. */
    fun view(state: PbrEditorState, profile: Int, next: PbrContainer): PbrEditorState {
        val parsed = save ?: return state
        container = next
        val entries = parsed.readContainer(profile, next).mapIndexedNotNull { slot, s ->
            s.mon?.let { toEntry(slot, s.offset, it) }
        }
        return state.copy(
            profile = profile,
            containerLabel = next.label,
            boxLabels = (0 until PbrSave.BOX_COUNT).map { box ->
                parsed.boxName(profile, box).ifBlank { "Box ${box + 1}" }
            },
            entries = entries,
            slotCount = next.slotCount,
            selected = if (entries.isEmpty()) -1 else 0
        )
    }

    private fun toEntry(slot: Int, offset: Int, mon: Bk4Mon): PbrEntry = PbrEntry(
        slot = slot,
        offset = offset,
        species = mon.species,
        speciesName = data.speciesName(mon.species),
        nickname = mon.nickname,
        level = mon.level,
        itemName = if (mon.heldItem == 0) "—" else data.itemName(mon.heldItem),
        abilityName = data.abilityName(mon.ability),
        natureName = data.natureName(mon.nature),
        otName = mon.otName,
        gender = mon.genderRaw,
        shiny = mon.isShiny,
        checksumOk = mon.isChecksumValid,
        moveNames = mon.moves.filter { it != 0 }.map { data.moveName(it) },
        evs = mon.evs.toList(),
        ivs = mon.ivs.toList()
    )

    // -- mutations ------------------------------------------------------------

    fun importShowdown(state: PbrEditorState, text: String): PbrEditorState {
        val parsed = save ?: return state.copy(messages = listOf("No save loaded"))
        val sets = ShowdownParser.parseTeam(text)
        if (sets.isEmpty()) {
            return state.copy(messages = listOf("Nothing recognizable in that paste"))
        }

        val messages = mutableListOf<String>()
        val otName = parsed.profileOtName(state.profile).ifEmpty { "PBR" }
        val tid = parsed.profileTid(state.profile)
        val sid = parsed.profileSid(state.profile)
        val built = mutableListOf<Bk4Mon>()
        for (set in sets) {
            if (built.size == container.slotCount) {
                messages += "Skipped ${set.species}: ${container.label} is full"
                continue
            }
            try {
                built += Bk4Factory.build(set, data, otName, tid, sid)
            } catch (e: Exception) {
                messages += "Skipped ${set.species}: ${e.message ?: "could not build"}"
            }
        }
        if (built.isEmpty()) {
            return state.copy(messages = messages)
        }
        parsed.writeContainer(state.profile, container, built)
        messages.add(0, "Imported ${built.size} Pokémon into ${container.label} (replaced it)")
        return view(state, state.profile, container).copy(dirty = true, messages = messages)
    }

    fun removeSelected(state: PbrEditorState): PbrEditorState {
        val parsed = save ?: return state
        val entry = state.selectedEntry
            ?: return state.copy(messages = listOf("Nothing selected"))
        val kept = parsed.readContainer(state.profile, container)
            .mapNotNull { it.mon }
            .filterIndexed { i, _ -> i != state.selected }
        parsed.writeContainer(state.profile, container, kept)
        return view(state, state.profile, container).copy(
            dirty = true,
            messages = listOf("Removed ${entry.speciesName} from ${container.label}")
        )
    }

    /**
     * Apply the edit dialog's fields. The PID is only re-rolled when the
     * requested nature, ability slot or shininess actually disagrees with the
     * one it already encodes — editing a nickname must not silently change a
     * Pokémon's identity.
     */
    fun applyEdit(state: PbrEditorState, form: PbrEditForm): PbrEditorState {
        val parsed = save ?: return state
        val entry = state.selectedEntry
            ?: return state.copy(messages = listOf("Nothing selected"))
        val mon = parsed.readEntity(entry.offset)?.copy()
            ?: return state.copy(messages = listOf("That slot is empty"))

        return try {
            val species = data.species(data.speciesName(mon.species))
                ?: throw IllegalArgumentException("unknown species #${mon.species}")

            val level = form.level.trim().toIntOrNull()
                ?: throw IllegalArgumentException("level must be a number")
            require(level in 1..100) { "level must be 1..100" }

            val heldItem = form.item.trim().let {
                if (it.isEmpty() || it == "—") {
                    0
                } else {
                    data.itemId(it) ?: throw IllegalArgumentException("unknown item: $it")
                }
            }
            val abilityId = form.ability.trim().let {
                if (it.isEmpty()) {
                    mon.ability
                } else {
                    data.abilityId(it) ?: throw IllegalArgumentException("unknown ability: $it")
                }
            }
            val nature = data.nature(form.nature.trim())
                ?: throw IllegalArgumentException("unknown nature: ${form.nature}")

            val moveIds = IntArray(4)
            val moveNames = form.moves.split(",", "\n")
                .map { it.trim() }
                .filter { it.isNotEmpty() }
            require(moveNames.size <= 4) { "at most 4 moves" }
            for (i in moveNames.indices) {
                moveIds[i] = data.moveId(moveNames[i])
                    ?: throw IllegalArgumentException("unknown move: ${moveNames[i]}")
            }

            val spreads = ShowdownParser.parseSet(
                listOf("Placeholder", "EVs: ${form.evs}", "IVs: ${form.ivs}")
            ) ?: throw IllegalArgumentException("could not read the EV/IV spread")

            val wantedSlot = Bk4Factory.abilitySlotFor(species, abilityId)
            if (mon.nature != nature.id ||
                (mon.pid and 1) != wantedSlot ||
                mon.isShiny != form.shiny
            ) {
                val wantedGender = when (mon.genderRaw) {
                    Bk4Factory.GENDER_FEMALE -> 'F'
                    Bk4Factory.GENDER_MALE -> 'M'
                    else -> null
                }
                mon.pid = Bk4Factory.choosePid(
                    natureId = nature.id,
                    abilitySlot = wantedSlot,
                    genderRatio = species.genderRatio,
                    wantedGender = wantedGender,
                    shiny = form.shiny,
                    tid = mon.tid,
                    sid = mon.sid
                )
                mon.genderRaw = Bk4Factory.genderFor(species.genderRatio, mon.pid)
            }

            val previousMoves = mon.moves
            val previousPp = mon.movePp
            mon.moves = moveIds
            mon.movePp = IntArray(4) { i ->
                when {
                    moveIds[i] == 0 -> 0
                    moveIds[i] == previousMoves[i] -> previousPp[i]
                    else -> DEFAULT_PP
                }
            }
            mon.heldItem = heldItem
            mon.ability = abilityId
            mon.exp = Gen4Growth.expFor(level, mon.species)
            mon.evs = IntArray(6) { spreads.evs[it].coerceIn(0, 255) }
            mon.ivs = IntArray(6) { spreads.ivs[it].coerceIn(0, 31) }

            val nickname = Bk4Factory.sanitizeName(form.nickname.trim(), Bk4Mon.NICKNAME_CHARS)
            if (nickname.isEmpty() || nickname.equals(species.name, ignoreCase = true)) {
                mon.nickname = species.name
                mon.isNicknamed = false
            } else {
                mon.nickname = nickname
                mon.isNicknamed = true
            }

            parsed.writeEntity(entry.offset, mon)
            val messages = mutableListOf("Updated ${data.speciesName(mon.species)}")
            val evTotal = mon.evs.sum()
            if (evTotal > 510) {
                messages += "Warning: EV total is $evTotal (the games cap it at 510)"
            }
            view(state, state.profile, container)
                .copy(selected = state.selected, dirty = true, messages = messages)
        } catch (e: Exception) {
            state.copy(messages = listOf("Not applied — ${e.message ?: "invalid input"}"))
        }
    }

    // -- write ----------------------------------------------------------------

    /**
     * Verified write: build the output, parse it back from scratch, check every
     * container checksum and every Pokémon byte-for-byte, then back up the
     * original and swap the new file in through a temp file. Any failure leaves
     * the NAND save untouched.
     */
    fun saveToDisk(state: PbrEditorState): PbrEditorState {
        val parsed = save ?: return state.copy(messages = listOf("No save loaded"))
        val target = file ?: return state.copy(messages = listOf("No save path"))
        val messages = mutableListOf<String>()
        return try {
            val out = parsed.toBytes()

            val verify = PbrSave.load(out)
            check(verify.partition == parsed.partition) {
                "the active partition moved (${parsed.partition} -> ${verify.partition})"
            }
            check(verify.partitions.all { it.valid == parsed.partitions[it.index].valid }) {
                "a partition's checksum validity changed"
            }
            check(verify.partitions[parsed.partition].valid) {
                "the live partition's checksums do not verify"
            }
            var checked = 0
            for (p in 0 until PbrSave.PROFILE_COUNT) {
                val before = parsed.readAllEntities(p)
                val after = verify.readAllEntities(p)
                check(before.size == after.size) {
                    "profile $p holds ${after.size} Pokémon after the write, expected ${before.size}"
                }
                for (i in before.indices) {
                    check(
                        before[i].offset == after[i].offset &&
                            before[i].mon!!.data.contentEquals(after[i].mon!!.data)
                    ) { "profile $p slot ${before[i].label} did not round-trip" }
                    checked++
                }
            }

            if (target.exists()) {
                target.copyTo(File(target.path + ".bak"), overwrite = true)
                messages += "Backed up the previous save to ${target.name}.bak"
            }
            val tmp = File(target.path + ".tmp")
            tmp.writeBytes(out)
            check(tmp.renameTo(target)) { "could not replace ${target.name}" }
            messages.add(0, "Saved ✓ verified — $checked Pokémon re-read byte-for-byte")
            state.copy(dirty = false, messages = messages)
        } catch (e: Exception) {
            state.copy(
                messages = listOf(
                    "NOT saved — ${e.message ?: "verification failed"}",
                    "Your PbrSaveData was left untouched."
                )
            )
        }
    }

    companion object {
        val STAT_LABELS = listOf("HP", "Atk", "Def", "SpA", "SpD", "Spe")
        private const val DEFAULT_PP = 40

        /**
         * "0 HP / 252 Atk / 0 Def / 0 SpA / 4 SpD / 252 Spe" — Showdown's own
         * syntax, written out in full so the edit dialog can hand the string
         * straight back to [ShowdownParser] without a blank field meaning
         * "reset this stat".
         */
        fun spreadText(values: List<Int>): String =
            STAT_LABELS.indices.joinToString(" / ") {
                "${values.getOrElse(it) { 0 }} ${STAT_LABELS[it]}"
            }

        /** Characters the Gen 4 glyph table cannot store, for a live hint. */
        fun unencodableIn(text: String): String =
            text.filterNot { Gen4Text.canEncodeGlyph(it) }
    }
}
