// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay

import org.dolphinemu.dolphinemu.features.settings.model.IntSetting
import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.Gen3Mon

/**
 * Kotlin side of the battle-FORMAT bridge in
 * Source/Android/jni/NetPlay/NetPlayIndexBridge.cpp for XD GBA-vs-GBA netplay.
 *
 * Everything about a format comes from shared core
 * (UICommon/XDNetplay/FormatRules): the ordered list every picker shows, the
 * display names, the lobby tags, which formats carry team rules, the fixed
 * level, and the ruleset itself. Nothing is mirrored here by hand, so the two
 * platforms cannot drift. Validation returned here is a NON-BLOCKING
 * paste-time note; the enforcing gates (the Start gate and the guest
 * submission gate in the host's TeamInjector) never consult this bridge.
 *
 * The Format key itself is plain config (Main / XDNetplay / Format), persisted
 * through the settings model as [IntSetting.MAIN_XD_FORMAT]
 * (org.dolphinemu.dolphinemu.features.settings.model.IntSetting) exactly like
 * the Battle Style picks. In a room the host changes it through
 * NetplaySession.setRoomFormat.
 *
 * The HOST's format governs a room; a joiner's own key only drives local notes
 * outside a room.
 */
object FormatBridge {
    /** Values of the Format config key, matching FormatRules::FORMAT_* in C++.
     *  Ids only: names, tags and rules come from shared core. */
    const val FORMAT_FREE = 0
    const val FORMAT_ORRE_COLOSSEUM = 1
    const val FORMAT_OU = 2
    const val FORMAT_ORRE_UNLIMITED = 3
    const val FORMAT_ORRE_LIMITED = 4
    const val FORMAT_HOENN_STADIUM = 5
    const val FORMAT_HOENN_UNLIMITED = 6
    const val FORMAT_HOENN_LIMITED = 7
    const val FORMAT_DOUBLES_OU = 8
    /** Reserved, not built yet: only listed while the MultiEnabled flag is on. */
    const val FORMAT_MULTI = 9

    /** True only for the exact OU value: an unknown key value behaves as Free. */
    fun isOu(formatKeyValue: Int): Boolean = formatKeyValue == FORMAT_OU

    /** The ordered list every picker shows (FormatRules::SelectableFormats with
     *  the MultiEnabled flag): Orre Colosseum, OU, Doubles OU, the Orre and
     *  Hoenn families, Multi only when enabled, then Free. */
    fun selectableFormats(): IntArray = nativeSelectableFormats()

    /** Every known format id, Multi included whatever the flag says. For
     *  recognizing names built from any format. */
    fun knownFormats(): IntArray = nativeKnownFormats()

    /** The fixed battle level a format pins: 100, 50, or 0 for none
     *  (FormatRules::FormatFixedLevel). */
    fun fixedLevel(formatKeyValue: Int): Int = nativeFixedLevel(formatKeyValue)

    /** True for every format with a party-legality layer
     *  (FormatRules::HasTeamRules). */
    fun hasTeamRules(formatKeyValue: Int): Boolean = nativeHasTeamRules(formatKeyValue)

    /** Display name for pickers, notes and messages
     *  (FormatRules::FormatDisplayName); unknown values read "Free". */
    fun displayName(formatKeyValue: Int): String = nativeDisplayName(formatKeyValue)

    /** Public-lobby session-name tag, brackets and trailing space included,
     *  "" for Free (FormatRules::FormatSessionTag). */
    fun sessionTag(formatKeyValue: Int): String = nativeSessionTag(formatKeyValue)

    /**
     * Legality check of a Showdown team export under [formatId] (by default
     * this device's own Format pick), using shared core's own parser and name
     * resolution (so "KYOGRE", "Mr. Mime" and "soul dew" resolve exactly as
     * they will at build time, and a set the builder would refuse anyway is
     * skipped rather than misreported). Returns "" when the paste is legal, or
     * unparseable, e.g. a pokepast.es LINK, which cannot be inspected without
     * fetching; the host's gate still enforces on the fetched text. Otherwise
     * one human-readable reason, e.g. "banned species: Kyogre",
     * "duplicate item: Leftovers (x2)" or "banned move: Explosion (Metagross)".
     */
    fun validateShowdown(text: String, formatId: Int = IntSetting.MAIN_XD_FORMAT.int): String =
        nativeValidateShowdown(text, formatId)

    /**
     * Legality check of built party mons (the team editor's in-memory party)
     * under this device's Format pick. Species travel as INTERNAL (Hoenn) ids
     * exactly as [Gen3Mon] holds them; shared core maps them to National dex
     * numbers before the ban list applies. Moves travel as four internal move
     * ids per mon (0 = empty slot) for the move bans. Returns "" when legal,
     * else one human-readable reason naming the mon, item or move.
     */
    fun validateParty(party: List<Gen3Mon>): String = nativeValidateParty(
        IntSetting.MAIN_XD_FORMAT.int,
        party.map { it.species }.toIntArray(),
        party.map { it.heldItem }.toIntArray(),
        party.map { it.level }.toIntArray(),
        party.flatMap { mon -> (0 until 4).map { mon.moves.getOrElse(it) { 0 } } }.toIntArray()
    )

    private external fun nativeSelectableFormats(): IntArray

    private external fun nativeKnownFormats(): IntArray

    private external fun nativeFixedLevel(formatKeyValue: Int): Int

    private external fun nativeHasTeamRules(formatKeyValue: Int): Boolean

    private external fun nativeDisplayName(formatKeyValue: Int): String

    private external fun nativeSessionTag(formatKeyValue: Int): String

    private external fun nativeValidateShowdown(text: String, formatId: Int): String

    private external fun nativeValidateParty(
        formatId: Int,
        species: IntArray,
        items: IntArray,
        levels: IntArray,
        moves: IntArray
    ): String
}
