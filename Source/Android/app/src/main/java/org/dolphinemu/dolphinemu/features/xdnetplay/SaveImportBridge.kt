// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay

/**
 * Kotlin side of the save-import group in
 * Source/Android/jni/NetPlay/NetPlayIndexBridge.cpp — opt-in import of a
 * user's own Gen 3 save over a GBA socket's bundled team-editor save.
 *
 * This is a thin accessor, deliberately: ALL of the logic lives in the shared
 * core (UICommon/XDNetplay/SaveImport, the same code the desktop launcher
 * calls) — the validation (size, structure, checksums, save-game vs port-ROM
 * match), the refusals while emulation or a hosted room owns the save, the
 * once-only `<save>.preimport` backup, the tmp/readback/rename write, and the
 * per-port ImportedSave2/3 config bookkeeping. Every message that comes back
 * is user-displayable and identical across platforms.
 *
 * The one Android-specific rule callers must follow: the core reads plain
 * files, so a SAF content:// pick must be fully drained and copied to a real
 * path first (the ROM-picker idiom; see XDLauncherActivity.importUserSave).
 * Never hand a content:// string to [importUserSave].
 */
object SaveImportBridge {
    /** Result of an import/restore: [message] is user-displayable as-is. */
    data class Outcome(val ok: Boolean, val message: String)

    /** GBA port 2 — your side. Matches TeamRole.HOST.deviceNumber. */
    const val DEVICE_PORT_2 = 1

    /** GBA port 3 — the guest slot. Matches TeamRole.GUEST.deviceNumber. */
    const val DEVICE_PORT_3 = 2

    /**
     * Import the (already real-path) save file at [sourcePath] into GBA socket
     * [deviceNumber]. The source file is only ever read; whatever the socket
     * held is backed up once to `<save>.preimport`, which no cleanup path ever
     * deletes. On success [Outcome.message] carries the summary (game, trainer,
     * backup name, host-saves note); on refusal it says exactly why.
     */
    fun importUserSave(sourcePath: String, deviceNumber: Int): Outcome =
        parseOutcome(nativeImportUserSave(sourcePath, deviceNumber))

    /**
     * Put the bundled team-editor template back into socket [deviceNumber]'s
     * save and clear its ImportedSave config key. The replaced save is kept as
     * `<save>.bak`; `<save>.preimport` is never touched. On success
     * [Outcome.message] is empty — the caller shows its own localized string.
     */
    fun restoreDefaultSave(deviceNumber: Int): Outcome =
        parseOutcome(nativeRestoreDefaultSave(deviceNumber))

    /** Outcome encoding shared with the C++ side: ["1"/"0", message]. */
    private fun parseOutcome(flat: Array<String>): Outcome =
        Outcome(ok = flat.getOrNull(0) == "1", message = flat.getOrNull(1) ?: "")

    private external fun nativeImportUserSave(sourcePath: String, device: Int): Array<String>

    private external fun nativeRestoreDefaultSave(device: Int): Array<String>
}
