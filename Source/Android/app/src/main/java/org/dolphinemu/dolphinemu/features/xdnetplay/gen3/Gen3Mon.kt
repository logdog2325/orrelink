// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.gen3

/**
 * Fully decoded Gen 3 party Pokemon (100-byte struct).
 *
 * Ported 1:1 from the empirically validated Python reference
 * (gen3save.py PartyPokemon; see VALIDATION.md). fromBytes()/encode()
 * round-trip byte-identically.
 *
 * Box portion (80 bytes):
 *   0x00 u32  personality (PID)
 *   0x04 u32  OT ID (low u16 public, high u16 secret)
 *   0x08 10B  nickname (Gen 3 charset)
 *   0x12 u16  language (0x0202 = English)
 *   0x14 7B   OT name
 *   0x1B u8   markings
 *   0x1C u16  checksum (u16-sum of DECRYPTED 48-byte data block)
 *   0x1E u16  unknown/padding (preserved verbatim)
 *   0x20 48B  encrypted data: four 12-byte substructures, order = PID % 24,
 *             each u32 XORed with key = PID ^ OTID
 * Party-only portion (20 bytes):
 *   0x50 u32 status, 0x54 u8 level, 0x55 u8 pokerus days, 0x56 u16 curHP,
 *   0x58 u16 maxHP, 0x5A atk, 0x5C def, 0x5E spe, 0x60 spa, 0x62 spd
 */
class Gen3Mon {
    // Box header
    var pid = 0                       // raw u32 bit pattern
    var otId = 0                      // raw u32 bit pattern
    var nicknameRaw = ByteArray(10)   // raw bytes (kept for exact round-trip)
    var language = 0x0202
    var otNameRaw = ByteArray(7)
    var markings = 0
    var unknown1E = 0

    // Growth substructure
    var species = 0
    var heldItem = 0
    var experience = 0
    var ppBonuses = 0
    var friendship = 0
    var growthUnknown = 0

    // Attacks substructure
    var moves = IntArray(4)
    var pp = IntArray(4)

    // EVs substructure (+ contest stats)
    var evs = IntArray(6)             // hp, atk, def, speed, spatk, spdef
    var contest = IntArray(6)         // cool, beauty, cute, smart, tough, feel

    // Misc substructure
    var pokerus = 0
    var metLocation = 0
    var origins = 0                   // bits: 0-6 level met, 7-10 game, 11-14 ball, 15 OT gender
    var ivEggAbility = 0              // bits: 6x5-bit IVs, 30 is_egg, 31 ability slot
    var ribbons = 0

    // Party-only portion
    var status = 0
    var level = 0
    var pokerusDays = 0
    var currentHp = 0
    var maxHp = 0
    var attack = 0
    var defense = 0
    var speed = 0
    var spAttack = 0
    var spDefense = 0

    // -- derived accessors ---------------------------------------------------

    val nickname: String
        get() = Gen3Text.decode(nicknameRaw, 0, 10)

    val otName: String
        get() = Gen3Text.decode(otNameRaw, 0, 7)

    /** Nature index 0..24 = unsigned PID % 25. */
    val nature: Int
        get() = unsignedMod(pid, 25)

    val natureName: String
        get() = NATURES[nature]

    /** IVs as (hp, atk, def, speed, spatk, spdef), 5 bits each. */
    fun ivs(): IntArray {
        val out = IntArray(6)
        for (i in 0..5) {
            out[i] = (ivEggAbility ushr (5 * i)) and 0x1F
        }
        return out
    }

    /** Set IVs from (hp, atk, def, speed, spatk, spdef). */
    fun setIvs(ivs: IntArray) {
        var v = ivEggAbility and 0x3FFFFFFF.inv()
        for (i in 0..5) {
            v = v or ((ivs[i] and 0x1F) shl (5 * i))
        }
        ivEggAbility = v
    }

    val isEgg: Int
        get() = (ivEggAbility ushr 30) and 1

    val abilitySlot: Int
        get() = (ivEggAbility ushr 31) and 1

    fun setAbilitySlot(slot: Int) {
        ivEggAbility = (ivEggAbility and 0x7FFFFFFF) or ((slot and 1) shl 31)
    }

    fun isEmpty(): Boolean = pid == 0 && species == 0

    // -- encode --------------------------------------------------------------

