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
) {
    companion object {
        // Same values as NetPlay::XdRole in Core/NetPlayClient.h.
        const val ROLE_UNKNOWN = 0
        const val ROLE_HOST = 1
        const val ROLE_OPPONENT = 2
        const val ROLE_WAITING = 3
        const val ROLE_WATCHING = 4
    }
}
