// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay

/**
 * Kotlin side of the battle-style group in
 * Source/Android/jni/NetPlay/NetPlayIndexBridge.cpp — the cosmetic selectors
 * (trainer models, battle music, battle location) for XD GBA-vs-GBA netplay.
 *
 * This is a thin accessor, deliberately: the option tables live ONLY in
 * UICommon/XDNetplay/BattleCustomizer.cpp and are fetched from native code, and
 * the actual AR-code assembly happens in shared core on the host at start time
 * (never here). The four host selections are stored in the native
 * MAIN_XD_STYLE_* config keys, which is exactly where the host-side assembly
 * reads them from — so a value set here is already "applied".
 *
 * An id of 0 always means "Game default" / "No preference": the corresponding
 * AR section is genuinely absent, never a write of the vanilla value.
 */
object BattleStyleBridge {
    /** One selectable option. Tables come tested-safe first, then
     *  [experimental] entries, so a dropdown can insert its divider at the
     *  first tier change. */
    data class StyleOption(
        val id: Int,
        val name: String,
        val experimental: Boolean
    )

    /** "which" indices shared with the C++ side of the bridge. */
    const val SELECTION_HOST_MODEL = 0
    const val SELECTION_GUEST_MODEL_FALLBACK = 1
    const val SELECTION_MUSIC = 2
    const val SELECTION_VENUE = 3

    /** Trainer models the host/guest can appear as. */
    fun modelTable(): List<StyleOption> = parseTable(nativeGetModelTable())

    /** Battle background music streams. */
    fun musicTable(): List<StyleOption> = parseTable(nativeGetMusicTable())

    /** Battle locations (battlefield-table indices). */
    fun venueTable(): List<StyleOption> = parseTable(nativeGetVenueTable())

    /** Current selection for one of the SELECTION_* slots; 0 = game default. */
    fun getSelection(which: Int): Int = nativeGetSelection(which)

    /**
     * Persist a selection (0 = game default). Validated native-side against the
     * same tables the host's code assembly validates against; an unknown id is
     * stored as 0, never clamped.
     */
    fun setSelection(which: Int, id: Int) = nativeSetSelection(which, id)

    /** Flat [id, name, tier, ...] triples from the C++ tables. */
    private fun parseTable(flat: Array<String>): List<StyleOption> =
        (flat.indices step 3).map {
            StyleOption(
                id = flat[it].toIntOrNull() ?: 0,
                name = flat[it + 1],
                experimental = flat[it + 2] == "experimental"
            )
        }

    private external fun nativeGetModelTable(): Array<String>

    private external fun nativeGetMusicTable(): Array<String>

    private external fun nativeGetVenueTable(): Array<String>

    private external fun nativeGetSelection(which: Int): Int

    private external fun nativeSetSelection(which: Int, id: Int)

    /**
     * Writes the Battle Style code block for a SOLO boot. Netplay hosting does
     * this on its own inside nativeStartGame; the solo path bypasses that, so
     * the launcher calls this right before launching emulation. With every
     * selector on "Game default" it is a no-op.
     */
    fun prepareForBoot() = nativePrepareForBoot()

    private external fun nativePrepareForBoot()
}
