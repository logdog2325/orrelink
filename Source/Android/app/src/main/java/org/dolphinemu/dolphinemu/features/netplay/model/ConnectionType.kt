// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.netplay.model

import androidx.annotation.StringRes
import org.dolphinemu.dolphinemu.R

sealed class ConnectionType(
    @StringRes val labelId: Int,
    val configValue: String,
) {
    object DirectConnection : ConnectionType(
        labelId = R.string.netplay_connection_type_direct_connection,
        configValue = "direct",
    )

    object TraversalServer : ConnectionType(
        labelId = R.string.netplay_connection_type_traversal_server,
        configValue = "traversal",
    )

    companion object {
        val all: List<ConnectionType>
            get() = listOf(DirectConnection, TraversalServer)

        // Never throw: this parses a PERSISTED config value, and other writers (upstream desktop
        // code, older builds, hand-edited INIs) are not bound by this enum. Throwing here crashed
        // the whole netplay setup screen on open -- an unrecognized string just means traversal,
        // the app's default.
        fun fromString(value: String): ConnectionType =
            all.find { it.configValue == value } ?: TraversalServer
    }
}
