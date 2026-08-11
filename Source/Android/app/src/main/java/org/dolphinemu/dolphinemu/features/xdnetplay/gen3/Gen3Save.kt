// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.gen3

/**
 * Little-endian byte helpers.
 *
 * Kotlin Byte is signed: every read masks with 0xFF before widening.
 * u32 values are handled as raw Int bit patterns (may be negative); use
 * [readU32Long] when unsigned arithmetic or comparison is needed.
 */
internal object Gen3Bytes {
    fun readU8(buf: ByteArray, off: Int): Int = buf[off].toInt() and 0xFF

    fun readU16(buf: ByteArray, off: Int): Int =
        (buf[off].toInt() and 0xFF) or ((buf[off + 1].toInt() and 0xFF) shl 8)

    /** Returns the raw 32-bit pattern; may be negative as a Kotlin Int. */
    fun readU32(buf: ByteArray, off: Int): Int =
        (buf[off].toInt() and 0xFF) or
            ((buf[off + 1].toInt() and 0xFF) shl 8) or
            ((buf[off + 2].toInt() and 0xFF) shl 16) or
            ((buf[off + 3].toInt() and 0xFF) shl 24)

    /** u32 as a non-negative Long (for unsigned arithmetic/comparisons). */
    fun readU32Long(buf: ByteArray, off: Int): Long =
        readU32(buf, off).toLong() and 0xFFFFFFFFL

    fun writeU8(buf: ByteArray, off: Int, value: Int) {
        buf[off] = (value and 0xFF).toByte()
    }

    fun writeU16(buf: ByteArray, off: Int, value: Int) {
        buf[off] = (value and 0xFF).toByte()
        buf[off + 1] = ((value ushr 8) and 0xFF).toByte()
    }

    fun writeU32(buf: ByteArray, off: Int, value: Int) {
        buf[off] = (value and 0xFF).toByte()
        buf[off + 1] = ((value ushr 8) and 0xFF).toByte()
        buf[off + 2] = ((value ushr 16) and 0xFF).toByte()
        buf[off + 3] = ((value ushr 24) and 0xFF).toByte()
    }
}

/** One section whose stored footer checksum does not match the computed one. */
data class ChecksumMismatch(
    val block: Int,
    val slot: Int,
    val sectionId: Int,
    val stored: Int,
    val calculated: Int
)

/**
 * Which Gen 3 game wrote a save image. The block/section/footer layout is
 * identical across all five cartridges; what differs is the per-section
 * checksum length table and some section-interior offsets, so game-aware
 * callers pick behavior off this enum. [EmeraldSave.detectGame] is the single
 * authoritative detector on every platform (mirrors Gen3Save.cpp).
 *
 * [displayName] mirrors the C++ GameDisplayName(), for user-facing messages.
 */
enum class Gen3Game(val displayName: String) {
    RubySapphire("Ruby/Sapphire"),
    FireRedLeafGreen("FireRed/LeafGreen"),
    Emerald("Emerald")
}

/**
 * Pokemon Emerald (Gen 3) save file: 131072 bytes of flash plus any trailing
 * bytes (e.g. the 16-byte mGBA RTC footer), which are preserved verbatim.
 *
 * Ported 1:1 from the empirically validated Python reference
 * (gen3save.py EmeraldSave; see VALIDATION.md):
 *  - Flash holds two 57344-byte game-save blocks of 14 rotating 4096-byte
 *    sections each; the physical slot of logical section N shifts every save.
 *  - Section footer at +0x0FF4: u16 section id, u16 checksum,
 *    u32 signature (0x08012025), u32 save index (shared by the block).
 *  - Active block = the fully valid block with the higher save index.
 *  - Section checksum: sum of the section's data bytes as little-endian u32s
 *    over that section's data length, folded to u16 as
 *    ((sum & 0xFFFF) + (sum >> 16)) & 0xFFFF.
 */
class EmeraldSave(raw: ByteArray) {
    private val raw: ByteArray
    val activeBlock: Int
    private val sectionOffsets: IntArray