    /**
     * Rebuild the 100-byte struct from decoded fields (re-encrypts and
     * recomputes the substructure checksum).
     */
    fun encode(): ByteArray {
        require(nicknameRaw.size == 10) { "nicknameRaw must be 10 bytes" }
        require(otNameRaw.size == 7) { "otNameRaw must be 7 bytes" }
        val buf = ByteArray(SIZE)
        Gen3Bytes.writeU32(buf, 0x00, pid)
        Gen3Bytes.writeU32(buf, 0x04, otId)
        nicknameRaw.copyInto(buf, 0x08)
        Gen3Bytes.writeU16(buf, 0x12, language)
        otNameRaw.copyInto(buf, 0x14)
        Gen3Bytes.writeU8(buf, 0x1B, markings)
        Gen3Bytes.writeU16(buf, 0x1E, unknown1E)

        val g = ByteArray(12)
        Gen3Bytes.writeU16(g, 0, species)
        Gen3Bytes.writeU16(g, 2, heldItem)
        Gen3Bytes.writeU32(g, 4, experience)
        Gen3Bytes.writeU8(g, 8, ppBonuses)
        Gen3Bytes.writeU8(g, 9, friendship)
        Gen3Bytes.writeU16(g, 10, growthUnknown)

        val a = ByteArray(12)
        for (i in 0..3) {
            Gen3Bytes.writeU16(a, 2 * i, moves[i])
            Gen3Bytes.writeU8(a, 8 + i, pp[i])
        }

        val e = ByteArray(12)
        for (i in 0..5) {
            Gen3Bytes.writeU8(e, i, evs[i])
            Gen3Bytes.writeU8(e, 6 + i, contest[i])
        }

        val m = ByteArray(12)
        Gen3Bytes.writeU8(m, 0, pokerus)
        Gen3Bytes.writeU8(m, 1, metLocation)
        Gen3Bytes.writeU16(m, 2, origins)
        Gen3Bytes.writeU32(m, 4, ivEggAbility)
        Gen3Bytes.writeU32(m, 8, ribbons)

        val order = SUBSTRUCT_ORDER[unsignedMod(pid, 24)]
        val decrypted = ByteArray(48)
        for (slot in 0..3) {
            val sub = when (order[slot]) {
                'G' -> g
                'A' -> a
                'E' -> e
                else -> m
            }
            sub.copyInto(decrypted, slot * 12)
        }

        Gen3Bytes.writeU16(buf, 0x1C, dataChecksum(decrypted))
        val key = pid xor otId
        xorBlockInPlace(decrypted, key)
        decrypted.copyInto(buf, 0x20)

        Gen3Bytes.writeU32(buf, 0x50, status)
        Gen3Bytes.writeU8(buf, 0x54, level)
        Gen3Bytes.writeU8(buf, 0x55, pokerusDays)
        Gen3Bytes.writeU16(buf, 0x56, currentHp)
        Gen3Bytes.writeU16(buf, 0x58, maxHp)
        Gen3Bytes.writeU16(buf, 0x5A, attack)
        Gen3Bytes.writeU16(buf, 0x5C, defense)
        Gen3Bytes.writeU16(buf, 0x5E, speed)
        Gen3Bytes.writeU16(buf, 0x60, spAttack)
        Gen3Bytes.writeU16(buf, 0x62, spDefense)
        return buf
    }

    override fun toString(): String {
        val iv = ivs()
        return "Gen3Mon(species=$species item=$heldItem lv=$level exp=$experience " +
            "nature=$natureName abilitySlot=$abilitySlot nick=$nickname ot=$otName " +
            "moves=${moves.toList()} evs=${evs.toList()} ivs=${iv.toList()} " +
            "hp=$currentHp/$maxHp atk=$attack def=$defense spe=$speed spa=$spAttack spd=$spDefense)"
    }

