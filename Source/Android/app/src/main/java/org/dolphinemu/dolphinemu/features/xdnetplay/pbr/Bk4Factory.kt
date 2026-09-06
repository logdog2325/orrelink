// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.pbr

import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.ShowdownSet

/**
 * Builds a [Bk4Mon] from a parsed Showdown set — the Gen 4 counterpart of the
 * XD editor's [org.dolphinemu.dolphinemu.features.xdnetplay.gen3.MonFactory].
 * The parser itself ([org.dolphinemu.dolphinemu.features.xdnetplay.gen3.ShowdownParser])
 * is format-agnostic and is reused as-is.
 *
 * Defaults, chosen so an imported Pokémon looks like something the games could
 * have produced:
 *  - language 2 (English), friendship 255, Poké Ball, OT gender male
 *  - version 12 (Diamond) as the game of origin, fateful encounter off
 *  - met level = current level, met date 2007-04-25 (PBR's own release day —
 *    a fixed, obviously-deliberate date rather than a fake plausible one)
 *  - PP 40 per non-empty move slot with no PP Ups; the games clamp PP on use
 *    and the asset carries no per-move base-PP table
 *  - the profile's own OT name / TID / SID, so the team reads as the player's
 *
 * The PID is chosen deterministically to satisfy, in priority order: nature
 * (unsigned PID % 25), ability slot (PID and 1), gender (PID low byte against
 * the species gender ratio) and shininess ((TID^SID^PIDhi^PIDlo) < 8). Shiny is
 * relaxed first when the combination is unsatisfiable; the search is bounded.
 */
object Bk4Factory {
    private const val LANGUAGE_ENGLISH = 2
    private const val DEFAULT_FRIENDSHIP = 255
    private const val DEFAULT_PP = 40
    private const val BALL_POKE = 4
    private const val OT_GENDER_MALE = 0
    private const val VERSION_DIAMOND = 12

    // PBR's Japanese release date, used as a fixed met date.
    private const val MET_YEAR = 7   // 2000 + 7
    private const val MET_MONTH = 4
    private const val MET_DAY = 25

    private const val RATIO_ALWAYS_MALE = 0
    private const val RATIO_ALWAYS_FEMALE = 254
    private const val RATIO_GENDERLESS = 255

    const val GENDER_MALE = 0
    const val GENDER_FEMALE = 1
    const val GENDER_GENDERLESS = 2

    /**
     * @param trainerName OT name to stamp, in the Gen 4 glyph charset.
     * @throws IllegalArgumentException for unknown species/move/item/ability/nature.
     */
    fun build(
        set: ShowdownSet,
        data: Gen4Data,
        trainerName: String,
        tid: Int,
        sid: Int
    ): Bk4Mon {
        val species = data.species(set.species)
            ?: throw IllegalArgumentException("unknown species: ${set.species}")

        val level = set.level.coerceIn(1, 100)

        val nature = if (set.nature != null) {
            data.nature(set.nature)
                ?: throw IllegalArgumentException("unknown nature: ${set.nature}")
        } else {
            data.nature("hardy") ?: Gen4Data.Nature("hardy", "Hardy", 0, -1, -1)
        }

        val abilityId = if (set.ability != null) {
            data.abilityId(set.ability)
                ?: throw IllegalArgumentException("unknown ability: ${set.ability}")
        } else {
            species.abilities[0]
        }
        val abilitySlot = abilitySlotFor(species, abilityId)

        val pid = choosePid(
            natureId = nature.id,
            abilitySlot = abilitySlot,
            genderRatio = species.genderRatio,
            wantedGender = set.gender,
            shiny = set.shiny,
            tid = tid,
            sid = sid
        )

        val mon = Bk4Mon.blank()
        mon.pid = pid
        mon.species = species.id
        mon.tid = tid and 0xFFFF
        mon.sid = sid and 0xFFFF
        mon.heldItem = if (set.item != null) {
            data.itemId(set.item)
                ?: throw IllegalArgumentException("unknown item: ${set.item}")
        } else {
            0
        }
        mon.exp = Gen4Growth.expFor(level, species.id)
        mon.friendship = set.happiness ?: DEFAULT_FRIENDSHIP
        mon.ability = abilityId
        mon.language = LANGUAGE_ENGLISH

        val evs = IntArray(6) { set.evs[it].coerceIn(0, 255) }
        val ivs = IntArray(6) { set.ivs[it].coerceIn(0, 31) }
        mon.evs = evs
        mon.ivs = ivs

        val moveIds = IntArray(4)
        for (i in 0 until minOf(set.moves.size, 4)) {
            moveIds[i] = resolveMoveId(set.moves[i], data)
        }
        mon.moves = moveIds
        mon.movePp = IntArray(4) { if (moveIds[it] != 0) DEFAULT_PP else 0 }
        mon.movePpUps = IntArray(4)

        mon.genderRaw = genderFor(species.genderRatio, pid)
        val nickname = sanitizeName(set.nickname ?: species.name, Bk4Mon.NICKNAME_CHARS)
        mon.nickname = nickname.ifEmpty { "?" }
        mon.isNicknamed = set.nickname != null
        mon.otName = sanitizeName(trainerName, Bk4Mon.OT_NAME_CHARS).ifEmpty { "PBR" }
        mon.otGender = OT_GENDER_MALE
        mon.metLevel = level
        mon.metYear = MET_YEAR
        mon.metMonth = MET_MONTH
        mon.metDay = MET_DAY
        mon.ball = BALL_POKE
        mon.version = VERSION_DIAMOND
        mon.refreshChecksum()
        return mon
    }

