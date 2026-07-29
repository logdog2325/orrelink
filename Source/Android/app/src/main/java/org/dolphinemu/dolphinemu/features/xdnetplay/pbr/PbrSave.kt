// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.pbr

/** Integrity of one 0x1C0000 partition. */
data class PbrPartitionStatus(
    val index: Int,
    val saveCount: Long,
    val smallChecksumOk: Boolean,
    val bigChecksumOk: Boolean
) {
    val valid: Boolean get() = smallChecksumOk && bigChecksumOk
}

/** One storage slot: where it lives in the container and what (if anything) is in it. */
data class PbrSlot(val offset: Int, val label: String, val mon: Bk4Mon?)

/** Which run of slots inside a profile the editor is looking at. */
sealed class PbrContainer {
    abstract val slotCount: Int
    abstract val label: String

    object Party : PbrContainer() {
        override val slotCount: Int get() = PbrSave.PARTY_SLOTS
        override val label: String get() = "Party"
    }

    data class Box(val index: Int) : PbrContainer() {
        override val slotCount: Int get() = PbrSave.BOX_SLOTS
        override val label: String get() = "Box ${index + 1}"
    }
}

/**
 * A Pokémon Battle Revolution save (`PbrSaveData`, exactly 0x380000 bytes),
 * held decrypted in memory.
 *
 * Container geometry (PKHeX SAV4BR.cs:19-22):
 *   0x380000 whole file
 *     = 2 partitions of 0x1C0000, each independently encrypted and checksummed
 *       = 4 profiles ("save slots") of 0x6FF00, plus a 0x400 tail that holds
 *         the partition's big checksum block.
 *
 * The two partitions are a ping-pong pair; the live one is chosen by save
 * counter among the partitions whose checksums verify (SAV4BR.cs:113-124). Both
 * partitions were valid in the save this port was proven against, so the
 * counter tiebreak is load-bearing, not decoration.
 *
 * Gotcha worth repeating: profile 0 starts at partition + 0, so its first 0x100
 * bytes ARE the partition header — bytes 0x08-0x47 are the small checksum block
 * and 0x4C is the save counter. Nothing this class exposes reaches into them.
 *
 * The class holds no Android types on purpose: it is exercised by a plain-JVM
 * round-trip test that re-encrypts a real save and asserts byte equality.
 */
