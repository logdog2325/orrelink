// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.gen3

/**
 * Base PP per internal Gen 3 move id, and the PP a move has with PP Ups on it.
 *
 * Generation 3 values, which differ from later games for some moves (Giga
 * Drain is 5 here, Recover 20). Extracted by script from the Emerald move
 * table and checked entry by entry against a second, independent source; the
 * two agree on all 355 ids. Kept in step with MOVE_BASE_PP in Gen3Data.cpp.
 */
object Gen3MovePp {
    /** Index = move id, 0..354. Index 0 is "no move". */
    private val BASE_PP = intArrayOf(
         0, 35, 25, 10, 15, 20, 20, 15, 15, 15, 35, 30,  5, 10, 30, 30, 35, 35, 20, 15,
        20, 20, 10, 20, 30,  5, 25, 15, 15, 15, 25, 20,  5, 35, 15, 20, 20, 20, 15, 30,
        35, 20, 20, 30, 25, 40, 20, 15, 20, 20, 20, 30, 25, 15, 30, 25,  5, 15, 10,  5,
        20, 20, 20,  5, 35, 20, 25, 20, 20, 20, 15, 20, 10, 10, 40, 25, 10, 35, 30, 15,
        20, 40, 10, 15, 30, 15, 20, 10, 15, 10,  5, 10, 10, 25, 10, 20, 40, 30, 30, 20,
        20, 15, 10, 40, 15, 20, 30, 20, 20, 10, 40, 40, 30, 30, 30, 20, 30, 10, 10, 20,
         5, 10, 30, 20, 20, 20,  5, 15, 10, 20, 15, 15, 35, 20, 15, 10, 20, 30, 15, 40,
        20, 15, 10,  5, 10, 30, 10, 15, 20, 15, 40, 40, 10,  5, 15, 10, 10, 10, 15, 30,
        30, 10, 10, 20, 10,  1,  1, 10, 10, 10,  5, 15, 25, 15, 10, 15, 30,  5, 40, 15,
        10, 25, 10, 30, 10, 20, 10, 10, 10, 10, 10, 20,  5, 40,  5,  5, 15,  5, 10,  5,
        15, 10,  5, 10, 20, 20, 40, 15, 10, 20, 20, 25,  5, 15, 10,  5, 20, 15, 20, 25,
        20,  5, 30,  5, 10, 20, 40,  5, 20, 40, 20, 15, 35, 10,  5,  5,  5, 15,  5, 20,
         5,  5, 15, 20, 10,  5,  5, 15, 15, 15, 15, 10, 10, 10, 10, 10, 10, 10, 10, 15,
        15, 15, 10, 20, 20, 10, 20, 20, 20, 20, 20, 10, 10, 10, 20, 20,  5, 15, 10, 10,
        15, 10, 20,  5,  5, 10, 10, 20,  5, 10, 20, 10, 20, 20, 20,  5,  5, 15, 20, 10,
        15, 20, 15, 10, 10, 15, 10,  5,  5, 10, 15, 10,  5, 20, 25,  5, 40, 10,  5, 40,
        15, 20, 20,  5, 15, 20, 30, 15, 15,  5, 10, 30, 20, 30, 15,  5, 40, 15,  5, 20,
         5, 15, 25, 40, 15, 20, 15, 20, 15, 20, 10, 20, 20,  5,  5,
    )

    /** Base PP (no PP Ups). 0 for "no move" and for an id outside the table. */
    fun basePp(moveId: Int): Int = if (moveId in 1 until BASE_PP.size) BASE_PP[moveId] else 0

    /**
     * PP with [ppUps] PP Ups (0..3), the games' own formula: every PP Up adds a
     * fifth of the base value, rounded down.
     */
    fun maxPp(moveId: Int, ppUps: Int): Int {
        val base = basePp(moveId)
        return base + base * ppUps.coerceIn(0, 3) / 5
    }
}