    init {
        require(raw.size >= 2 * BLOCK_SIZE) { "file too small for an Emerald save" }
        this.raw = raw.copyOf()
        activeBlock = findActiveBlock()
        sectionOffsets = mapSections(activeBlock)
    }

    /** Bytes after the two game-save blocks (Hall of Fame area, emulator RTC
     *  footer, ...). Preserved verbatim; exposed for inspection only. */
    val extraBytes: ByteArray
        get() = raw.copyOfRange(2 * BLOCK_SIZE, raw.size)

    /** The full file image (flash + any trailing bytes), reflecting all edits. */
    fun toBytes(): ByteArray = raw.copyOf()

    // -- block/section discovery ---------------------------------------------

    /**
     * Returns (saveIndex, allValid) for a block; the index is taken from any
     * valid section (all 14 share it). saveIndex is -1 if no section was valid.
     */
    private fun blockIndexAndValid(block: Int): Pair<Long, Boolean> {
        var idx = -1L
        var valid = true
        val seen = BooleanArray(SECTIONS_PER_BLOCK)
        var seenCount = 0
        for (slot in 0 until SECTIONS_PER_BLOCK) {
            val off = block * BLOCK_SIZE + slot * SECTION_SIZE
            val sid = Gen3Bytes.readU16(raw, off + SECTION_FOOTER_OFFSET)
            val sig = Gen3Bytes.readU32(raw, off + SECTION_FOOTER_OFFSET + 4)
            val sidx = Gen3Bytes.readU32Long(raw, off + SECTION_FOOTER_OFFSET + 8)
            if (sig != SAVE_SIGNATURE || sid >= SECTIONS_PER_BLOCK) {
                valid = false
                continue
            }
            if (!seen[sid]) {
                seen[sid] = true
                seenCount++
            }
            if (idx == -1L) {
                idx = sidx
            } else if (sidx != idx) {
                valid = false
            }
        }
        if (seenCount != SECTIONS_PER_BLOCK) {
            valid = false
        }
        return Pair(idx, valid)
    }

    private fun findActiveBlock(): Int {
        val (idx0, ok0) = blockIndexAndValid(0)
        val (idx1, ok1) = blockIndexAndValid(1)
        if (ok0 && ok1) {
            return if (idx1 > idx0) 1 else 0
        }
        if (ok1) {
            return 1
        }
        if (ok0) {
            return 0
        }
        throw IllegalArgumentException("no valid save block found")
    }

    /** Map logical section id -> absolute file offset of its 4096-byte slot. */
    private fun mapSections(block: Int): IntArray {
        val offsets = IntArray(SECTIONS_PER_BLOCK)
        for (slot in 0 until SECTIONS_PER_BLOCK) {
            val off = block * BLOCK_SIZE + slot * SECTION_SIZE
            val sid = Gen3Bytes.readU16(raw, off + SECTION_FOOTER_OFFSET)
            if (sid < SECTIONS_PER_BLOCK) {
                offsets[sid] = off
            }
        }
        return offsets
    }

    // -- section access ------------------------------------------------------

    /** Copy of the 4096-byte physical section holding logical [sectionId]. */
    fun sectionBytes(sectionId: Int): ByteArray {
        val off = sectionOffsets[sectionId]
        return raw.copyOfRange(off, off + SECTION_SIZE)
    }

    /**
     * Check every section footer checksum in BOTH blocks. Returns the list of
     * mismatches (empty when the file is fully consistent).
     */
    /**
     * Game-aware callers pass [detectGame]'s answer; the Emerald default keeps
     * the bundled-save paths (and every pre-import caller) unchanged.
     */
    fun verifyAllChecksums(game: Gen3Game = Gen3Game.Emerald): List<ChecksumMismatch> {
        val lengths = sectionDataLengths(game)
        val bad = ArrayList<ChecksumMismatch>()
        for (block in 0..1) {
            for (slot in 0 until SECTIONS_PER_BLOCK) {
                val off = block * BLOCK_SIZE + slot * SECTION_SIZE
                val sid = Gen3Bytes.readU16(raw, off + SECTION_FOOTER_OFFSET)
                val stored = Gen3Bytes.readU16(raw, off + SECTION_FOOTER_OFFSET + 2)
                if (sid >= SECTIONS_PER_BLOCK) {
                    continue
                }
                val calc = sectionChecksum(raw, off, lengths[sid])
                if (calc != stored) {
                    bad.add(ChecksumMismatch(block, slot, sid, stored, calc))
                }
            }
        }
        return bad
    }

