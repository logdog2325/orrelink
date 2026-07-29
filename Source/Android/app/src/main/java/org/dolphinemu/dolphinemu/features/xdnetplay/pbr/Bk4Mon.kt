// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.pbr

/**
 * A BK4 record — the 136-byte big-endian Gen 4 Pokémon entity Pokémon Battle
 * Revolution stores in its party and boxes.
 *
 * Ported from the Python reference that was proven against a real 3.5 MB
 * PbrSaveData (1638 Pokémon parsed, 1635 entity checksums valid — the three
 * failures are records literally nicknamed "BAD EGG"). Cited offsets are
 * PKHeX BK4.cs / PokeCrypto.cs at commit 757a297f1463.
 *
 * Three things differ from the more familiar PK4, and getting any of them wrong
 * silently produces garbage:
 *  1. There is NO XOR cipher at rest. [decode] only un-shuffles the four
 *     32-byte blocks (PokeCrypto.Decrypt4BE); PK4's PRNG XOR layer is absent.
 *  2. TID and SID are SWAPPED relative to PK4: SID lives at 0x0C, TID at 0x0E.
 *  3. The IV/flag u32 bit layout is MIRRORED relative to PK4 (see [ivs]).
 *
 * This class owns the *decrypted* 136 bytes verbatim and edits them in place,
 * so every field it does not model — contest stats, ribbons, met location,
 * shiny leaf, the lot — survives a load/save cycle untouched.
 */
