// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.gen3

/**
 * Replicates Dolphin's HW::GBA::Core::GetSavePath (Source/Core/Core/HW/GBACore.cpp)
 * for the SavesInRomPath=false case, so the Android side derives exactly the
 * same .sav filename the core will open.
 */
object SaveNaming {
    /**
     * Kotlin equivalent of:
     *
     *   save_path = fmt::format("{}-{}.sav",
     *       rom_path.substr(0, rom_path.find_last_of('.')), device_number + 1);
     *   save_path = gba_saves_dir + save_path.substr(save_path.find_last_of("/\\") + 1);
     *
     * Byte-for-byte string behavior is preserved, including the C++ npos edge
     * cases (no '.' -> whole string kept; no separator -> whole string is the
     * filename) and the quirk that a '.' in a directory component of an
     * extensionless path truncates there. content:// URIs are treated as
     * opaque strings: percent-encodings such as %2F are NOT decoded, matching
     * the C++ core's byte-level handling.
     *
     * @param gbaSavesDir the GBA saves directory (with or without a trailing
     *        separator; C++ File::GetUserPath returns it with one)
     * @param romPath the ROM path/URI as passed to the core
     * @param deviceNumber 0-based GBA device number (player index)
     */
    fun deriveSavePath(gbaSavesDir: String, romPath: String, deviceNumber: Int): String {
        // rom_path.substr(0, rom_path.find_last_of('.')): with no '.',
        // find_last_of returns npos and substr keeps the whole string.
        val dot = romPath.lastIndexOf('.')
        val stem = if (dot >= 0) romPath.substring(0, dot) else romPath
        val savePath = stem + "-" + (deviceNumber + 1) + ".sav"

        // save_path.substr(save_path.find_last_of("/\\") + 1): with no
        // separator, npos + 1 wraps to 0 and the whole string is kept —
        // lastIndexOf's -1 + 1 = 0 matches that exactly.
        val sep = maxOf(savePath.lastIndexOf('/'), savePath.lastIndexOf('\\'))
        val fileName = savePath.substring(sep + 1)

        val dir = if (gbaSavesDir.endsWith("/") || gbaSavesDir.endsWith("\\")) {
            gbaSavesDir
        } else {
            gbaSavesDir + "/"
        }
        return dir + fileName
    }
}
