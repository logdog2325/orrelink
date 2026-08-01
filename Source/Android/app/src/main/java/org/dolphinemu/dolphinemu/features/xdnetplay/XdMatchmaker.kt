// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay

import org.dolphinemu.dolphinemu.features.settings.model.IntSetting
import org.dolphinemu.dolphinemu.features.settings.model.NativeConfig
import org.dolphinemu.dolphinemu.features.settings.model.Settings
import org.dolphinemu.dolphinemu.features.settings.model.StringSetting

/**
 * The rules and config plumbing behind "Search for Match", shared by the XD
 * launcher's one-button flow and the Find Battles lobby browser.
 *
 * Nothing here touches the UI or blocks: [pickMatch] is pure, and the two
 * apply* helpers only write settings. The blocking index call
 * ([NetPlayIndexBridge.listSessions]) and the actual join stay with the caller,
 * because they need a coroutine scope and an Activity.
 *
 * The desktop counterpart is Source/Core/DolphinQt/XDNetplay — the naming
 * convention lives in XDNetplayConfig.{h,cpp} (MakeOpenSessionName /
 * LooksLikeXdSession) and the selection rule in XDLauncherDialog.cpp
 * (PickMatch). The two must agree or the platforms will not find each other.
 */
object XdMatchmaker {

    /**
     * Session-name format for every XD room either launcher publishes to
     * Dolphin's public session index:
     *
     *     XD [OC] <nickname>            ("OC" = Open Challenge)
     *
     * The IDENTICAL format lives in XDNetplayConfig.cpp::MakeOpenSessionName on
     * desktop. Keep the two in sync — it is the only thing that lets a human
     * scanning the lobby and the auto-matcher agree on what an XD room is.
     *
     * Matching itself never keys off the name (see [pickMatch]); the tag is for
     * people, so a differently named open room is still joinable.
     */
    const val SESSION_NAME_PREFIX = "XD [OC] "

    const val FILE = Settings.FILE_DOLPHIN
    const val SECTION = Settings.SECTION_INI_NETPLAY
    const val KEY_USE_INDEX = "UseIndex"
    const val KEY_INDEX_NAME = "IndexName"
    const val KEY_INDEX_REGION = "IndexRegion"
    const val KEY_INDEX_PASSWORD = "IndexPassword"

    /** Default publish region. Only ever a browser filter — [pickMatch] ignores it. */
    const val DEFAULT_REGION = "NA"

    fun sessionName(nickname: String): String =
        SESSION_NAME_PREFIX + nickname.ifEmpty { "Player" }

    /**
     * Picks the room "Search for Match" should join, or null to host instead.
     *
     * The rules are deliberately dumb, so that both platforms reach the same
     * answer and a bad match is reproducible rather than a coin flip:
     *  - same build — netplay only connects between identical git hashes, and
     *    the index keys on the build's version string, so a foreign build's
     *    room is unjoinable by construction. [NetPlayIndexBridge] already
     *    filters this natively; re-checking is free.
     *  - looks like Pokemon XD (the index's "game" field is a display name, not
     *    a game id — see [LobbySession.isXdBattle]).
     *  - exactly one player waiting. Zero is a room mid-teardown; two is a
     *    battle already under way, and XD's GBA link is strictly two-player.
     *  - not in game, not password protected.
     *  - first hit in index order wins, i.e. whoever has been waiting longest.
     */
    fun pickMatch(sessions: List<LobbySession>): LobbySession? {
        val localVersion = NetPlayIndexBridge.localVersion
        return sessions.firstOrNull { session ->
            session.version == localVersion &&
                session.isXdBattle &&
                session.playerCount == 1 &&
                !session.inGame &&
                !session.hasPassword
        }
    }

    /**
     * Writes the config a join needs — the same handoff desktop's
     * NetPlayBrowser::accept() performs. [serverId] is passed separately so a
     * password-protected room can hand in its decrypted id; auto-matched rooms
     * never have one, since [pickMatch] drops them.
     */
    fun applyJoinConfig(session: LobbySession, serverId: String = session.serverId) {
        StringSetting.NETPLAY_TRAVERSAL_CHOICE.setString(NativeConfig.LAYER_BASE, session.method)
        IntSetting.NETPLAY_CONNECT_PORT.setInt(NativeConfig.LAYER_BASE, session.port)
        if (session.isTraversal) {
            StringSetting.NETPLAY_HOST_CODE.setString(NativeConfig.LAYER_BASE, serverId)
        } else {
            StringSetting.NETPLAY_ADDRESS.setString(NativeConfig.LAYER_BASE, serverId)
        }
    }

    /**
     * Turns on index publishing for the host side and saves. Core's
     * NetPlayServer::SetupIndex() picks these up when the room opens, keeps the
     * entry fresh from the server ping loop, and unlists it when hosting stops.
     * It refuses to publish at all with an empty name or region, hence the
     * fallbacks.
     */
    fun applyHostConfig(
        name: String = sessionName(StringSetting.NETPLAY_NICKNAME.string),
        region: String = DEFAULT_REGION,
        password: String = ""
    ) {
        NativeConfig.setBoolean(NativeConfig.LAYER_BASE, FILE, SECTION, KEY_USE_INDEX, true)
        NativeConfig.setString(
            NativeConfig.LAYER_BASE, FILE, SECTION, KEY_INDEX_NAME,
            name.ifEmpty { sessionName(StringSetting.NETPLAY_NICKNAME.string) }
        )
        NativeConfig.setString(
            NativeConfig.LAYER_BASE, FILE, SECTION, KEY_INDEX_REGION,
            region.ifEmpty { DEFAULT_REGION }
        )
        NativeConfig.setString(NativeConfig.LAYER_BASE, FILE, SECTION, KEY_INDEX_PASSWORD, password)
        // Traversal needs no port forwarding — the sane default on mobile.
        StringSetting.NETPLAY_TRAVERSAL_CHOICE.setString(NativeConfig.LAYER_BASE, "traversal")
        NativeConfig.save(NativeConfig.LAYER_BASE)
    }
}
