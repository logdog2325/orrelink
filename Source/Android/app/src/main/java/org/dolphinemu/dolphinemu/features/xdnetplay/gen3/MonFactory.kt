// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.gen3

/**
 * Builds a Gen3Mon from a parsed Showdown set, mirroring the defaults the
 * validated Python injector (gen3save.py make_party_mon) used for its
 * in-game-verified injected mon:
 *
 *  - language English (0x0202), friendship 70
 *  - origins: met level = current level, game of origin 3 (Emerald),
 *    ball 4 (Poke Ball), OT gender male, met location 0
 *  - PP = 35 for every non-empty move slot, 0 otherwise, PP bonuses 0
 *    (the Python used exactly this constant; there is no per-move base-PP
 *    table in the data JSON, and the game clamps PP on use)
 *  - contest stats, ribbons, status, pokerus all 0
 *
 * The PID is chosen deterministically to satisfy, in priority order:
 * nature (unsigned PID % 25), ability slot (PID & 1), gender (PID low byte
 * vs the species gender ratio, when the set requests a gender the species
 * can actually have), and shininess ((TID^SID^PIDhigh^PIDlow) < 8). Shiny is
 * relaxed first if the combination is unsatisfiable; the search is bounded,
 * never infinite.
 */
object MonFactory {
    private const val LANGUAGE_ENGLISH = 0x0202
    private const val DEFAULT_FRIENDSHIP = 70
    private const val DEFAULT_PP = 35        // mirrors the Python injector
    private const val GAME_EMERALD = 3
    private const val BALL_POKE = 4          // the Python injector's default ball
    private const val OT_GENDER_MALE = 0
    private const val SHINY_THRESHOLD = 8

    // Gender ratio sentinel values (never PID-dependent).
    private const val RATIO_ALWAYS_MALE = 0
    private const val RATIO_ALWAYS_FEMALE = 254
    private const val RATIO_GENDERLESS = 255

    /**
     * Build a party-ready [Gen3Mon] owned by [trainerName]/[trainerId]
     * (raw u32: low u16 public ID, high u16 secret ID, as read from the save).
     *
     * @throws IllegalArgumentException for unknown species/move/item/ability/
     * nature names.
     */
    fun build(set: ShowdownSet, data: Gen3Data, trainerName: String, trainerId: Int): Gen3Mon {
        val species = data.species(set.species)
            ?: throw IllegalArgumentException("unknown species: ${set.species}")

        val level = set.level.coerceIn(1, 100)

        // Nature: default Hardy (neutral) when the set omits it, so the
        // stats we store match the nature the PID encodes.
        val nature = if (set.nature != null) {
            data.nature(set.nature)
                ?: throw IllegalArgumentException("unknown nature: ${set.nature}")
        } else {
            data.nature("hardy") ?: Gen3Data.Nature("hardy", 0, -1, -1)
        }

        // Ability slot: 1 only when the requested ability is the species'
        // distinct second ability; otherwise 0.
        val abilitySlot = if (set.ability != null) {
            val abilityId = data.abilityId(set.ability)
                ?: throw IllegalArgumentException("unknown ability: ${set.ability}")
            if (abilityId == species.abilities[1] &&
                species.abilities[1] != 0 &&
                species.abilities[1] != species.abilities[0]
            ) 1 else 0
        } else {
            0
        }

        val pid = choosePid(
            natureId = nature.id,
            abilitySlot = abilitySlot,
            genderRatio = species.genderRatio,
            wantedGender = set.gender,
            shiny = set.shiny,
            trainerId = trainerId
        )

        // Remap hp/atk/def/spa/spd/spe (Showdown order) to
        // hp/atk/def/speed/spatk/spdef (Gen 3 save order).
        val evs = showdownToGen3Order(set.evs)
        val ivs = showdownToGen3Order(set.ivs)
        for (i in 0..5) {
            ivs[i] = ivs[i].coerceIn(0, 31)
            evs[i] = evs[i].coerceIn(0, 255)
        }

        val moveIds = IntArray(4)
        for (i in 0 until minOf(set.moves.size, 4)) {
            moveIds[i] = resolveMoveId(set.moves[i], data)
        }

        val heldItem = if (set.item != null) {
            data.itemId(set.item)
                ?: throw IllegalArgumentException("unknown item: ${set.item}")
        } else {
            0
        }

        val stats = computeStats(species, level, ivs, evs, nature)

        val mon = Gen3Mon()
        mon.pid = pid
        mon.otId = trainerId
        mon.nicknameRaw = Gen3Text.encode(
            sanitizeText(set.nickname ?: set.species.uppercase()), 10
        )
        mon.language = LANGUAGE_ENGLISH
        mon.otNameRaw = Gen3Text.encode(sanitizeText(trainerName), 7)
        mon.species = species.id
        mon.heldItem = heldItem
        mon.experience = data.expForLevel(species.expGroup, level)
        mon.ppBonuses = 0
        mon.friendship = DEFAULT_FRIENDSHIP
        mon.moves = moveIds
        for (i in 0..3) {
            mon.pp[i] = if (moveIds[i] != 0) DEFAULT_PP else 0
        }
        mon.evs = evs
        mon.metLocation = 0
        mon.origins = (level and 0x7F) or
            ((GAME_EMERALD and 0xF) shl 7) or
            ((BALL_POKE and 0xF) shl 11) or
            ((OT_GENDER_MALE and 1) shl 15)
        mon.setIvs(ivs)
        mon.setAbilitySlot(abilitySlot)
        mon.level = level
        mon.maxHp = stats[0]
        mon.currentHp = stats[0]
        mon.attack = stats[1]
        mon.defense = stats[2]
        mon.speed = stats[3]
        mon.spAttack = stats[4]
        mon.spDefense = stats[5]
        return mon
    }

