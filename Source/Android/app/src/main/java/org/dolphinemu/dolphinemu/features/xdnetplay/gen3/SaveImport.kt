// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.gen3

import java.io.File
import java.io.RandomAccessFile

/**
 * Kotlin mirror of UICommon/XDNetplay/SaveImport.{h,cpp}: opt-in import of a
 * user's own Gen 3 save into a GBA socket, replacing the bundled team-editor
 * save. Guarantees, in order of importance:
 *  - the bytes the user picked are only ever READ (the launcher hands them in
 *    after fully draining the SAF stream; nothing here re-opens the source);
 *  - whatever occupied the socket is backed up ONCE to `<save>.preimport`, a
 *    name no cleanup path (TeamInjector's purge included) ever deletes, and a
 *    re-import never overwrites that first backup;
 *  - nothing lands in the socket unless the image validated in memory first
 *    (size, structure, checksums, and game-vs-ROM match).
 *
 * Pure JVM on purpose: callers supply the resolved save path (via
 * [SaveNaming.deriveSavePath]), the ROM path, the bundled template bytes, and
 * the room state. Config bookkeeping (StringSetting.MAIN_XD_IMPORTED_SAVE_*)
 * and the emulation-not-running precondition are the caller's job.
 * Error and status strings are shared verbatim with the desktop build.
 */
object SaveImport {
    /**
     * Backup of whatever occupied a GBA socket before the FIRST user import.
     * PROVABLY outside TeamInjector's cleanup set (.hostteam / .guestteam /
     * .bak / .tmp / NetPlayTemp*.sav): no code path in the tree deletes a
     * .preimport file, so the pre-import world survives every session, purge,
     * and re-import.
     */
    const val PREIMPORT_SUFFIX = ".preimport"

    // TeamInjector's session bookkeeping files. MUST stay in step with the
    // suffix constants in TeamInjector.cpp: their presence next to a socket
    // save means a room's injection round trip owns that file right now.
    private const val HOST_STASH_SUFFIX = ".hostteam"
    private const val GUEST_MARK_SUFFIX = ".guestteam"

    /**
     * The full 128 KiB flash image every Gen 3 cartridge writes. EmeraldSave
     * only needs the two game-save blocks, but an import always stores the
     * whole flash so the Hall of Fame / e-Reader sectors and any emulator
     * footer ride along verbatim.
     */
    private const val FULL_FLASH_SIZE = 131072

    /** A 64 KiB dump holds a single bank: block 0 plus the start of block 1. */
    private const val SINGLE_BANK_SIZE = 65536

    /**
     * Requirement 5, appended to every success status: an import must never
     * leave a joiner believing their file reaches a host's room.
     */
    private const val HOST_SAVES_NOTE =
        "Solo play uses this save as-is. Hosting a netplay room shares only a rebuilt save carrying its party and trainer identity - never the full file." +
            "solo or host; when you join someone's room, use Submit Team instead."

    private const val RS_REVISION_NOTE =
        "Note: Ruby/Sapphire revisions (1.0/1.1/1.2) also differ — use a save from your exact " +
            "ROM revision."

    /** What [validateGen3SaveImage] learned about an accepted image. */
    class SaveValidation(
        val game: Gen3Game,
        /** Byte count of the file as picked, before any padding. */
        val originalSize: Int,
        /**
         * A 65536-byte single-bank file was 0xFF-padded (flash erase state) up
         * to the full 131072; block 0 carries the whole save in that case.
         */
        val paddedFrom64k: Boolean,
        /** Always 0 on an accepted image -- any mismatch refuses it. */
        val badChecksums: Int,
        /**
         * Exactly the image an import may write (the C++ side pads its in/out
         * buffer instead; ByteArray is fixed-size, so the copy lives here).
         */
        val bytes: ByteArray
    )