class PbrSave private constructor(
    private val dec: ByteArray,
    /** Index of the live partition, 0 or 1. */
    val partition: Int,
    val saveCount: Long,
    val partitions: List<PbrPartitionStatus>
) {
    // -- profile-level accessors ---------------------------------------------

    fun profileStart(profile: Int): Int {
        require(profile in 0 until PROFILE_COUNT) { "profile out of range: $profile" }
        return partition * SIZE_PARTITION + profile * SIZE_PROFILE
    }

    /** Profile owner name — raw UTF-16 BE, NOT the BK4 glyph encoding. */
    fun profileOtName(profile: Int): String =
        Gen4Text.decodeUnicodeBE(dec, profileStart(profile) + OFS_OT_NAME, OT_NAME_BYTES)

    /**
     * Visible trainer ID. SAV4BR.cs:283 splits it across two bytes at opposite
     * ends of the trainer block: high at +0x12867, low at +0x12860.
     *
     * Caveat carried over from the format work: in the reference save the low
     * byte is 0x00 in every profile, which makes the resulting TIDs suspiciously
     * round. SID and money in the same block self-corroborate (999999 = MaxMoney)
     * but the TID split does not, so this editor never writes it.
     */
    fun profileTid(profile: Int): Int {
        val s = profileStart(profile)
        return (PbrBytes.readU8(dec, s + OFS_TID_HI) shl 8) or PbrBytes.readU8(dec, s + OFS_TID_LO)
    }

    /** Secret ID; SAV4BR.cs:295 stores it high-byte-first at +0x12865/+0x12866. */
    fun profileSid(profile: Int): Int {
        val s = profileStart(profile)
        return (PbrBytes.readU8(dec, s + OFS_SID_HI) shl 8) or PbrBytes.readU8(dec, s + OFS_SID_LO)
    }

    /** Money, 3-byte big-endian (SAV4BR.cs:270). */
    fun profileMoney(profile: Int): Int {
        val s = profileStart(profile) + OFS_MONEY
        return (PbrBytes.readU8(dec, s) shl 16) or
            (PbrBytes.readU8(dec, s + 1) shl 8) or
            PbrBytes.readU8(dec, s + 2)
    }

    /** Playtime in seconds, stored as an IEEE-754 double, big-endian. */
    fun profilePlaytimeSeconds(profile: Int): Double =
        PbrBytes.readF64BE(dec, profileStart(profile) + OFS_PLAYTIME)

    fun boxName(profile: Int, box: Int): String {
        require(box in 0 until BOX_COUNT) { "box out of range: $box" }
        return Gen4Text.decodeUnicodeBE(
            dec, profileStart(profile) + OFS_BOX_NAMES + box * BOX_NAME_BYTES, BOX_NAME_BYTES
        )
    }

    /** True when a profile has never been used (no name and no Pokémon). */
    fun isProfileEmpty(profile: Int): Boolean {
        if (profileOtName(profile).isNotEmpty()) {
            return false
        }
        for (container in listOf(PbrContainer.Party) + (0 until BOX_COUNT).map { PbrContainer.Box(it) }) {
            if (readContainer(profile, container).any { it.mon != null }) {
                return false
            }
        }
        return true
    }

    // -- entity access --------------------------------------------------------

    /** SAV4BR.cs:434 — 6 party slots of 220 bytes; only the first 136 are a BK4. */
    fun partyOffset(profile: Int, slot: Int): Int {
        require(slot in 0 until PARTY_SLOTS) { "party slot out of range: $slot" }
        return profileStart(profile) + OFS_PARTY + SIZE_PARTY_SLOT * slot
    }

    /** SAV4BR.cs:441 — 18 boxes of 30 packed 136-byte records. */
    fun boxOffset(profile: Int, box: Int, slot: Int): Int {
        require(box in 0 until BOX_COUNT) { "box out of range: $box" }
        require(slot in 0 until BOX_SLOTS) { "box slot out of range: $slot" }
        return profileStart(profile) + OFS_BOXES +
            Bk4Mon.SIZE_STORED * (box * BOX_SLOTS + slot)
    }

    fun slotOffset(profile: Int, container: PbrContainer, slot: Int): Int = when (container) {
        is PbrContainer.Party -> partyOffset(profile, slot)
        is PbrContainer.Box -> boxOffset(profile, container.index, slot)
    }

    fun readEntity(offset: Int): Bk4Mon? = Bk4Mon.decode(dec, offset)

    fun readContainer(profile: Int, container: PbrContainer): List<PbrSlot> =
        (0 until container.slotCount).map { slot ->
            val off = slotOffset(profile, container, slot)
            PbrSlot(off, "${container.label} ${slot + 1}", readEntity(off))
        }

    /** Every occupied slot in a profile, party first then boxes in order. */
    fun readAllEntities(profile: Int): List<PbrSlot> {
        val out = ArrayList<PbrSlot>()
        out += readContainer(profile, PbrContainer.Party).filter { it.mon != null }
        for (box in 0 until BOX_COUNT) {
            out += readContainer(profile, PbrContainer.Box(box)).filter { it.mon != null }
        }
        return out
    }

    /**
     * Write [mon] into the 136-byte record at [offset], or clear the record when
     * [mon] is null. Party slots keep their trailing 84 bytes untouched: they are
     * all zero in every slot of the reference save and their meaning is unknown,
     * so they are neither read nor rewritten.
     */
    fun writeEntity(offset: Int, mon: Bk4Mon?) {
        require(offset >= 0 && offset + Bk4Mon.SIZE_STORED <= dec.size) { "offset out of range" }
        if (mon == null) {
            dec.fill(0, offset, offset + Bk4Mon.SIZE_STORED)
        } else {
            mon.encode().copyInto(dec, offset)
        }
    }

    /**
     * Replace the contents of a container with [mons], packed from slot 0 and
     * zeroing the remainder. Only the slots of that one container are touched.
     */
    fun writeContainer(profile: Int, container: PbrContainer, mons: List<Bk4Mon>) {
        require(mons.size <= container.slotCount) {
            "${mons.size} Pokémon do not fit in ${container.label} (${container.slotCount} slots)"
        }
        for (slot in 0 until container.slotCount) {
            writeEntity(slotOffset(profile, container, slot), mons.getOrNull(slot))
        }
    }

    // -- serialization --------------------------------------------------------

    /**
     * Re-checksum the live partition and re-encrypt the whole container.
     * Does not mutate this instance, so a failed verification leaves the
     * in-memory save usable.
     */
    fun toBytes(): ByteArray {
        val out = dec.copyOf()
        setChecksums(out, partition)
        for (p in 0 until PARTITION_COUNT) {
            geniusCrypt(out, p * SIZE_PARTITION, decrypt = false)
        }
        return out
    }

    /** The decrypted image, for diffing in tests. Not exposed to the UI. */
    internal fun decryptedCopy(): ByteArray = dec.copyOf()

    companion object {
        // -- geometry (SAV4BR.cs:19-22) --
        const val SIZE_SAVE = 0x380000
        const val SIZE_PARTITION = 0x1C0000
        const val SIZE_PROFILE = 0x6FF00
        const val PARTITION_COUNT = 2
        const val PROFILE_COUNT = 4

        const val PARTY_SLOTS = 6
        const val BOX_COUNT = 18
        const val BOX_SLOTS = 30

        /** SAV4BR.cs:157 — a party slot is 220 bytes, of which 136 are the BK4. */
        const val SIZE_PARTY_SLOT = 220

        // -- profile-relative offsets (SAV4BR.cs) --
        const val OFS_PLAYTIME = 0x388
        const val OFS_OT_NAME = 0x390
        const val OT_NAME_BYTES = 16
        const val OFS_PARTY = 0x44C
        const val OFS_BOXES = 0x978
        const val OFS_MONEY = 0x12861
        const val OFS_TID_LO = 0x12860
        const val OFS_SID_HI = 0x12865
        const val OFS_SID_LO = 0x12866
        const val OFS_TID_HI = 0x12867
        const val OFS_BOX_NAMES = 0x58674
        const val BOX_NAME_BYTES = 0x28

        // -- partition-relative --
        const val OFS_SAVE_COUNT = 0x4C
        private const val CHK_BLOCK_BYTES = 64      // 16 x u32 BE
        private const val CHK_SMALL_AT = 0x08
        private const val CHK_SMALL_LEN = 0x100
        private const val CHK_BIG_AT = SIZE_PARTITION - 0x80
        private const val CHK_BIG_LEN = SIZE_PARTITION

        /**
         * Decrypt [raw] and pick the live partition.
         *
         * @throws IllegalArgumentException if the file is the wrong size or
         * neither partition passes its checksums (which would mean the cipher,
         * the checksum, or the file itself is wrong — never a reason to write).
         */
        fun load(raw: ByteArray): PbrSave {
            require(raw.size == SIZE_SAVE) {
                "PbrSaveData must be $SIZE_SAVE bytes, got ${raw.size}"
            }
            val dec = raw.copyOf()
            for (p in 0 until PARTITION_COUNT) {
                geniusCrypt(dec, p * SIZE_PARTITION, decrypt = true)
            }
            val status = (0 until PARTITION_COUNT).map { p ->
                val base = p * SIZE_PARTITION
                PbrPartitionStatus(
                    index = p,
                    saveCount = PbrBytes.readU32LongBE(dec, base + OFS_SAVE_COUNT),
                    smallChecksumOk = verifyChecksum(dec, base, CHK_SMALL_LEN, base + CHK_SMALL_AT),
                    bigChecksumOk = verifyChecksum(dec, base, CHK_BIG_LEN, base + CHK_BIG_AT)
                )
            }
            // SAV4BR.cs:113-124 DetectPartition.
            val first = status[0]
            val second = status[1]
            val chosen = when {
                second.valid && (!first.valid || second.saveCount > first.saveCount) -> second
                first.valid -> first
                else -> throw IllegalArgumentException(
                    "no valid partition in PbrSaveData (checksums failed on both halves)"
                )
            }
            return PbrSave(dec, chosen.index, chosen.saveCount, status)
        }

        // -- Genius Sonority stream cipher (GeniusCrypto.cs) -------------------

        /**
         * GeniusCrypto.cs:83-101 AdvanceKeys. Bias each key, then re-deal the
         * nibbles across all four. Written on Int with explicit 0xFFFF masks on
         * the results; the intermediate sums may carry into bit 16, which every
         * consumer masks away exactly as the reference does.
         */
        private fun advanceKeys(keys: IntArray) {
            val a0 = keys[0] + 0x43
            val a1 = keys[1] + 0x29
            val a2 = keys[2] + 0x17
            val a3 = keys[3] + 0x13
            val n3 = ((a0 ushr 12) and 0xF) or ((a1 ushr 8) and 0xF0) or
                ((a2 ushr 4) and 0xF00) or (a3 and 0xF000)
            val n2 = ((a0 ushr 8) and 0xF) or ((a1 ushr 4) and 0xF0) or
                (a2 and 0xF00) or ((a3 shl 4) and 0xF000)
            val n1 = ((a0 ushr 4) and 0xF) or (a1 and 0xF0) or
                ((a2 shl 4) and 0xF00) or ((a3 shl 8) and 0xF000)
            val n0 = (a0 and 0xF) or ((a1 shl 4) and 0xF0) or
                ((a2 shl 8) and 0xF00) or ((a3 shl 12) and 0xF000)
            keys[0] = n0 and 0xFFFF
            keys[1] = n1 and 0xFFFF
            keys[2] = n2 and 0xFFFF
            keys[3] = n3 and 0xFFFF
        }

        /**
         * GeniusCrypto.cs:35-81. The four u16 big-endian seed keys live at
         * partition + 0 and stay in plaintext; the body [base+8, base+0x1C0000)
         * is transformed in groups of four u16, decrypt subtracting the key and
         * encrypt adding it, mod 2^16. The region is 917500 u16 = 229375 whole
         * groups, so there is never a partial group.
         */
        private fun geniusCrypt(buf: ByteArray, base: Int, decrypt: Boolean) {
            val keys = IntArray(4) { PbrBytes.readU16BE(buf, base + it * 2) }
            val start = base + 8
            val end = base + SIZE_PARTITION
            require((end - start) % 8 == 0) { "cipher region is not a whole number of key groups" }
            var i = start
            while (i < end) {
                for (j in 0 until 4) {
                    val at = i + j * 2
                    val v = PbrBytes.readU16BE(buf, at)
                    val k = keys[j]
                    PbrBytes.writeU16BE(buf, at, if (decrypt) v - k else v + k)
                }
                advanceKeys(keys)
                i += 8
            }
        }

        // -- container checksums (SAV4BR.cs:540-635) ---------------------------

        /**
         * SAV4BR.cs:587-635 ComputeChecksums. Not a sum: checksums[k] is the
         * NUMBER of u16 values (read big-endian) in the span whose bit k is set.
         *
         * The stored block always lies inside the span it covers, so it is
         * treated as zero — skipped rather than copied, since the big span is
         * 1.8 MB and zeros contribute nothing.
         */
        private fun computeChecksums(
            buf: ByteArray,
            offset: Int,
            length: Int,
            skipAt: Int,
            skipLen: Int
        ): IntArray {
            require(offset % 2 == 0 && length % 2 == 0) { "checksum span must be u16-aligned" }
            require(skipAt % 2 == 0 && skipLen % 2 == 0) { "checksum block must be u16-aligned" }
            require(skipAt >= offset && skipAt + skipLen <= offset + length) {
                "checksum block must lie inside the covered span"
            }
            val counts = IntArray(16)
            val end = offset + length
            var i = offset
            while (i < end) {
                if (i == skipAt) {
                    i += skipLen
                    continue
                }
                val v = PbrBytes.readU16BE(buf, i)
                for (k in 0 until 16) {
                    counts[k] += (v ushr k) and 1
                }
                i += 2
            }
            return counts
        }

        private fun verifyChecksum(buf: ByteArray, offset: Int, length: Int, chkAt: Int): Boolean {
            val computed = computeChecksums(buf, offset, length, chkAt, CHK_BLOCK_BYTES)
            for (k in 0 until 16) {
                if (PbrBytes.readU32BE(buf, chkAt + k * 4) != computed[k]) {
                    return false
                }
            }
            return true
        }

        private fun setChecksum(buf: ByteArray, offset: Int, length: Int, chkAt: Int) {
            val computed = computeChecksums(buf, offset, length, chkAt, CHK_BLOCK_BYTES)
            for (k in 0 until 16) {
                PbrBytes.writeU32BE(buf, chkAt + k * 4, computed[k])
            }
        }

        /**
         * SAV4BR.cs:198-203 SetChecksums. ORDER MATTERS: the small block is
         * written first and is then itself covered by the big block.
         */
        private fun setChecksums(buf: ByteArray, partition: Int) {
            val base = partition * SIZE_PARTITION
            setChecksum(buf, base, CHK_SMALL_LEN, base + CHK_SMALL_AT)
            setChecksum(buf, base, CHK_BIG_LEN, base + CHK_BIG_AT)
        }
    }
}
