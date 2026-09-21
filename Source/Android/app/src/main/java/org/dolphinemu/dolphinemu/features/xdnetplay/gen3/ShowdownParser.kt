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
    val moves: List<String> = emptyList(),    // max 4
    val happiness: Int? = null                // "Happiness: N", 0..255; null = 255
)

/**
 * Parser for Showdown team-export text. Sets are separated by blank lines;
 * lines that are not recognized (e.g. "Tera Type:") are
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
        var happiness: Int? = null
        val evs = IntArray(6)
        val ivs = IntArray(6) { 31 }
        val ivsGiven = BooleanArray(6)
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
                    parseStatList(line.substring("IVs:".length), ivs, ivsGiven)
                }
                line.startsWith("Happiness:") -> {
                    line.substring("Happiness:".length).trim().toIntOrNull()?.let {
                        happiness = it.coerceIn(0, 255)
                    }
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
                // Anything else ("Tera Type:", ...) is ignored.
            }
        }

        applyHiddenPowerIvs(moves, ivs, ivsGiven)
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
            moves = moves,
            happiness = happiness
        )
    }

    /** Parse "252 Atk / 4 Def / 252 Spe" into [out]; unknown tokens ignored. */
    private fun parseStatList(text: String, out: IntArray, given: BooleanArray? = null) {
        for (part in text.split("/")) {
            val tokens = part.trim().split(Regex("\\s+"))
            if (tokens.size < 2) {
                continue
            }
            val value = tokens[0].toIntOrNull() ?: continue
            val index = STAT_INDEX[tokens[1].lowercase()] ?: continue
            out[index] = value
            given?.set(index, true)
        }
    }

    // ---- Hidden Power ----
    //
    // Gen 3 and Gen 4 store no Hidden Power type. The game derives it from the
    // low bit of each IV:
    //   type = (hp + 2*atk + 4*def + 8*spe + 16*spa + 32*spd) * 15 / 63
    // A paste names the type ("Hidden Power [Ice]", "Hidden Power Ice") and
    // very often carries no IVs line at all, or only "IVs: 0 Atk". Left at 31s
    // such a set would come out as Hidden Power Dark, so the IVs are made to
    // match the type the paste asks for, the way Showdown's own importer does.
    // Kept in step with ShowdownParser.cpp, which proves the table at compile
    // time.

    /** Low-bit weight of each stat, in Showdown order hp/atk/def/spa/spd/spe. */
    private val HP_BIT_WEIGHT = intArrayOf(1, 2, 4, 16, 32, 8)

    /** The 16 types in the order the formula numbers them. */
    private val HP_TYPE_NAMES = listOf(
        "fighting", "flying", "poison", "ground", "rock", "bug", "ghost", "steel",
        "fire", "water", "grass", "electric", "psychic", "ice", "dragon", "dark"
    )

    /**
     * Showdown's standard spread per type (its HPivs table): bit i set means
     * stat i (Showdown order) is 30, every other stat is 31. All of them give
     * 70 power. Indexed like [HP_TYPE_NAMES].
     */
    private val HP_STANDARD_EVEN_MASK = intArrayOf(
        0b111100, // fighting: def spa spd spe
        0b011111, // flying:   hp atk def spa spd
        0b011100, // poison:   def spa spd
        0b011000, // ground:   spa spd
        0b110100, // rock:     def spd spe
        0b010110, // bug:      atk def spd
        0b010100, // ghost:    def spd
        0b010000, // steel:    spd
        0b101010, // fire:     atk spa spe
        0b001110, // water:    atk def spa
        0b001010, // grass:    atk spa
        0b001000, // electric: spa
        0b100010, // psychic:  atk spe
        0b000110, // ice:      atk def
        0b000010, // dragon:   atk
        0b000000, // dark:     all 31
    )

    /** Type index the game derives from a parity pattern: bit i set = stat i odd. */
    private fun hiddenPowerTypeOfOddMask(oddMask: Int): Int {
        var sum = 0
        for (i in 0 until 6) {
            if (oddMask and (1 shl i) != 0) {
                sum += HP_BIT_WEIGHT[i]
            }
        }
        return sum * 15 / 63
    }

    /** Type index named by a move line, or -1: not Hidden Power, or no type named. */
    private fun namedHiddenPowerType(move: String): Int {
        val letters = move.filter { it in 'a'..'z' || it in 'A'..'Z' }.lowercase()
        if (!letters.startsWith("hiddenpower")) {
            return -1
        }
        return HP_TYPE_NAMES.indexOf(letters.substring("hiddenpower".length))
    }

    /**
     * Makes [ivs] produce the Hidden Power type that [moves] name.
     *  - IVs already right: nothing changes.
     *  - No IVs line: Showdown's standard spread for the type.
     *  - Some IVs given: the fewest low-bit flips that reach the type. A stat
     *    the paste named is only touched when no other stat can do it, Speed is
     *    the next most protected, and ties go to the pattern nearest the
     *    standard spread. A flip moves a value by one (31 to 30, 0 to 1), so
     *    "0 Atk" stays a minimum Attack set.
     */
    private fun applyHiddenPowerIvs(moves: List<String>, ivs: IntArray, given: BooleanArray) {
        val wanted = moves.map { namedHiddenPowerType(it) }.firstOrNull { it >= 0 } ?: return

        var oddMask = 0
        for (i in 0 until 6) {
            ivs[i] = ivs[i].coerceIn(0, 31)
            if (ivs[i] and 1 != 0) {
                oddMask = oddMask or (1 shl i)
            }
        }
        if (hiddenPowerTypeOfOddMask(oddMask) == wanted) {
            return
        }

        val standardOddMask = 0b111111 and HP_STANDARD_EVEN_MASK[wanted].inv()
        var target = standardOddMask
        if (given.any { it }) {
            val spe = 5
            var bestCost = -1
            for (candidate in 0 until 64) {
                if (hiddenPowerTypeOfOddMask(candidate) != wanted) {
                    continue
                }
                val flips = candidate xor oddMask
                var cost = 0
                for (i in 0 until 6) {
                    if (flips and (1 shl i) != 0) {
                        cost += if (given[i]) 1000 else if (i == spe) 30 else 10
                    }
                }
                cost += Integer.bitCount(candidate xor standardOddMask)
                if (bestCost < 0 || cost < bestCost) {
                    bestCost = cost
                    target = candidate
                }
            }
        }

        for (i in 0 until 6) {
            if (((target xor oddMask) shr i) and 1 != 0) {
                ivs[i] = ivs[i] xor 1
            }
        }
    }
}
