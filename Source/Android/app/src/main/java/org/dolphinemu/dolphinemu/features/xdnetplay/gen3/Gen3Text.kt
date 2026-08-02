// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.gen3

/**
 * Gen 3 western (International) character set encode/decode.
 *
 * Ported 1:1 from the empirically validated Python reference (gen3save.py);
 * all byte values were confirmed against real Emerald saves (VALIDATION.md).
 * Strings are fixed-width, 0xFF-terminated; a string that fills the whole
 * field carries no terminator, exactly like the games.
 */
object Gen3Text {
    const val TERMINATOR = 0xFF

    private val byteToChar = HashMap<Int, Char>()
    private val charToByte = HashMap<Char, Int>()

    private fun map(b: Int, c: Char) {
        byteToChar[b] = c
        charToByte[c] = b
    }

    init {
        map(0x00, ' ')
        // Digits 0-9: 0xA1..0xAA
        for (i in 0..9) {
            map(0xA1 + i, '0' + i)
        }
        map(0xAB, '!')
        map(0xAC, '?')
        map(0xAD, '.')
        map(0xAE, '-')
        map(0xAF, '・')
        map(0xB0, '…')
        map(0xB1, '“')
        map(0xB2, '”')
        map(0xB3, '‘')
        map(0xB4, '’')
        map(0xB5, '♂')
        map(0xB6, '♀')
        map(0xB7, '$')
        map(0xB8, ',')
        map(0xB9, '×')
        map(0xBA, '/')
        // A-Z: 0xBB..0xD4, a-z: 0xD5..0xEE
        for (i in 0..25) {
            map(0xBB + i, 'A' + i)
            map(0xD5 + i, 'a' + i)
        }
    }

    /** True if [c] has a Gen 3 byte value in this charset. */
    fun canEncode(c: Char): Boolean = charToByte.containsKey(c)

    /**
     * Decode up to [maxLen] bytes of Gen 3 text starting at [off].
     * Unknown bytes decode as '?', mirroring the Python reference.
     */
    fun decode(buf: ByteArray, off: Int, maxLen: Int): String {
        val sb = StringBuilder()
        for (i in 0 until maxLen) {
            // Kotlin Byte is signed: mask to 0..255 before any comparison.
            val b = buf[off + i].toInt() and 0xFF
            if (b == TERMINATOR) {
                break
            }
            sb.append(byteToChar[b] ?: '?')
        }
        return sb.toString()
    }

    /**
     * Encode [text] to exactly [maxLen] bytes, 0xFF-terminated/padded.
     * Text longer than [maxLen] is truncated (as in the Python reference).
     *
     * @throws IllegalArgumentException if a character cannot be encoded.
     */
    fun encode(text: String, maxLen: Int): ByteArray {
        val out = ByteArray(maxLen) { TERMINATOR.toByte() }
        val n = minOf(text.length, maxLen)
        for (i in 0 until n) {
            val ch = text[i]
            val b = charToByte[ch]
                ?: throw IllegalArgumentException(
                    "character '$ch' not encodable in Gen 3 charset"
                )
            out[i] = b.toByte()
        }
        return out
    }

    /**
     * Make [text] Gen 3-encodable by force: straight ASCII quotes become the
     * charset's curly forms, anything else unencodable is DROPPED, and the
     * result is clamped to [maxLen] characters (0 = no clamp).
     *
     * Use this for input that must degrade rather than fail (a nickname out of
     * a Showdown paste, a trainer name that arrived over the wire); use
     * [encode]'s exception when a human is typing and deserves to be told which
     * character is the problem.
     */
    fun sanitize(text: String, maxLen: Int = 0): String {
        val sb = StringBuilder()
        for (c in text) {
            if (maxLen != 0 && sb.length == maxLen) {
                break
            }
            val mapped = when (c) {
                '\'' -> '’'
                '"' -> '”'
                else -> c
            }
            if (canEncode(mapped)) {
                sb.append(mapped)
            }
        }
        return sb.toString()
    }
}