    /**
     * Pure, no I/O. Size gate (exactly 65536, or >= 131072 with any trailing
     * emulator footer preserved) -> optional 0xFF pad to 131072 -> structure
     * parse -> game detection -> game-aware checksum verification.
     *
     * @throws IllegalArgumentException with a user-displayable reason.
     */
    fun validateGen3SaveImage(bytes: ByteArray): SaveValidation {
        val originalSize = bytes.size
        var paddedFrom64k = false
        val image = when {
            bytes.size == SINGLE_BANK_SIZE -> {
                // 0xFF is flash erase state: the padded second bank scans as
                // invalid and block 0 becomes the active block, exactly like a
                // once-saved cartridge.
                paddedFrom64k = true
                ByteArray(FULL_FLASH_SIZE) { i ->
                    if (i < bytes.size) bytes[i] else 0xFF.toByte()
                }
            }
            bytes.size < FULL_FLASH_SIZE -> throw IllegalArgumentException(
                "That file is not a Gen 3 save: expected 131,072 bytes of save data (128 KiB, " +
                    "optionally with an emulator footer), got $originalSize bytes."
            )
            else -> bytes.copyOf()
        }

        val save = try {
            EmeraldSave(image)
        } catch (_: IllegalArgumentException) {
            throw IllegalArgumentException(
                "No valid save block found — the file may be corrupt, or from a different " +
                    "game/emulator format."
            )
        }

        val game = EmeraldSave.detectGame(save)
        val bad = save.verifyAllChecksums(game)
        if (bad.isNotEmpty()) {
            throw IllegalArgumentException(
                "${bad.size} section checksum(s) are invalid — the save looks corrupt, so it " +
                    "was not imported."
            )
        }
        return SaveValidation(game, originalSize, paddedFrom64k, 0, image)
    }

    /**
     * The Gen 3 game a GBA ROM is, from the 4-character game code at header
     * 0xAC..0xAF ("BPE*" Emerald, "AXV*"/"AXP*" Ruby/Sapphire, "BPR*"/"BPG*"
     * FireRed/LeafGreen; the fourth letter is only the language). The
     * launcher's manual picker accepts any GBA ROM, so this cannot assume
     * Emerald.
     *
     * @throws IllegalArgumentException for unreadable files and non-Pokemon ROMs.
     */
    fun detectRomGame(romPath: String): Gen3Game {
        val code = ByteArray(4)
        try {
            RandomAccessFile(romPath, "r").use { rom ->
                require(rom.length() >= 0xC0)
                rom.seek(0xAC)
                rom.readFully(code)
            }
        } catch (_: Exception) {
            throw IllegalArgumentException("could not read the ROM header of $romPath")
        }

        val id = String(code, Charsets.US_ASCII)
        return when (id.substring(0, 3)) { // fourth letter = language
            "AXV", "AXP" -> Gen3Game.RubySapphire
            "BPR", "BPG" -> Gen3Game.FireRedLeafGreen
            "BPE" -> Gen3Game.Emerald
            else -> {
                val printable = id.map { if (it.code in 0x20..0x7E) it else '?' }
                    .joinToString("")
                throw IllegalArgumentException(
                    "this ROM is not a Gen 3 Pokemon game (game code \"$printable\"), so there " +
                        "is nothing to match the save against"
                )
            }
        }
    }

    /**
     * The whole no-destruction import flow for GBA socket [deviceNumber]
     * (1 = port 2, the user's side; 2 = port 3, the guest slot). [sourceBytes]
     * must be the fully drained content of the picked file (a SAF content://
     * stream can silently truncate, so the byte COUNT is validated here before
     * anything is written -- never stream-copy into the save slot).
     *
     * Returns a user-displayable success summary.
     *
     * @throws IllegalArgumentException with a user-displayable refusal.
     */
    fun importUserSave(
        sourceBytes: ByteArray,
        sourceDisplayName: String,
        savePath: File,
        romPath: String,
        deviceNumber: Int,
        isHostingRoom: Boolean
    ): String {
        require(deviceNumber == 1 || deviceNumber == 2) {
            "no such GBA socket ($deviceNumber)"
        }
        refuseIfGuestSlotIsLive(deviceNumber, savePath, isHostingRoom)

        val validation = validateGen3SaveImage(sourceBytes)
        val romGame = detectRomGame(romPath)
        if (validation.game != romGame) {
            var message =
                "That save is from ${validation.game.displayName}, but this GBA port runs " +
                    "${romGame.displayName}. The game itself would refuse a mismatched save, " +
                    "so it was not imported."
            if (validation.game == Gen3Game.RubySapphire) {
                message += " $RS_REVISION_NOTE"
            }
            throw IllegalArgumentException(message)
        }

        // First backup wins, forever: a re-import must never overwrite the
        // record of what the socket held before the FIRST import.
        val backup = File(savePath.path + PREIMPORT_SUFFIX)
        val backupExisted = backup.exists()
        val hadPrevious = savePath.exists()
        if (hadPrevious && !backupExisted) {
            try {
                savePath.copyTo(backup)
            } catch (e: Exception) {
                throw IllegalArgumentException(
                    "could not back up the current save to ${backup.path} — nothing was changed"
                )
            }
        }

        writeFileVerified(savePath, validation.bytes)

        val trainer = try {
            // The trainer-name field lives at the same section-0 offset in all
            // five cartridges, so it is worth showing whatever the game is.
            EmeraldSave(validation.bytes).trainerName
        } catch (_: Exception) {
            "?"
        }
        var status = "Imported $sourceDisplayName (${validation.game.displayName}, " +
            "trainer $trainer)."
        if (validation.paddedFrom64k) {
            status += " The 64 KiB file was padded to the full 128 KiB flash size."
        }
        if (backupExisted) {
            status += " Your original save from before the first import is kept as ${backup.name}."
        } else if (hadPrevious) {
            status += " Your previous save is kept as ${backup.name}."
        }
        if (validation.game == Gen3Game.RubySapphire) {
            status += " $RS_REVISION_NOTE"
        }
        return status + "\n\n" + HOST_SAVES_NOTE
    }

