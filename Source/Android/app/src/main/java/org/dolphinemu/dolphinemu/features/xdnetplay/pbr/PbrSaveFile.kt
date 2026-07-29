// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.pbr

import java.io.File
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization

/**
 * Where PBR keeps its save inside Dolphin's emulated NAND.
 *
 * Unlike the GBA saves the XD editor handles, this is not a loose file the user
 * picks: it is a Wii title data file, so it only exists after PBR has actually
 * been run and has created a profile (which for the online modes means running
 * it once while connected). Its path is
 *
 *   <user dir>/Wii/title/00010000/<title-lo>/data/GeniusPbr/PbrSaveData
 *
 * with title-lo the game ID's first four characters as lowercase hex — the same
 * `{}/title/{:08x}/{:08x}` layout Common/NandPaths.cpp builds natively.
 */
object PbrSaveFile {
    const val FILE_NAME = "PbrSaveData"

    /** Title-lo directory name -> the region it belongs to, in preference order. */
    val TITLES: List<Pair<String, String>> = listOf(
        "52504245" to "USA (RPBE01)",
        "52504250" to "Europe (RPBP01)",
        "5250424a" to "Japan (RPBJ01)"
    )

    data class Located(val file: File, val region: String)

    fun nandRoot(): File = File(DirectoryInitialization.getUserDirectory(), "Wii")

    fun candidates(): List<Located> = TITLES.map { (title, region) ->
        Located(
            File(
                nandRoot(),
                "title" + File.separator + "00010000" + File.separator + title +
                    File.separator + "data" + File.separator + "GeniusPbr" +
                    File.separator + FILE_NAME
            ),
            region
        )
    }

    /** The first region whose save actually exists, or null if PBR has none yet. */
    fun find(): Located? = candidates().firstOrNull { it.file.isFile }

    /** Human-readable list of the paths that were searched, for the "not found" UI. */
    fun searchedPaths(): List<String> = candidates().map { it.file.path }
}
