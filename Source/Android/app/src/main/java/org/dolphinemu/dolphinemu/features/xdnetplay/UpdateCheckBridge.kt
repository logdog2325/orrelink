// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay

/**
 * Kotlin side of the release check in Source/Android/jni/NetPlay/NetPlayIndexBridge.cpp, which
 * forwards to UICommon/XDNetplay/UpdateCheck so desktop and Android reach the same verdict.
 *
 * [check] is a blocking HTTP call; call it from Dispatchers.IO only.
 */
object UpdateCheckBridge {

    /** The XD Netplay release this build belongs to (XDNetplay::VERSION). */
    val localVersion: String
        get() = nativeGetVersion()

    /**
     * Asks GitHub for the newest published release and compares it against [localVersion].
     * Never reports [UpdateStatus.UP_TO_DATE] for a check that did not complete.
     */
    fun check(): UpdateCheckResult {
        val row = nativeCheckForUpdate()
        if (row.size < 6) {
            return UpdateCheckResult(
                status = UpdateStatus.UNKNOWN,
                message = "The update check did not return a result."
            )
        }
        return UpdateCheckResult(
            status = UpdateStatus.fromToken(row[0]),
            latestTag = row[1],
            releaseName = row[2],
            htmlUrl = row[3],
            publishedAt = row[4],
            message = row[5]
        )
    }

    private external fun nativeGetVersion(): String

    private external fun nativeCheckForUpdate(): Array<String>
}

enum class UpdateStatus {
    UP_TO_DATE,
    UPDATE_AVAILABLE,
    UNKNOWN,
    NETWORK_ERROR;

    companion object {
        /** Tokens are produced by the JNI bridge; anything unrecognised stays [UNKNOWN]. */
        fun fromToken(token: String): UpdateStatus = when (token) {
            "uptodate" -> UP_TO_DATE
            "update" -> UPDATE_AVAILABLE
            "network" -> NETWORK_ERROR
            else -> UNKNOWN
        }
    }
}

data class UpdateCheckResult(
    val status: UpdateStatus,
    val latestTag: String = "",
    val releaseName: String = "",
    val htmlUrl: String = "",
    val publishedAt: String = "",
    val message: String = ""
) {
    /**
     * Whether to offer the release page. [UpdateStatus.UNKNOWN] with a tag qualifies: that verdict
     * means the two versions could not be ordered automatically, so the user has to look.
     */
    val canOpenReleasePage: Boolean
        get() = htmlUrl.isNotEmpty() &&
            (status == UpdateStatus.UPDATE_AVAILABLE ||
                (status == UpdateStatus.UNKNOWN && latestTag.isNotEmpty()))
}
