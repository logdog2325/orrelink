// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.pbr

/**
 * Explicit big-endian byte helpers for the Wii-side (PowerPC) save format.
 *
 * Everything in a Pokémon Battle Revolution save — the Genius Sonority cipher
 * keys, the container checksums, and every field of a BK4 record — is stored
 * big-endian, which is the opposite of the Gen 3 GBA saves handled by
 * [org.dolphinemu.dolphinemu.features.xdnetplay.gen3.Gen3Bytes]. The two helper
 * objects are deliberately separate and their names carry the endianness so a
 * misuse is visible at the call site rather than at runtime.
 *
 * Kotlin Byte is signed: every read masks with 0xFF before widening. u32 values
 * are returned as raw Int bit patterns (may be negative); use [readU32LongBE]
 * when unsigned arithmetic or comparison is needed.
 */
internal object PbrBytes {
    fun readU8(buf: ByteArray, off: Int): Int = buf[off].toInt() and 0xFF

    fun readU16BE(buf: ByteArray, off: Int): Int =
        ((buf[off].toInt() and 0xFF) shl 8) or (buf[off + 1].toInt() and 0xFF)

    /** Returns the raw 32-bit pattern; may be negative as a Kotlin Int. */
    fun readU32BE(buf: ByteArray, off: Int): Int =
        ((buf[off].toInt() and 0xFF) shl 24) or
            ((buf[off + 1].toInt() and 0xFF) shl 16) or
            ((buf[off + 2].toInt() and 0xFF) shl 8) or
            (buf[off + 3].toInt() and 0xFF)

    /** u32 as a non-negative Long (for unsigned arithmetic/comparisons). */
    fun readU32LongBE(buf: ByteArray, off: Int): Long =
        readU32BE(buf, off).toLong() and 0xFFFFFFFFL

    /** IEEE-754 binary64, big-endian (the profile playtime field). */
    fun readF64BE(buf: ByteArray, off: Int): Double {
        var bits = 0L
        for (i in 0 until 8) {
            bits = (bits shl 8) or (buf[off + i].toLong() and 0xFF)
        }
        return Double.fromBits(bits)
    }

    fun writeU8(buf: ByteArray, off: Int, value: Int) {
        buf[off] = (value and 0xFF).toByte()
    }

    fun writeU16BE(buf: ByteArray, off: Int, value: Int) {
        buf[off] = ((value ushr 8) and 0xFF).toByte()
        buf[off + 1] = (value and 0xFF).toByte()
    }

    fun writeU32BE(buf: ByteArray, off: Int, value: Int) {
        buf[off] = ((value ushr 24) and 0xFF).toByte()
        buf[off + 1] = ((value ushr 16) and 0xFF).toByte()
        buf[off + 2] = ((value ushr 8) and 0xFF).toByte()
        buf[off + 3] = (value and 0xFF).toByte()
    }

    fun writeU32BE(buf: ByteArray, off: Int, value: Long) {
        writeU32BE(buf, off, (value and 0xFFFFFFFFL).toInt())
    }

    /**
     * Unsigned [value] % [modulus]. A PID is stored as a raw Int bit pattern
     * which may be negative; Kotlin's % on a negative Int would give a
     * different residue than the unsigned arithmetic the games use.
     */
    fun unsignedMod(value: Int, modulus: Int): Int =
        ((value.toLong() and 0xFFFFFFFFL) % modulus.toLong()).toInt()
}