    /**
     * Ability slot 1 only when [abilityId] is the species' *distinct* second
     * ability. PersonalInfo4 repeats ability 1 in the second field for species
     * that have only one, so a plain equality test would pick slot 1 wrongly.
     */
    fun abilitySlotFor(species: Gen4Data.Species, abilityId: Int): Int =
        if (abilityId == species.abilities[1] &&
            species.abilities[1] != 0 &&
            species.abilities[1] != species.abilities[0]
        ) 1 else 0

    /** BK4 gender nibble for a PID, given the species' gender ratio. */
    fun genderFor(genderRatio: Int, pid: Int): Int = when (genderRatio) {
        RATIO_GENDERLESS -> GENDER_GENDERLESS
        RATIO_ALWAYS_FEMALE -> GENDER_FEMALE
        RATIO_ALWAYS_MALE -> GENDER_MALE
        else -> if ((pid and 0xFF) < genderRatio) GENDER_FEMALE else GENDER_MALE
    }

    /**
     * "Hidden Power [Ice]" / "Hidden Power Ice" resolve to plain Hidden Power;
     * Gen 4 stores no Hidden Power type, it derives from the IVs.
     */
    private fun resolveMoveId(name: String, data: Gen4Data): Int {
        data.moveId(name)?.let { return it }
        if (Gen4Data.normalizeName(name).startsWith("hiddenpower")) {
            data.moveId("hiddenpower")?.let { return it }
        }
        throw IllegalArgumentException("unknown move: $name")
    }

    /**
     * Make [text] encodable in the Gen 4 glyph table: straight quotes become the
     * charset's curly forms, anything still unencodable is dropped.
     */
    fun sanitizeName(text: String, maxChars: Int): String {
        val sb = StringBuilder(text.length)
        for (c in text) {
            val mapped = when (c) {
                '\'' -> '’'
                '"' -> '”'
                else -> c
            }
            if (Gen4Text.canEncodeGlyph(mapped)) {
                sb.append(mapped)
            }
            if (sb.length == maxChars) {
                break
            }
        }
        return sb.toString()
    }

    // -- PID selection --------------------------------------------------------

    fun isShinyPid(pid: Int, tid: Int, sid: Int): Boolean =
        ((tid and 0xFFFF) xor (sid and 0xFFFF) xor
            ((pid ushr 16) and 0xFFFF) xor (pid and 0xFFFF)) < Bk4Mon.SHINY_THRESHOLD

    private fun genderOk(pidLowByte: Int, genderRatio: Int, wantedGender: Char?): Boolean {
        if (genderRatio == RATIO_GENDERLESS ||
            genderRatio == RATIO_ALWAYS_FEMALE ||
            genderRatio == RATIO_ALWAYS_MALE
        ) {
            return true // fixed by the species, the PID cannot change it
        }
        return when (wantedGender) {
            'F' -> pidLowByte < genderRatio
            'M' -> pidLowByte >= genderRatio
            else -> true
        }
    }

    /**
     * Deterministic PID search, same shape as the Gen 3 editor's.
     *
     * Shiny: enumerate the low u16 (parity = ability slot, low byte satisfying
     * gender), derive the eight shiny high u16 values, take the first with the
     * right nature. Otherwise: start from a nature-aligned base and step by 25,
     * which walks the low u16 through every value (gcd(25, 2^16) = 1), until
     * parity and gender match and the PID is not accidentally shiny.
     */
    fun choosePid(
        natureId: Int,
        abilitySlot: Int,
        genderRatio: Int,
        wantedGender: Char?,
        shiny: Boolean,
        tid: Int,
        sid: Int
    ): Int {
        val t = tid and 0xFFFF
        val s = sid and 0xFFFF

        if (shiny) {
            for (low in 0..0xFFFF) {
                if ((low and 1) != abilitySlot) {
                    continue
                }
                if (!genderOk(low and 0xFF, genderRatio, wantedGender)) {
                    continue
                }
                for (r in 0 until Bk4Mon.SHINY_THRESHOLD) {
                    val high = (t xor s xor low xor r) and 0xFFFF
                    val pid = (high shl 16) or low
                    if (PbrBytes.unsignedMod(pid, Bk4Mon.NATURE_COUNT) == natureId) {
                        return pid
                    }
                }
            }
            // Nothing shiny satisfies nature+ability+gender: relax shininess.
        }

        val base = 0x12345678
        var pid = base - (base % Bk4Mon.NATURE_COUNT) + natureId
        for (step in 0 until 0x20000) {
            if ((pid and 1) == abilitySlot &&
                genderOk(pid and 0xFF, genderRatio, wantedGender) &&
                !isShinyPid(pid, t, s)
            ) {
                return pid
            }
            pid += Bk4Mon.NATURE_COUNT
        }
        throw IllegalStateException("no PID satisfies the requested constraints")
    }
}