    fun updateSectionChecksum(sectionId: Int, game: Gen3Game = Gen3Game.Emerald) {
        val off = sectionOffsets[sectionId]
        val calc = sectionChecksum(raw, off, sectionDataLengths(game)[sectionId])
        Gen3Bytes.writeU16(raw, off + SECTION_FOOTER_OFFSET + 2, calc)
    }

    // -- trainer info (logical section 0) ------------------------------------

    val trainerName: String
        get() = Gen3Text.decode(raw, sectionOffsets[0] + TRAINER_NAME_OFFSET, TRAINER_NAME_LEN)

    /**
     * Overwrite the trainer name in the trainer block and fix section 0's
     * checksum. [name] must be 1..[TRAINER_NAME_LEN] (7) characters, every one
     * of them encodable in the Gen 3 charset.
     *
     * An unencodable character throws and leaves the save completely
     * untouched, rather than writing a '?' or a truncated field. Use
     * [sanitizeTrainerName] first for input that should degrade instead.
     *
     * NOTE for callers that also rebuild the party: every Pokemon carries its
     * own OT name copy, so renaming the trainer does NOT rename the mons
     * already in the party. Set the name FIRST, then build/re-stamp the party.
     *
     * @throws IllegalArgumentException on an empty, over-long, or unencodable name.
     */
    fun setTrainerName(name: String) {
        require(name.isNotEmpty()) { "trainer name cannot be empty" }
        // encode() would silently truncate; for a name a human just typed, say
        // so instead.
        require(name.length <= TRAINER_NAME_LEN) {
            "trainer name is longer than $TRAINER_NAME_LEN characters"
        }
        // Encode first: it throws on any character with no Gen 3 byte, before
        // a single byte of the save has been touched.
        val encoded = Gen3Text.encode(name, TRAINER_NAME_LEN)
        val off = sectionOffsets[0] + TRAINER_NAME_OFFSET
        encoded.copyInto(raw, off)
        // encode() already 0xFF-pads the 7 usable bytes; byte 7 is the field's
        // hard terminator, which the game keeps at 0xFF even for a 7-character
        // name. Byte 8 is the gender and must not be disturbed.
        raw[off + TRAINER_NAME_LEN] = Gen3Text.TERMINATOR.toByte()
        updateSectionChecksum(0)
    }

    /** Raw u32: low u16 = public (visible) trainer ID, high u16 = secret ID. */
    val trainerId: Int
        get() = Gen3Bytes.readU32(raw, sectionOffsets[0] + TRAINER_ID_OFFSET)

    val trainerPublicId: Int
        get() = trainerId and 0xFFFF

    val trainerSecretId: Int
        get() = (trainerId ushr 16) and 0xFFFF

    val trainerGender: Int
        get() = Gen3Bytes.readU8(raw, sectionOffsets[0] + TRAINER_GENDER_OFFSET)

    // -- party (logical section 1) -------------------------------------------

    /**
     * Stored party count, clamped to 0..6. (The Python reference trusted the
     * raw u32; the clamp is a defensive addition so a corrupt count cannot
     * drive reads past the party area.)
     */
    val partyCount: Int
        get() {
            val off = sectionOffsets[1]
            val count = Gen3Bytes.readU32Long(raw, off + PARTY_COUNT_OFFSET)
            return count.coerceIn(0L, PARTY_MAX.toLong()).toInt()
        }

    fun readParty(): List<Gen3Mon> {
        val off = sectionOffsets[1]
        val mons = ArrayList<Gen3Mon>()
        for (i in 0 until partyCount) {
            mons.add(Gen3Mon.fromBytes(raw, off + PARTY_DATA_OFFSET + i * Gen3Mon.SIZE))
        }
        return mons
    }