    /** Remap hp/atk/def/spa/spd/spe -> hp/atk/def/speed/spatk/spdef. */
    private fun showdownToGen3Order(sd: IntArray): IntArray =
        intArrayOf(sd[0], sd[1], sd[2], sd[5], sd[3], sd[4])

    /**
     * Move name -> id. "Hidden Power [Ice]" / "Hidden Power Ice" resolve to
     * plain Hidden Power (Gen 3 stores no HP type; it derives from IVs).
     */
    private fun resolveMoveId(name: String, data: Gen3Data): Int {
        data.moveId(name)?.let { return it }
        val normalized = Gen3Data.normalizeName(name)
        if (normalized.startsWith("hiddenpower")) {
            data.moveId("hiddenpower")?.let { return it }
        }
        throw IllegalArgumentException("unknown move: $name")
    }

    /**
     * Make [text] Gen 3-encodable: straight ASCII quotes become the charset's
     * curly forms, anything else unencodable is dropped. (The implementation
     * moved to Gen3Text so the save's trainer-name setter shares exactly this
     * behaviour.)
     */
    private fun sanitizeText(text: String): String = Gen3Text.sanitize(text)

    // -- stats ---------------------------------------------------------------

    /**
     * Gen 3 stat formula, integer division exactly as the game (and Python //):
     *   HP     = (2*base + IV + EV/4) * level / 100 + level + 10   (Shedinja: 1)
     *   others = ((2*base + IV + EV/4) * level / 100 + 5) * nature
     * where the nature multiplier is *110/100 or *90/100 (integer division),
     * with nature plus/minus indices 0=atk 1=def 2=speed 3=spatk 4=spdef.
     * Arrays are in Gen 3 order: hp, atk, def, speed, spatk, spdef.
     */
    private fun computeStats(
        species: Gen3Data.Species,
        level: Int,
        ivs: IntArray,
        evs: IntArray,
        nature: Gen3Data.Nature
    ): IntArray {
        val out = IntArray(6)
        out[0] = if (species.name == "shedinja") {
            1
        } else {
            (2 * species.baseStats[0] + ivs[0] + evs[0] / 4) * level / 100 + level + 10
        }
        for (i in 1..5) {
            var v = (2 * species.baseStats[i] + ivs[i] + evs[i] / 4) * level / 100 + 5
            val statIndex = i - 1  // nature indices skip HP
            if (nature.plus == statIndex && nature.minus != statIndex) {
                v = v * 110 / 100
            } else if (nature.minus == statIndex && nature.plus != statIndex) {
                v = v * 90 / 100
            }
            out[i] = v
        }
        return out
    }

    // -- PID selection -------------------------------------------------------

    private fun isShinyPid(pid: Int, trainerId: Int): Boolean {
        val tid = trainerId and 0xFFFF
        val sid = (trainerId ushr 16) and 0xFFFF
        val pidLow = pid and 0xFFFF
        val pidHigh = (pid ushr 16) and 0xFFFF
        return (tid xor sid xor pidHigh xor pidLow) < SHINY_THRESHOLD
    }

    private fun genderOk(pidLowByte: Int, genderRatio: Int, wantedGender: Char?): Boolean {
        if (genderRatio == RATIO_GENDERLESS ||
            genderRatio == RATIO_ALWAYS_FEMALE ||
            genderRatio == RATIO_ALWAYS_MALE
        ) {
            // Gender is fixed by the species regardless of PID.
            return true
        }
        return when (wantedGender) {
            'F' -> pidLowByte < genderRatio   // female iff (PID & 0xFF) < ratio
            'M' -> pidLowByte >= genderRatio
            else -> true
        }
    }

    /**
     * Deterministic PID search. For shiny requests: enumerate the low u16
     * (parity = ability slot, low byte satisfying gender), derive the eight
     * shiny high u16 values, and take the first PID with the right nature.
     * Non-shiny (or shiny-unsatisfiable, relaxed): start from the Python
     * reference's nature-preserving base (0x12345678 rounded to the nature)
     * and step by 25, which cycles the low u16 through every value, until
     * parity + gender match and the PID is not accidentally shiny.
     */
    private fun choosePid(
        natureId: Int,
        abilitySlot: Int,
        genderRatio: Int,
        wantedGender: Char?,
        shiny: Boolean,
        trainerId: Int
    ): Int {
        val tid = trainerId and 0xFFFF
        val sid = (trainerId ushr 16) and 0xFFFF

        if (shiny) {
            for (low in 0..0xFFFF) {
                if ((low and 1) != abilitySlot) {
                    continue
                }
                if (!genderOk(low and 0xFF, genderRatio, wantedGender)) {
                    continue
                }
                for (r in 0 until SHINY_THRESHOLD) {
                    val high = (tid xor sid xor low xor r) and 0xFFFF
                    val pid = (high shl 16) or low
                    if (Gen3Mon.unsignedMod(pid, 25) == natureId) {
                        return pid
                    }
                }
            }
            // No shiny PID satisfies nature+ability+gender: relax shiny.
        }

        // Nature-preserving scan (mirrors find_pid_for_nature's base). No
        // overflow: base + 25 * 0x20000 stays far below 2^31.
        val base = 0x12345678
        var pid = base - (base % 25) + natureId
        for (step in 0 until 0x20000) {
            if ((pid and 1) == abilitySlot &&
                genderOk(pid and 0xFF, genderRatio, wantedGender) &&
                !isShinyPid(pid, trainerId)
            ) {
                return pid
            }
            pid += 25
        }
        // Unreachable: the low u16 cycles through all values (gcd(25, 2^16)=1),
        // so parity+gender always hit, and shininess excludes at most 8/65536.
        throw IllegalStateException("no PID satisfies the requested constraints")
    }
}