    companion object {
        /** Size of a party Pokemon struct in bytes. */
        const val SIZE = 100

        /**
         * Substructure order by PID % 24. G=Growth A=Attacks E=EVs M=Misc.
         * Entry i gives, in storage order, which substructure occupies each
         * of the four 12-byte slots.
         */
        val SUBSTRUCT_ORDER = arrayOf(
            "GAEM", "GAME", "GEAM", "GEMA", "GMAE", "GMEA",
            "AGEM", "AGME", "AEGM", "AEMG", "AMGE", "AMEG",
            "EGAM", "EGMA", "EAGM", "EAMG", "EMGA", "EMAG",
            "MGAE", "MGEA", "MAGE", "MAEG", "MEGA", "MEAG"
        )

        val NATURES = arrayOf(
            "Hardy", "Lonely", "Brave", "Adamant", "Naughty",
            "Bold", "Docile", "Relaxed", "Impish", "Lax",
            "Timid", "Hasty", "Serious", "Jolly", "Naive",
            "Modest", "Mild", "Quiet", "Bashful", "Rash",
            "Calm", "Gentle", "Sassy", "Careful", "Quirky"
        )

        /**
         * Unsigned [value] % [modulus]. The PID is stored as a raw Int bit
         * pattern which may be negative; Kotlin's % on a negative Int would
         * give a different residue than Python's % on the unsigned value.
         */
        fun unsignedMod(value: Int, modulus: Int): Int =
            ((value.toLong() and 0xFFFFFFFFL) % modulus.toLong()).toInt()

        /** XOR each little-endian u32 of the 48-byte block with key (symmetric). */
        private fun xorBlockInPlace(block48: ByteArray, key: Int) {
            var i = 0
            while (i < 48) {
                Gen3Bytes.writeU32(block48, i, Gen3Bytes.readU32(block48, i) xor key)
                i += 4
            }
        }

        /** u16 sum of the decrypted 48-byte block taken as 24 u16 words. */
        private fun dataChecksum(decrypted48: ByteArray): Int {
            var total = 0
            var i = 0
            while (i < 48) {
                total = (total + Gen3Bytes.readU16(decrypted48, i)) and 0xFFFF
                i += 2
            }
            return total
        }

        /** An unused party slot is all zeroes (PID 0, OTID 0, checksum 0). */
        fun isEmptySlotBytes(buf: ByteArray, off: Int): Boolean {
            for (i in 0 until SIZE) {
                if (buf[off + i].toInt() != 0) {
                    return false
                }
            }
            return true
        }

        /**
         * Decode a 100-byte party struct at [off]. Verifies the substructure
         * checksum for non-empty slots.
         *
         * @throws IllegalArgumentException on checksum mismatch.
         */
        fun fromBytes(buf: ByteArray, off: Int = 0): Gen3Mon {
            val mon = Gen3Mon()
            mon.pid = Gen3Bytes.readU32(buf, off + 0x00)
            mon.otId = Gen3Bytes.readU32(buf, off + 0x04)
            mon.nicknameRaw = buf.copyOfRange(off + 0x08, off + 0x12)
            mon.language = Gen3Bytes.readU16(buf, off + 0x12)
            mon.otNameRaw = buf.copyOfRange(off + 0x14, off + 0x1B)
            mon.markings = Gen3Bytes.readU8(buf, off + 0x1B)
            val storedChecksum = Gen3Bytes.readU16(buf, off + 0x1C)
            mon.unknown1E = Gen3Bytes.readU16(buf, off + 0x1E)

            val key = mon.pid xor mon.otId
            val decrypted = buf.copyOfRange(off + 0x20, off + 0x50)
            xorBlockInPlace(decrypted, key)

            if (!isEmptySlotBytes(buf, off)) {
                val calc = dataChecksum(decrypted)
                if (calc != storedChecksum) {
                    throw IllegalArgumentException(
                        "substructure checksum mismatch: stored=0x%04X calc=0x%04X"
                            .format(storedChecksum, calc)
                    )
                }
            }

            val order = SUBSTRUCT_ORDER[unsignedMod(mon.pid, 24)]
            val subs = HashMap<Char, ByteArray>()
            for (slot in 0..3) {
                subs[order[slot]] = decrypted.copyOfRange(slot * 12, (slot + 1) * 12)
            }

            val g = subs.getValue('G')
            mon.species = Gen3Bytes.readU16(g, 0)
            mon.heldItem = Gen3Bytes.readU16(g, 2)
            mon.experience = Gen3Bytes.readU32(g, 4)
            mon.ppBonuses = Gen3Bytes.readU8(g, 8)
            mon.friendship = Gen3Bytes.readU8(g, 9)
            mon.growthUnknown = Gen3Bytes.readU16(g, 10)

            val a = subs.getValue('A')
            for (i in 0..3) {
                mon.moves[i] = Gen3Bytes.readU16(a, 2 * i)
                mon.pp[i] = Gen3Bytes.readU8(a, 8 + i)
            }

            val e = subs.getValue('E')
            for (i in 0..5) {
                mon.evs[i] = Gen3Bytes.readU8(e, i)
                mon.contest[i] = Gen3Bytes.readU8(e, 6 + i)
            }

            val m = subs.getValue('M')
            mon.pokerus = Gen3Bytes.readU8(m, 0)
            mon.metLocation = Gen3Bytes.readU8(m, 1)
            mon.origins = Gen3Bytes.readU16(m, 2)
            mon.ivEggAbility = Gen3Bytes.readU32(m, 4)
            mon.ribbons = Gen3Bytes.readU32(m, 8)

            mon.status = Gen3Bytes.readU32(buf, off + 0x50)
            mon.level = Gen3Bytes.readU8(buf, off + 0x54)
            mon.pokerusDays = Gen3Bytes.readU8(buf, off + 0x55)
            mon.currentHp = Gen3Bytes.readU16(buf, off + 0x56)
            mon.maxHp = Gen3Bytes.readU16(buf, off + 0x58)
            mon.attack = Gen3Bytes.readU16(buf, off + 0x5A)
            mon.defense = Gen3Bytes.readU16(buf, off + 0x5C)
            mon.speed = Gen3Bytes.readU16(buf, off + 0x5E)
            mon.spAttack = Gen3Bytes.readU16(buf, off + 0x60)
            mon.spDefense = Gen3Bytes.readU16(buf, off + 0x62)
            return mon
        }
    }
}
