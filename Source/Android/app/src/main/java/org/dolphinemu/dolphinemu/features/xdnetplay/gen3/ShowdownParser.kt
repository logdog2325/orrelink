// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.gen3

/**
 * One parsed set from a Pokemon Showdown team export.
 *
 * EV/IV arrays use Showdown's writing order: hp, atk, def, spa, spd, spe.
 * (Note this differs from the Gen 3 save order hp/atk/def/speed/spatk/spdef;
 * MonFactory remaps.)
 */
data class ShowdownSet(
    val species: String,
    val nickname: String? = null,
    val gender: Char? = null,   // 'M' / 'F' from a "(M)"/"(F)" marker, if any
    val item: String? = null,
    val ability: String? = null,
    val level: Int = 100,
    val shiny: Boolean = false,
    val nature: String? = null,
    val evs: IntArray = IntArray(6),          // hp, atk, def, spa, spd, spe
    val ivs: IntArray = IntArray(6) { 31 },   // hp, atk, def, spa, spd, spe
    val moves: List<String> = emptyList()     // max 4
)

/**
 * Parser for Showdown team-export text. Sets are separated by blank lines;
 * lines that are not recognized (e.g. "Happiness:", "Tera Type:") are
 * ignored gracefully.
 */
object ShowdownParser {
    private const val MAX_MOVES = 4

    // Showdown stat tokens -> index in the hp/atk/def/spa/spd/spe arrays.
    private val STAT_INDEX = mapOf(
        "hp" to 0,
        "atk" to 1, "attack" to 1,
        "def" to 2, "defense" to 2,
        "spa" to 3, "spatk" to 3, "spatt" to 3, "spattack" to 3,
        "spd" to 4, "spdef" to 4, "spdefense" to 4,
        "spe" to 5, "speed" to 5
    )

    /** Parse a full export possibly containing multiple blank-line-separated sets. */
    fun parseTeam(text: String): List<ShowdownSet> {
        val sets = ArrayList<ShowdownSet>()
        val block = ArrayList<String>()
        for (rawLine in text.lines()) {
            val line = rawLine.trimEnd('\r').trim()
            if (line.isEmpty()) {
                parseSet(block)?.let { sets.add(it) }
                block.clear()
            } else if (!line.startsWith("===")) {
                // "=== [gen3ou] Team Name ===" headers from full exports are skipped.
                block.add(line)
            }
        }
        parseSet(block)?.let { sets.add(it) }
        return sets
    }

    /** Parse one set from its (trimmed, non-blank) lines. Returns null if empty. */
    fun parseSet(lines: List<String>): ShowdownSet? {
        if (lines.isEmpty()) {
            return null
        }

        // ---- header line: "Nickname (Species) (G) @ Item" with every part
        // except the species optional.
        var header = lines[0]
        var item: String? = null
        val atIdx = header.lastIndexOf(" @ ")
        if (atIdx >= 0) {
            item = header.substring(atIdx + 3).trim().ifEmpty { null }
            header = header.substring(0, atIdx).trim()
        }

        var gender: Char? = null
        if (header.endsWith("(M)")) {
            gender = 'M'
            header = header.dropLast(3).trim()
        } else if (header.endsWith("(F)")) {
            gender = 'F'
            header = header.dropLast(3).trim()
        }

        var nickname: String? = null
        var speciesName = header
        if (header.endsWith(")")) {
            val open = header.lastIndexOf('(')
            if (open > 0) {
                speciesName = header.substring(open + 1, header.length - 1).trim()
                nickname = header.substring(0, open).trim().ifEmpty { null }
            }
        }
        if (speciesName.isEmpty()) {
            return null
        }

        // ---- body lines
        var ability: String? = null
        var level = 100
        var shiny = false
        var nature: String? = null
        val evs = IntArray(6)
        val ivs = IntArray(6) { 31 }
        val moves = ArrayList<String>()

        for (i in 1 until lines.size) {
            val line = lines[i]
            when {
                line.startsWith("Ability:") -> {
                    ability = line.substring("Ability:".length).trim().ifEmpty { null }
                }
                line.startsWith("Level:") -> {
                    val n = line.substring("Level:".length).trim().toIntOrNull()
                    if (n != null) {
                        level = n
                    }
                }
                line.startsWith("Shiny:") -> {
                    shiny = line.substring("Shiny:".length).trim()
                        .equals("Yes", ignoreCase = true)
                }
                line.startsWith("EVs:") -> {
                    parseStatList(line.substring("EVs:".length), evs)
                }
                line.startsWith("IVs:") -> {
                    parseStatList(line.substring("IVs:".length), ivs)
                }
                line.startsWith("-") -> {
                    if (moves.size < MAX_MOVES) {
                        val move = line.substring(1).trim()
                        if (move.isNotEmpty()) {
                            moves.add(move)
                        }
                    }
                }
                line.endsWith(" Nature") -> {
                    val n = line.removeSuffix(" Nature").trim()
                    if (n.isNotEmpty()) {
                        nature = n
                    }
                }
                // Anything else ("Happiness:", "Tera Type:", ...) is ignored.
            }
        }

        return ShowdownSet(
            species = speciesName,
            nickname = nickname,
            gender = gender,
            item = item,
            ability = ability,
            level = level,
            shiny = shiny,
            nature = nature,
            evs = evs,
            ivs = ivs,
            moves = moves
        )
    }

    /** Parse "252 Atk / 4 Def / 252 Spe" into [out]; unknown tokens ignored. */
    private fun parseStatList(text: String, out: IntArray) {
        for (part in text.split("/")) {
            val tokens = part.trim().split(Regex("\\s+"))
            if (tokens.size < 2) {
                continue
            }
            val value = tokens[0].toIntOrNull() ?: continue
            val index = STAT_INDEX[tokens[1].lowercase()] ?: continue
            out[index] = value
        }
    }
}
