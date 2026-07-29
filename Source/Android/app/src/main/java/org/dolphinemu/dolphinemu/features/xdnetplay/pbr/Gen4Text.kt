// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.pbr

/**
 * The two string encodings a PBR save uses. They are NOT interchangeable and
 * conflating them is the classic way to corrupt a name field:
 *
 *  - [decodeGlyphs] / [encodeGlyphs] — BK4 nickname and OT name. u16 big-endian
 *    Gen 4 *glyph values* mapped through [Gen4Glyphs.TABLE_INT], terminated by
 *    0xFFFF. 'A' is 0x012B, 'a' is 0x0145, '0' is 0x0121.
 *    (PKHeX StringConverter4GC.LoadString/SetString.)
 *  - [decodeUnicodeBE] / [encodeUnicodeBE] — profile OT name and box names.
 *    Raw UTF-16 big-endian, NUL-terminated, with 0xFFFF as a variable escape.
 *    (PKHeX StringConverter4GC.LoadStringUnicodeBR.)
 *
 * Both were confirmed on a real save: the profile OT bytes
 * `0050 004B 0054 004F 0050 0049 0041 0000` and the BK4 OT bytes
 * `013A 0135 013E 0139 013A 0133 012B FFFF` both decode to "PKTOPIA".
 */
internal object Gen4Text {
    const val TERMINATOR = 0xFFFF
    private const val NUL = 0x0000

    private const val MALE_SIGN = '♂'
    private const val FEMALE_SIGN = '♀'

    /** Glyph values PKHeX uses for the half-width gender markers in names. */
    private const val GLYPH_HALF_MALE = 0x1BB
    private const val GLYPH_HALF_FEMALE = 0x1BC

    /** char -> glyph value. First occurrence in the table wins, then the
     *  half-width gender markers are forced to the values Gen 4 names use. */
    private val reverse: Map<Char, Int> by lazy {
        val map = HashMap<Char, Int>(Gen4Glyphs.TABLE_INT.size)
        for (i in Gen4Glyphs.TABLE_INT.indices) {
            val c = Gen4Glyphs.TABLE_INT[i]
            if (c != Gen4Glyphs.UNMAPPED && !map.containsKey(c.toChar())) {
                map[c.toChar()] = i
            }
        }
        map[MALE_SIGN] = GLYPH_HALF_MALE
        map[FEMALE_SIGN] = GLYPH_HALF_FEMALE
        map
    }

    /** True if [c] has a Gen 4 glyph value (i.e. can appear in a nickname). */
    fun canEncodeGlyph(c: Char): Boolean = reverse.containsKey(c)

    /**
     * Decode [byteLen] bytes of BK4 name text at [off]. Stops at the 0xFFFF
     * terminator; glyphs with no mapping render as '?' (they are never written
     * back, because a name is only re-encoded when the user edits it).
     */
    fun decodeGlyphs(buf: ByteArray, off: Int, byteLen: Int): String {
        val sb = StringBuilder(byteLen / 2)
        var i = 0
        while (i + 1 < byteLen) {
            val v = PbrBytes.readU16BE(buf, off + i)
            if (v == TERMINATOR) {
                break
            }
            val mapped = if (v < Gen4Glyphs.TABLE_INT.size) {
                Gen4Glyphs.TABLE_INT[v]
            } else {
                Gen4Glyphs.UNMAPPED
            }
            sb.append(
                when (mapped) {
                    Gen4Glyphs.HALF_MALE -> MALE_SIGN
                    Gen4Glyphs.HALF_FEMALE -> FEMALE_SIGN
                    Gen4Glyphs.UNMAPPED -> '?'
                    else -> mapped.toChar()
                }
            )
            i += 2
        }
        return sb.toString()
    }

    /**
     * Encode [text] into exactly [bufLen] bytes: up to [maxChars] glyphs, then
     * the 0xFFFF terminator, then NUL padding.
     *
     * @throws IllegalArgumentException if a character has no Gen 4 glyph.
     */
    fun encodeGlyphs(text: String, bufLen: Int, maxChars: Int): ByteArray {
        require(bufLen >= 2) { "name buffer too small" }
        val out = ByteArray(bufLen)
        val n = minOf(text.length, maxChars, (bufLen / 2) - 1)
        for (i in 0 until n) {
            val glyph = reverse[text[i]]
                ?: throw IllegalArgumentException("character '${text[i]}' has no Gen 4 glyph")
            PbrBytes.writeU16BE(out, i * 2, glyph)
        }
        PbrBytes.writeU16BE(out, n * 2, TERMINATOR)
        return out
    }

    /**
     * Decode [byteLen] bytes of raw UTF-16 big-endian text at [off], stopping
     * at NUL. 0xFFFF introduces a two-word variable/escape sequence, which is
     * skipped rather than rendered.
     */
    fun decodeUnicodeBE(buf: ByteArray, off: Int, byteLen: Int): String {
        val sb = StringBuilder(byteLen / 2)
        var i = 0
        while (i + 1 < byteLen) {
            val v = PbrBytes.readU16BE(buf, off + i)
            if (v == NUL) {
                break
            }
            if (v == TERMINATOR) {
                i += 2
                if (i + 1 >= byteLen || PbrBytes.readU16BE(buf, off + i) == TERMINATOR) {
                    break
                }
                i += 2
                continue
            }
            sb.append(v.toChar())
            i += 2
        }
        return sb.toString()
    }

    /** Encode [text] as UTF-16 big-endian into exactly [bufLen] NUL-padded bytes. */
    fun encodeUnicodeBE(text: String, bufLen: Int, maxChars: Int): ByteArray {
        require(bufLen >= 2) { "name buffer too small" }
        val out = ByteArray(bufLen)
        val n = minOf(text.length, maxChars, (bufLen / 2) - 1)
        for (i in 0 until n) {
            PbrBytes.writeU16BE(out, i * 2, text[i].code)
        }
        return out
    }
}
