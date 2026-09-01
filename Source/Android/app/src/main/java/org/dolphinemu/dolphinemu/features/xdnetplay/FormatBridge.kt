// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay

import org.dolphinemu.dolphinemu.features.xdnetplay.gen3.Gen3Mon

/**
 * Kotlin side of the battle-FORMAT bridge in
 * Source/Android/jni/NetPlay/NetPlayIndexBridge.cpp — the one-tap Format pick
 * (Free / OU / the six community formats) for XD GBA-vs-GBA netplay.
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

    /** The community "$XD OU Fixes" patches (bring 6 pick 4). No legality
     *  layer — like Free it produces no notes; shared core enables the
     *  Sys-bundled OU Fixes code on the host when this is the pick (the
     *  Format dropdown replaced the old standalone OU switch). */
    const val FORMAT_OU = 2

    /**
     * True only for the exact Orre Colosseum value, matching
     * FormatRules::IsOrreColosseum: an unknown/garbage key value behaves as
     * Free (no notes, no enforcement), never as a surprise lockout.
     */
    fun isOrreColosseum(formatKeyValue: Int): Boolean =
        formatKeyValue == FORMAT_ORRE_COLOSSEUM

    /** True only for the exact OU value — same unknown-behaves-as-Free rule. */
    fun isOu(formatKeyValue: Int): Boolean = formatKeyValue == FORMAT_OU

    /** The fixed battle level a format pins: 100 for the Standard/Unlimited
     *  formats, 50 for the Limited ones, 0 for Free/OU/unknown. Mirrors
     *  FormatRules::FormatFixedLevel — keep in sync. */
    fun fixedLevel(formatKeyValue: Int): Int = when (formatKeyValue) {
        FORMAT_ORRE_COLOSSEUM, FORMAT_ORRE_UNLIMITED,
        FORMAT_HOENN_STADIUM, FORMAT_HOENN_UNLIMITED -> 100
        FORMAT_ORRE_LIMITED, FORMAT_HOENN_LIMITED -> 50
        else -> 0
    }

    /** The Orre community's six settled formats: three rulesets (Standard =
     *  Orre Colosseum's rules; Unlimited = everything allowed, Soul Dew
     *  legal, clauses still apply; Limited = level 50 with ALL legendaries
     *  banned) times two entry shapes (Orre = bring 6 pick 4; Hoenn = bring
     *  6 pick 3). Values match FormatRules::FORMAT_* in C++. */
    const val FORMAT_ORRE_UNLIMITED = 3
    const val FORMAT_ORRE_LIMITED = 4
    const val FORMAT_HOENN_STADIUM = 5
    const val FORMAT_HOENN_UNLIMITED = 6
    const val FORMAT_HOENN_LIMITED = 7

    /** Every selectable format id, dropdown order. Mirrors the desktop combo. */
    val ALL_FORMATS = intArrayOf(
        FORMAT_FREE, FORMAT_ORRE_COLOSSEUM, FORMAT_ORRE_UNLIMITED, FORMAT_ORRE_LIMITED,
        FORMAT_HOENN_STADIUM, FORMAT_HOENN_UNLIMITED, FORMAT_HOENN_LIMITED, FORMAT_OU
    )

    /** True for every format with a party-legality layer (the six community
     *  formats). Mirrors FormatRules::HasTeamRules — keep in sync. */
    fun hasTeamRules(formatKeyValue: Int): Boolean = when (formatKeyValue) {
        FORMAT_ORRE_COLOSSEUM, FORMAT_ORRE_UNLIMITED, FORMAT_ORRE_LIMITED,
        FORMAT_HOENN_STADIUM, FORMAT_HOENN_UNLIMITED, FORMAT_HOENN_LIMITED -> true
        else -> false
    }

    /** Display name for notes and messages. Mirrors
     *  FormatRules::FormatDisplayName — keep in sync. */
    fun displayName(formatKeyValue: Int): String = when (formatKeyValue) {
        FORMAT_ORRE_COLOSSEUM -> "Orre Colosseum"
        FORMAT_OU -> "OU"
        FORMAT_ORRE_UNLIMITED -> "Orre Unlimited"
        FORMAT_ORRE_LIMITED -> "Orre Limited"
        FORMAT_HOENN_STADIUM -> "Hoenn Stadium"
        FORMAT_HOENN_UNLIMITED -> "Hoenn Unlimited"
        FORMAT_HOENN_LIMITED -> "Hoenn Limited"
        else -> "Free"
    }

    /** Public-lobby session-name tag, brackets and trailing space included,
     *  "" for Free. Mirrors FormatRules::FormatSessionTag — keep in sync. */
    fun sessionTag(formatKeyValue: Int): String = when (formatKeyValue) {
        FORMAT_ORRE_COLOSSEUM -> "[Orre] "
        FORMAT_OU -> "[OU] "
        FORMAT_ORRE_UNLIMITED -> "[Orre-U] "
        FORMAT_ORRE_LIMITED -> "[Orre-L] "
        FORMAT_HOENN_STADIUM -> "[Hoenn] "
        FORMAT_HOENN_UNLIMITED -> "[Hoenn-U] "
        FORMAT_HOENN_LIMITED -> "[Hoenn-L] "
        else -> ""
    }

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
        party.map { it.heldItem }.toIntArray(),
        party.map { it.level }.toIntArray()
    )

    private external fun nativeValidateShowdown(text: String): String

    private external fun nativeValidateParty(
        species: IntArray,
        items: IntArray,
        levels: IntArray
    ): String
}