    /**
     * Put the bundled team-editor template ([templateBytes], read from the
     * role's asset) back into the socket's save. The replaced file is kept as
     * `<save>.bak` (one-level undo, same as every verified write);
     * `<save>.preimport` is deliberately left alone -- it belongs to the
     * pre-import world and nothing may delete it. The caller clears the
     * per-port config key on success.
     *
     * @throws IllegalArgumentException with a user-displayable refusal.
     */
    fun restoreDefaultSave(
        savePath: File,
        templateBytes: ByteArray,
        deviceNumber: Int,
        isHostingRoom: Boolean
    ) {
        require(deviceNumber == 1 || deviceNumber == 2) {
            "no such GBA socket ($deviceNumber)"
        }
        refuseIfGuestSlotIsLive(deviceNumber, savePath, isHostingRoom)
        writeFileVerified(savePath, templateBytes)
    }

    /**
     * True when `<save>.preimport` exists for the socket. This -- not the
     * config key, which is display-only -- is the authoritative "an import
     * happened" signal, so a hand-deleted file cannot desync the UI.
     */
    fun hasImportBackup(savePath: File): Boolean =
        File(savePath.path + PREIMPORT_SUFFIX).exists()

    /**
     * Never touch the guest slot while a room's injection bookkeeping owns it
     * (the purge would later copy the stash straight over whatever was
     * written).
     */
    private fun refuseIfGuestSlotIsLive(
        deviceNumber: Int,
        savePath: File,
        isHostingRoom: Boolean
    ) {
        if (deviceNumber == 2 &&
            (isHostingRoom ||
                File(savePath.path + GUEST_MARK_SUFFIX).exists() ||
                File(savePath.path + HOST_STASH_SUFFIX).exists())
        ) {
            throw IllegalArgumentException(
                "You are hosting a room right now — the guest slot holds your opponent's team. " +
                    "Close the room, then import."
            )
        }
    }

    /**
     * The tmp/readback/rename discipline of TeamRepo.saveToDisk (and the C++
     * VerifiedWriteSaveFile), minus the Emerald-only re-parse -- an already
     * validated RS/FRLG image must pass. The destination is never touched
     * until the tmp has been read back byte-identical, and an existing
     * destination is kept as one-level .bak undo.
     */
    private fun writeFileVerified(path: File, bytes: ByteArray) {
        path.parentFile?.mkdirs()
        val tmp = File(path.path + ".tmp")
        tmp.writeBytes(bytes)
        if (!tmp.readBytes().contentEquals(bytes)) {
            tmp.delete()
            throw IllegalArgumentException("tmp readback mismatch for ${tmp.path}")
        }
        if (path.exists()) {
            try {
                path.copyTo(File(path.path + ".bak"), overwrite = true)
            } catch (e: Exception) {
                tmp.delete()
                throw IllegalArgumentException("could not back up ${path.path}")
            }
        }
        if (!tmp.renameTo(path)) {
            throw IllegalArgumentException("could not rename ${tmp.path} into place")
        }
    }
}