class Bk4Mon private constructor(
    /** The decrypted (un-shuffled) 136-byte record. Mutated in place by setters. */
    val data: ByteArray
) {
    init {
        require(data.size == SIZE_STORED) { "BK4 record must be $SIZE_STORED bytes" }
    }

    fun copy(): Bk4Mon = Bk4Mon(data.copyOf())

    // -- header (bytes 0x00-0x07 are never shuffled) --------------------------

    /** Personality value; raw u32 bit pattern, may be negative as an Int. */
    var pid: Int
        get() = PbrBytes.readU32BE(data, 0x00)
        set(value) = PbrBytes.writeU32BE(data, 0x00, value)

    val sanity: Int get() = PbrBytes.readU16BE(data, 0x04)

    val storedChecksum: Int get() = PbrBytes.readU16BE(data, 0x06)

    // -- block A --------------------------------------------------------------

    var species: Int
        get() = PbrBytes.readU16BE(data, 0x08)
        set(value) = PbrBytes.writeU16BE(data, 0x08, value)

    var heldItem: Int
        get() = PbrBytes.readU16BE(data, 0x0A)
        set(value) = PbrBytes.writeU16BE(data, 0x0A, value)

    /** BK4.cs:66 — secret ID, at 0x0C (PK4 has the trainer ID here). */
    var sid: Int
        get() = PbrBytes.readU16BE(data, 0x0C)
        set(value) = PbrBytes.writeU16BE(data, 0x0C, value)

    /** BK4.cs:67 — visible trainer ID, at 0x0E. */
    var tid: Int
        get() = PbrBytes.readU16BE(data, 0x0E)
        set(value) = PbrBytes.writeU16BE(data, 0x0E, value)

    var exp: Long
        get() = PbrBytes.readU32LongBE(data, 0x10)
        set(value) = PbrBytes.writeU32BE(data, 0x10, value)

    var friendship: Int
        get() = PbrBytes.readU8(data, 0x14)
        set(value) = PbrBytes.writeU8(data, 0x14, value)

    /** Stored as the ability ID itself, not as a slot index. */
    var ability: Int
        get() = PbrBytes.readU8(data, 0x15)
        set(value) = PbrBytes.writeU8(data, 0x15, value)

    var language: Int
        get() = PbrBytes.readU8(data, 0x17)
        set(value) = PbrBytes.writeU8(data, 0x17, value)

    /**
     * EVs in Showdown order (hp, atk, def, spa, spd, spe).
     * In memory (BK4.cs:79-84) they are HP, ATK, DEF, SPE, SPA, SPD — note SPE
     * sits third-from-last, so a straight copy would swap Speed with SpA/SpD.
     */
    var evs: IntArray
        get() = intArrayOf(
            PbrBytes.readU8(data, 0x18), // hp
            PbrBytes.readU8(data, 0x19), // atk
            PbrBytes.readU8(data, 0x1A), // def
            PbrBytes.readU8(data, 0x1C), // spa
            PbrBytes.readU8(data, 0x1D), // spd
            PbrBytes.readU8(data, 0x1B)  // spe
        )
        set(value) {
            require(value.size == 6) { "EV array must have 6 entries" }
            PbrBytes.writeU8(data, 0x18, value[0])
            PbrBytes.writeU8(data, 0x19, value[1])
            PbrBytes.writeU8(data, 0x1A, value[2])
            PbrBytes.writeU8(data, 0x1B, value[5])
            PbrBytes.writeU8(data, 0x1C, value[3])
            PbrBytes.writeU8(data, 0x1D, value[4])
        }

    // -- block B --------------------------------------------------------------

    var moves: IntArray
        get() = IntArray(4) { PbrBytes.readU16BE(data, 0x28 + it * 2) }
        set(value) {
            require(value.size == 4) { "move array must have 4 entries" }
            for (i in 0 until 4) {
                PbrBytes.writeU16BE(data, 0x28 + i * 2, value[i])
            }
        }

    var movePp: IntArray
        get() = IntArray(4) { PbrBytes.readU8(data, 0x30 + it) }
        set(value) {
            require(value.size == 4) { "PP array must have 4 entries" }
            for (i in 0 until 4) {
                PbrBytes.writeU8(data, 0x30 + i, value[i])
            }
        }

    var movePpUps: IntArray
        get() = IntArray(4) { PbrBytes.readU8(data, 0x34 + it) }
        set(value) {
            require(value.size == 4) { "PP-up array must have 4 entries" }
            for (i in 0 until 4) {
                PbrBytes.writeU8(data, 0x34 + i, value[i])
            }
        }

    var iv32: Int
        get() = PbrBytes.readU32BE(data, 0x38)
        set(value) = PbrBytes.writeU32BE(data, 0x38, value)

    /**
     * IVs in Showdown order (hp, atk, def, spa, spd, spe).
     *
     * BK4.cs:148-155 — the bit layout is MIRRORED relative to PK4:
     * bit 0 nicknamed, bit 1 egg, SpD >> 2, SpA >> 7, Spe >> 12, Def >> 17,
     * Atk >> 22, HP >> 27.
     */
    var ivs: IntArray
        get() {
            val v = iv32
            return intArrayOf(
                (v ushr 27) and 0x1F, // hp
                (v ushr 22) and 0x1F, // atk
                (v ushr 17) and 0x1F, // def
                (v ushr 7) and 0x1F,  // spa
                (v ushr 2) and 0x1F,  // spd
                (v ushr 12) and 0x1F  // spe
            )
        }
        set(value) {
            require(value.size == 6) { "IV array must have 6 entries" }
            var v = iv32 and 0x3         // keep bit 0 (nicknamed) and bit 1 (egg)
            v = v or ((value[4] and 0x1F) shl 2)
            v = v or ((value[3] and 0x1F) shl 7)
            v = v or ((value[5] and 0x1F) shl 12)
            v = v or ((value[2] and 0x1F) shl 17)
            v = v or ((value[1] and 0x1F) shl 22)
            v = v or ((value[0] and 0x1F) shl 27)
            iv32 = v
        }

    var isNicknamed: Boolean
        get() = (iv32 and 1) != 0
        set(value) {
            iv32 = if (value) iv32 or 1 else iv32 and 1.inv()
        }

    val isEgg: Boolean get() = ((iv32 ushr 1) and 1) != 0

    val isFateful: Boolean get() = (PbrBytes.readU8(data, 0x40) and 0x80) != 0

    /** 0 = male, 1 = female, 2 = genderless (BK4.cs:195). */
    var genderRaw: Int
        get() = (PbrBytes.readU8(data, 0x40) ushr 5) and 3
        set(value) {
            val b = PbrBytes.readU8(data, 0x40)
            PbrBytes.writeU8(data, 0x40, (b and 0x9F) or ((value and 3) shl 5))
        }

    val form: Int get() = PbrBytes.readU8(data, 0x40) and 0x1F

    // -- block C --------------------------------------------------------------

    var nickname: String
        get() = Gen4Text.decodeGlyphs(data, 0x48, NICKNAME_BYTES)
        set(value) {
            Gen4Text.encodeGlyphs(value, NICKNAME_BYTES, NICKNAME_CHARS)
                .copyInto(data, 0x48)
        }

    var version: Int
        get() = PbrBytes.readU8(data, 0x5F)
        set(value) = PbrBytes.writeU8(data, 0x5F, value)

    // -- block D --------------------------------------------------------------

    var otName: String
        get() = Gen4Text.decodeGlyphs(data, 0x68, OT_NAME_BYTES)
        set(value) {
            Gen4Text.encodeGlyphs(value, OT_NAME_BYTES, OT_NAME_CHARS)
                .copyInto(data, 0x68)
        }

    var metYear: Int
        get() = PbrBytes.readU8(data, 0x7B)
        set(value) = PbrBytes.writeU8(data, 0x7B, value)

    var metMonth: Int
        get() = PbrBytes.readU8(data, 0x7C)
        set(value) = PbrBytes.writeU8(data, 0x7C, value)

    var metDay: Int
        get() = PbrBytes.readU8(data, 0x7D)
        set(value) = PbrBytes.writeU8(data, 0x7D, value)

    /** Poké Ball, DP/Pt numbering (BK4.cs:290). */
    var ball: Int
        get() = PbrBytes.readU8(data, 0x83)
        set(value) = PbrBytes.writeU8(data, 0x83, value)

    var metLevel: Int
        get() = PbrBytes.readU8(data, 0x84) ushr 1
        set(value) {
            PbrBytes.writeU8(data, 0x84, ((value and 0x7F) shl 1) or otGender)
        }

    var otGender: Int
        get() = PbrBytes.readU8(data, 0x84) and 1
        set(value) {
            PbrBytes.writeU8(data, 0x84, (PbrBytes.readU8(data, 0x84) and 0xFE) or (value and 1))
        }

    // -- derived --------------------------------------------------------------

    val level: Int get() = Gen4Growth.levelFor(exp, species)

    val nature: Int get() = PbrBytes.unsignedMod(pid, NATURE_COUNT)

    val isShiny: Boolean
        get() {
            val p = pid
            return (tid xor sid xor ((p ushr 16) and 0xFFFF) xor (p and 0xFFFF)) < SHINY_THRESHOLD
        }

    fun computeChecksum(): Int {
        var total = 0
        var i = 8
        while (i < SIZE_STORED) {
            total = (total + PbrBytes.readU16BE(data, i)) and 0xFFFF
            i += 2
        }
        return total
    }

    val isChecksumValid: Boolean get() = storedChecksum == computeChecksum()

    fun refreshChecksum() {
        PbrBytes.writeU16BE(data, 0x06, computeChecksum())
    }

    /**
     * Re-shuffle into the 136 stored bytes, refreshing the entity checksum
     * first. The checksum is shuffle-invariant (it is a sum over the whole
     * body), so the order of the two steps does not matter — but refreshing
     * before writing keeps the invariant obvious.
     */
    fun encode(): ByteArray {
        refreshChecksum()
        val out = ByteArray(SIZE_STORED)
        data.copyInto(out, 0, 0, 8)
        val sv = BLOCK_POSITION_INVERT[(pid ushr 13) and 31]
        gatherBlocks(data, out, sv)
        return out
    }

    override fun toString(): String =
        "Bk4Mon(species=$species lv=$level nick=$nickname nature=$nature " +
            "item=$heldItem ability=$ability moves=${moves.toList()} " +
            "evs=${evs.toList()} ivs=${ivs.toList()} shiny=$isShiny)"

    companion object {
        /** PokeCrypto.SIZE_4STORED. */
        const val SIZE_STORED = 136
        const val SIZE_BLOCK = 32
        const val NATURE_COUNT = 25
        const val SHINY_THRESHOLD = 8

        const val NICKNAME_BYTES = 22
        const val NICKNAME_CHARS = 10
        const val OT_NAME_BYTES = 16
        const val OT_NAME_CHARS = 7

        /** Highest species index a Gen 4 record can legitimately hold. */
        const val MAX_SPECIES = 493

        /**
         * PokeCrypto.cs:61-88 BlockPosition. Row (pv >> 13) & 31 gives, for each
         * output block, which input block to take. Rows 24..31 duplicate rows
         * 0..7 so the &31 needs no modulus — proven equivalent to %24.
         */
        private val BLOCK_POSITION = intArrayOf(
            0, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2,
            0, 2, 3, 1, 0, 3, 2, 1, 1, 0, 2, 3, 1, 0, 3, 2,
            2, 0, 1, 3, 3, 0, 1, 2, 2, 0, 3, 1, 3, 0, 2, 1,
            1, 2, 0, 3, 1, 3, 0, 2, 2, 1, 0, 3, 3, 1, 0, 2,
            2, 3, 0, 1, 3, 2, 0, 1, 1, 2, 3, 0, 1, 3, 2, 0,
            2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1, 0,
            0, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2,
            0, 2, 3, 1, 0, 3, 2, 1, 1, 0, 2, 3, 1, 0, 3, 2
        )

        /** PokeCrypto.cs BlockPositionInvert — the row that undoes row i. */
        private val BLOCK_POSITION_INVERT = intArrayOf(
            0, 1, 2, 4, 3, 5, 6, 7,
            12, 18, 13, 19, 8, 10, 14, 20,
            16, 22, 9, 11, 15, 21, 17, 23,
            0, 1, 2, 4, 3, 5, 6, 7
        )

        /** out[0x08 + 32*i] = src[0x08 + 32*BLOCK_POSITION[row*4 + i]]. */
        private fun gatherBlocks(src: ByteArray, dst: ByteArray, row: Int) {
            for (i in 0 until 4) {
                val from = 8 + BLOCK_POSITION[row * 4 + i] * SIZE_BLOCK
                src.copyInto(dst, 8 + i * SIZE_BLOCK, from, from + SIZE_BLOCK)
            }
        }

        fun isEmptySlot(buf: ByteArray, off: Int): Boolean {
            for (i in 0 until SIZE_STORED) {
                if (buf[off + i].toInt() != 0) {
                    return false
                }
            }
            return true
        }

        /**
         * Un-shuffle the stored record at [off] (PokeCrypto.cs:132-141
         * Decrypt4BE). Returns null for an empty slot or a record whose species
         * is not a Gen 4 species — the same "do not pretend to understand it"
         * guard the Python reference used.
         */
        fun decode(buf: ByteArray, off: Int): Bk4Mon? {
            if (off < 0 || off + SIZE_STORED > buf.size || isEmptySlot(buf, off)) {
                return null
            }
            val dec = ByteArray(SIZE_STORED)
            buf.copyInto(dec, 0, off, off + 8)
            val pv = PbrBytes.readU32BE(buf, off)
            val src = ByteArray(SIZE_STORED)
            buf.copyInto(src, 0, off, off + SIZE_STORED)
            gatherBlocks(src, dec, (pv ushr 13) and 31)
            val mon = Bk4Mon(dec)
            if (mon.species <= 0 || mon.species > MAX_SPECIES) {
                return null
            }
            return mon
        }

        /** Build an empty record whose bytes the caller then fills in. */
        fun blank(): Bk4Mon = Bk4Mon(ByteArray(SIZE_STORED))
    }
}
