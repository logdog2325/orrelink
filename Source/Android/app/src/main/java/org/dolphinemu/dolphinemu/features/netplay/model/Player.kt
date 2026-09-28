// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.netplay.model

import androidx.annotation.Keep

@Keep
data class Player(
    val pid: Int,
    val name: String,
    val revision: String,
    val ping: Int,
    val isHost: Boolean,
    val mapping: String,
    /** XD Netplay: this player's part in the next battle, one of the ROLE_ values below. */
    val role: Int = ROLE_UNKNOWN,
    /** True for this device's own player. */
    val isLocal: Boolean = false,
    /** XD Netplay: the seat (SI port) this player holds: 1 for the host of a Multi room, 2-4 for
     *  a seated joiner there, 3 for the 1v1 opponent, 0 for none. */
    val seat: Int = 0,
    /** XD Netplay: this player's seat has a team in. */
    val teamIn: Boolean = false,
) {
    companion object {
        // Same values as NetPlay::XdRole in Core/NetPlayClient.h.
        const val ROLE_UNKNOWN = 0
        const val ROLE_HOST = 1
        const val ROLE_OPPONENT = 2
        const val ROLE_WAITING = 3
        const val ROLE_WATCHING = 4
        const val ROLE_SEATED = 5
    }
}
