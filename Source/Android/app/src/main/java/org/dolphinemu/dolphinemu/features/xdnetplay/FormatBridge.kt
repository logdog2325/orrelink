// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay

import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.Gen3Mon

/**
 * Kotlin side of the battle-FORMAT bridge in
 * Source/Android/jni/NetPlay/NetPlayIndexBridge.cpp — the one-tap Format pick
 * (Free / Orre Colosseum) for XD GBA-vs-GBA netplay.
 *
 * This exposes VALIDATION only, and validation is deliberately not duplicated
 * in Kotlin: the ruleset — the restricted-species ban list, Species Clause,
 * Item Clause, the Soul Dew ban — lives ONLY in shared core
 * (UICommon/XDNetplay/FormatRules), which is also what the two enforcing gates
 * run: the host gate inside nativeHost and the guest-submission gate inside
 * the host's TeamInjector. Everything returned here is a NON-BLOCKING
 * paste-time note; the gates never consult this bridge.
 *
 * The Format key itself is plain config (Main / XDNetplay / Format), persisted
 * through the settings model as [IntSetting.MAIN_XD_FORMAT]
 * (org.dolphinemu.dolphinemu.features.settings.model.IntSetting) exactly like
 * the Battle Style picks — no native setter lives here.
 *
 * The HOST's key governs a room; a joiner's own key only drives their local
 * paste-time notes.
 */
object FormatBridge {
    /** Values of the Format config key, matching FormatRules::FORMAT_* in C++. */
    const val FORMAT_FREE = 0
    const val FORMAT_ORRE_COLOSSEUM = 1

    /**
     * True only for the exact Orre Colosseum value, matching
     * FormatRules::IsOrreColosseum: an unknown/garbage key value behaves as
     * Free (no notes, no enforcement), never as a surprise lockout.
     */
    fun isOrreColosseum(formatKeyValue: Int): Boolean =
        formatKeyValue == FORMAT_ORRE_COLOSSEUM

    /**
     * Orre Colosseum check of a Showdown team export, using shared core's own
     * parser and name resolution (so "KYOGRE", "Mr. Mime" and "soul dew"
     * resolve exactly as they will at build time, and a set the builder would
     * refuse anyway is skipped rather than misreported). Returns "" when the
     * paste is legal — or unparseable, e.g. a pokepast.es LINK, which cannot
     * be inspected without fetching; the host's gate still enforces on the
     * fetched text. Otherwise one human-readable reason, e.g.
     * "banned species: Kyogre" or "duplicate item: Leftovers (x2)".
     */
    fun validateShowdown(text: String): String = nativeValidateShowdown(text)

    /**
     * Orre Colosseum check of built party mons (the team editor's in-memory
     * party). Species travel as INTERNAL (Hoenn) ids exactly as [Gen3Mon]
     * holds them; shared core maps them to National dex numbers before the ban
     * list applies. Returns "" when legal, else one human-readable reason
     * naming the mon or item.
     */
    fun validateParty(party: List<Gen3Mon>): String = nativeValidateParty(
        party.map { it.species }.toIntArray(),
        party.map { it.heldItem }.toIntArray()
    )

    private external fun nativeValidateShowdown(text: String): String

    private external fun nativeValidateParty(species: IntArray, items: IntArray): String
}
