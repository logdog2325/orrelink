// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay

/**
 * Kotlin side of Source/Android/jni/NetPlay/NetPlayIndexBridge.cpp — a thin client
 * for Dolphin's NetPlay session index ("lobby server").
 *
 * All list calls are blocking HTTP; call them from Dispatchers.IO only.
 *
 * Only sessions whose published version equals this build's version string
 * (Common::GetScmDescStr) are ever returned — anything else is unjoinable anyway.
 */
object NetPlayIndexBridge {

    /**
     * Lists open sessions, optionally filtered by (partial) session name, region code
     * and game string. Returns null on failure — see [lastError].
     *
     * Note on the game filter: the index's "game" field holds whatever name the host
     * published, not necessarily a 6-character game ID. Desktop hosts publish
     * "Long Name (GAMEID, Region)", but this fork's Android hosts publish the plain
     * long name (nativeChangeGame passes GameFile.getLongName()), which does NOT
     * contain "GXXE01". So for the default XD-only view, pass gameId = "" here and
     * filter client-side with [LobbySession.isXdBattle] instead.
     */
    fun listSessions(
        name: String = "",
        region: String = "",
        gameId: String = ""
    ): List<LobbySession>? =
        nativeListSessions(name, region, gameId)?.map { LobbySession.fromRow(it) }

    /** Error token from the last failed call: "NO_RESPONSE", "BAD_JSON", or a server status. */
    val lastError: String
        get() = nativeGetLastError()

    /** This build's version string as the index stores it (Common::GetScmDescStr). */
    val localVersion: String
        get() = nativeGetScmVersion()

    /**
     * Decrypts a password-protected session's server_id (XOR + checksum scheme from
     * NetPlaySession::DecryptID). Returns null if the password is wrong.
     */
    fun decryptServerId(encryptedServerId: String, password: String): String? =
        nativeDecryptServerId(encryptedServerId, password)

    /** Region (code, label) pairs accepted by the index, e.g. ("NA", "North America"). */
    fun regions(): List<Pair<String, String>> {
        val flat = nativeGetRegions()
        return (flat.indices step 2).map { flat[it] to flat[it + 1] }
    }

    private external fun nativeListSessions(
        name: String,
        region: String,
        gameId: String
    ): Array<Array<String>>?

    private external fun nativeGetLastError(): String

    private external fun nativeGetScmVersion(): String

    private external fun nativeDecryptServerId(serverId: String, password: String): String?

    private external fun nativeGetRegions(): Array<String>
}

/**
 * One row from the session index. Field order mirrors the String[10] encoding
 * produced by NetPlayIndexBridge.cpp.
 */
data class LobbySession(
    val name: String,
    val region: String,
    /** "traversal" (join via host code) or "direct" (join via IP). */
    val method: String,
    /** Traversal code or IP address; XOR-encrypted when [hasPassword] is true. */
    val serverId: String,
    /** The game string the host published (a name, not necessarily a game ID). */
    val game: String,
    val version: String,
    val playerCount: Int,
    val port: Int,
    val hasPassword: Boolean,
    val inGame: Boolean
) {
    val isTraversal: Boolean
        get() = method == "traversal"

    /**
     * True when this session looks like a Pokemon XD (GXXE01) battle. Matches both
     * desktop-published names ("... (GXXE01, GC)") and this fork's Android-published
     * long names ("Pokemon XD: Gale of Darkness").
     */
    val isXdBattle: Boolean
        get() {
            val g = game.lowercase()
            return g.contains("gxxe01") ||
                g.contains("gale of darkness") ||
                g.contains("pokemon xd") ||
                g.contains("pokémon xd")
        }

    companion object {
        fun fromRow(row: Array<String>): LobbySession = LobbySession(
            name = row[0],
            region = row[1],
            method = row[2],
            serverId = row[3],
            game = row[4],
            version = row[5],
            playerCount = row[6].toIntOrNull() ?: 0,
            port = row[7].toIntOrNull() ?: 0,
            hasPassword = row[8] == "1",
            inGame = row[9] == "1"
        )
    }
}