    /**
     * Serialize [mons] into section 1, zero unused slots, fix the count and
     * the section checksum.
     */
    fun writeParty(mons: List<Gen3Mon>) {
        require(mons.size <= PARTY_MAX) { "party larger than $PARTY_MAX" }
        val off = sectionOffsets[1]
        Gen3Bytes.writeU32(raw, off + PARTY_COUNT_OFFSET, mons.size)
        for (i in 0 until PARTY_MAX) {
            val slotOff = off + PARTY_DATA_OFFSET + i * Gen3Mon.SIZE
            val bytes = if (i < mons.size) mons[i].encode() else ByteArray(Gen3Mon.SIZE)
            bytes.copyInto(raw, slotOff)
        }
        updateSectionChecksum(1)
    }

    /** Re-encode the current party over itself (round-trip identity helper). */
    fun rewritePartyInPlace() {
        writeParty(readParty())
    }

    companion object {
        const val SECTION_SIZE = 4096
        const val SECTION_FOOTER_OFFSET = 0x0FF4
        const val SECTIONS_PER_BLOCK = 14
        const val BLOCK_SIZE = SECTION_SIZE * SECTIONS_PER_BLOCK // 57344
        const val SAVE_SIGNATURE = 0x08012025

        // Section 1 (Team/Items) offsets - Emerald.
        const val PARTY_COUNT_OFFSET = 0x0234 // u32
        const val PARTY_DATA_OFFSET = 0x0238  // 6 * 100 bytes
        const val PARTY_MAX = 6

        // Section 0 (Trainer Info) offsets - Emerald.
        const val TRAINER_NAME_OFFSET = 0x0000 // 7 bytes + 0xFF
        const val TRAINER_NAME_LEN = 7
        // The field the game reserves is 8 bytes (pokeemerald: playerName is
        // PLAYER_NAME_LENGTH + 1): 7 usable characters plus a byte that is
        // ALWAYS the 0xFF terminator. Byte 8 is the gender.
        const val TRAINER_NAME_FIELD_SIZE = TRAINER_NAME_LEN + 1
        const val TRAINER_GENDER_OFFSET = 0x0008
        const val TRAINER_ID_OFFSET = 0x000A   // u32: low u16 public, high u16 secret
        // u32 read by [detectGame]: 0 in Ruby/Sapphire (the field is unused
        // there), the constant 1 in FireRed/LeafGreen, and Emerald's security
        // key -- an effectively random nonzero value -- otherwise. Same
        // heuristic every Gen 3 save tool uses; the bundled team saves carry
        // 0xAA2F3199 / 0xCF0579AC.
        const val GAME_CODE_OFFSET = 0x00AC

        /** THE authoritative game detector (see [Gen3Game]). */
        fun detectGame(save: EmeraldSave): Gen3Game {
            val sec0 = save.sectionBytes(0)
            return when (Gen3Bytes.readU32(sec0, GAME_CODE_OFFSET)) {
                0 -> Gen3Game.RubySapphire
                1 -> Gen3Game.FireRedLeafGreen
                else -> Gen3Game.Emerald
            }
        }

        /**
         * Coerce arbitrary (possibly untrusted, possibly over-long) text into
         * something [setTrainerName] will accept: trim surrounding whitespace,
         * map straight quotes to the charset's curly forms, DROP anything still
         * unencodable, clamp to [TRAINER_NAME_LEN] characters.
         *
         * Returns "" when nothing survives -- callers decide whether that means
         * "keep the existing name" or "reject the request".
         */
        fun sanitizeTrainerName(name: String): String =
            // Trim first: a leading space IS encodable (0x00) and would
            // otherwise eat one of the seven slots and render blank in-game.
            // Trim again after dropping unencodable characters, since dropping
            // one can expose new surrounding whitespace ("Lo 🐬 gan").
            Gen3Text.sanitize(name.trim(), TRAINER_NAME_LEN).trim()

        /**
         * Per-section valid data length used by the checksum (Emerald).
         * Index = logical section ID. Empirically confirmed: with these exact
         * lengths every section checksum in the test saves matches.
         */
        val EMERALD_SECTION_DATA_LENGTHS = intArrayOf(
            3884, // 0  Trainer info
            3968, // 1  Team / items
            3968, // 2  Game state
            3968, // 3  Misc data
            3848, // 4  Rival info
            3968, // 5  PC buffer A
            3968, // 6  PC buffer B
            3968, // 7  PC buffer C
            3968, // 8  PC buffer D
            3968, // 9  PC buffer E
            3968, // 10 PC buffer F
            3968, // 11 PC buffer G
            3968, // 12 PC buffer H
            2000  // 13 PC buffer I
        )

        /**
         * Ruby/Sapphire and FireRed/LeafGreen checksum the same 14 sections
         * over different lengths. Each length is the sizeof() of that game's
         * own save struct chunk -- transcribed from the decomps' chunk tables
         * (pret/pokeruby src/save.c sSaveBlockChunks, pret/pokefirered
         * src/save.c sSaveSlotLayout: section 0 = SaveBlock2, 1..4 = SaveBlock1
         * split at 3968, 5..13 = PokemonStorage) with the struct sizes as
         * documented in PKHeX's SAV3 buffers (0x890/0xC40 RS, 0xF24/0xEE8
         * FRLG, 0x83D0 storage for all). Bytes past a section's length are
         * zeroed by the game's write buffer, which is why Emerald-length tools
         * APPEAR to work on RS/FRLG saves; verifying against the game's own
         * stored checksums still needs the real lengths.
         */
        val RS_SECTION_DATA_LENGTHS = intArrayOf(
            2192, // 0  Trainer info (sizeof(SaveBlock2) = 0x890)
            3968, // 1  Team / items
            3968, // 2  Game state
            3968, // 3  Misc data
            3136, // 4  Rival info (SaveBlock1 = 0x3AC0 leaves 0xC40)
            3968, // 5  PC buffer A
            3968, // 6  PC buffer B
            3968, // 7  PC buffer C
            3968, // 8  PC buffer D
            3968, // 9  PC buffer E
            3968, // 10 PC buffer F
            3968, // 11 PC buffer G
            3968, // 12 PC buffer H
            2000  // 13 PC buffer I
        )

        val FRLG_SECTION_DATA_LENGTHS = intArrayOf(
            3876, // 0  Trainer info (sizeof(SaveBlock2) = 0xF24)
            3968, // 1  Team / items
            3968, // 2  Game state
            3968, // 3  Misc data
            3816, // 4  Rival info (SaveBlock1 = 0x3D68 leaves 0xEE8)
            3968, // 5  PC buffer A
            3968, // 6  PC buffer B
            3968, // 7  PC buffer C
            3968, // 8  PC buffer D
            3968, // 9  PC buffer E
            3968, // 10 PC buffer F
            3968, // 11 PC buffer G
            3968, // 12 PC buffer H
            2000  // 13 PC buffer I
        )

        fun sectionDataLengths(game: Gen3Game): IntArray = when (game) {
            Gen3Game.RubySapphire -> RS_SECTION_DATA_LENGTHS
            Gen3Game.FireRedLeafGreen -> FRLG_SECTION_DATA_LENGTHS
            Gen3Game.Emerald -> EMERALD_SECTION_DATA_LENGTHS
        }

        /**
         * Sum [dataLength] bytes starting at [base] as little-endian u32s
         * (mod 2^32), then fold to u16: ((sum & 0xFFFF) + (sum >> 16)) & 0xFFFF.
         */
        fun sectionChecksum(buf: ByteArray, base: Int, dataLength: Int): Int {
            var total = 0L
            var i = 0
            while (i < dataLength) {
                total = (total + Gen3Bytes.readU32Long(buf, base + i)) and 0xFFFFFFFFL
                i += 4
            }
            return (((total and 0xFFFFL) + (total ushr 16)) and 0xFFFFL).toInt()
        }
    }
}
