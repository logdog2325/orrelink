// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.netplay

import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log

/**
 * Sends a real netplay goodbye when the user swipes Dolphin out of the recents list.
 *
 * The problem this exists for: a swiped-away task is killed outright. `Activity.onDestroy`,
 * `ViewModel.onCleared` and JVM shutdown hooks are all *not* guaranteed to run, so
 * [NetplaySession.closeBlocking] never fires, `~NetPlayClient` never runs, and no ENet
 * disconnect is ever put on the wire. The other players are then left waiting on pad frames
 * from a console that no longer exists, and the only thing that eventually rescues them is a
 * 30 s protocol timeout.
 *
 * `Service.onTaskRemoved` is the one callback Android *does* reliably deliver on task swipe
 * (as long as the service is started and `android:stopWithTask` is left false), and it runs
 * before the process is torn down. That is our window to disconnect politely.
 *
 * This is the *courtesy* half of the fix and nothing may depend on it. The load-bearing half
 * lives in `NetPlayClient::WaitOnRemote()` on the C++ side, which keeps the surviving clients
 * safe when this never gets to run at all -- battery death, airplane mode, a low-memory kill,
 * or any of the OEM task killers that skip this callback entirely.
 *
 * Note what is deliberately NOT hooked: `onStop` / `onTrimMemory(TRIM_MEMORY_UI_HIDDEN)`.
 * Those fire on ordinary backgrounding -- answering a phone call mid-battle -- and hanging up
 * on your opponent because they checked a notification would be a far worse bug than the one
 * being fixed.
 *
 * The service is also strictly self-limiting: it stops itself as soon as there is no live
 * session to watch, so it can never outlive one. [NetplayManager] calling [stop] is the fast
 * path, not the guarantee -- see [selfCheck].
 */
class NetplayTaskWatcherService : Service() {

    private val handler = Handler(Looper.getMainLooper())

    /**
     * Stops the service the moment [NetplayManager.activeSession] stops being a live session.
     *
     * This exists because `NetplayManager`'s call to [stop] rides on `NetplaySession.onClosed`,
     * which only `closeBlocking()` ever invokes. A session that is simply dropped and finalized
     * instead runs `finalize()` -> `releaseNativeResources()` and never touches `onClosed`, so
     * without this poll the service would sit in the process forever -- still registered for
     * `onTaskRemoved`, still holding the app in a state where the system thinks something is
     * running. One no-op comparison every 30 s is a cheap price for a hard bound.
     */
    private val selfCheck = object : Runnable {
        override fun run() {
            val session = NetplayManager.activeSession
            if (session == null || session.isClosed) {
                stopSelf()
                return
            }
            handler.postDelayed(this, SELF_CHECK_INTERVAL_MS)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // The first pass runs immediately rather than after an interval: if we were started
        // with no live session -- nothing to watch, nothing to say goodbye for -- the right
        // answer is to stop right now instead of lingering until the first tick.
        handler.removeCallbacks(selfCheck)
        handler.post(selfCheck)

        // START_NOT_STICKY: if we are ever killed there is nothing worth restarting -- the
        // session we were watching died with the process.
        return Service.START_NOT_STICKY
    }

    override fun onDestroy() {
        handler.removeCallbacks(selfCheck)
        super.onDestroy()
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        val session = NetplayManager.activeSession
        if (session != null && !session.isClosed) {
            // Synchronous on purpose. closeBlocking() is what actually reaches
            // enet_peer_disconnect(); handing it to another thread would race the process
            // death we are already inside of. The cost is a short main-thread block in a
            // process that is going away regardless.
            try {
                session.closeBlocking()
            } catch (e: Exception) {
                Log.e(TAG, "Failed to close netplay session on task removal", e)
            }
        }

        super.onTaskRemoved(rootIntent)
        stopSelf()
    }

    companion object {
        private const val TAG = "NetplayTaskWatcher"

        /**
         * How often the service re-checks that it still has a session to watch. Long enough to
         * be free, short enough that a leaked service is measured in seconds, not in the
         * lifetime of the process.
         */
        private const val SELF_CHECK_INTERVAL_MS = 30_000L

        /**
         * Must only be called once a live session exists and is published to
         * [NetplayManager.activeSession] -- the service has nothing to do without one and will
         * simply stop itself again.
         */
        @JvmStatic
        fun start(context: Context) {
            // Only ever called while a netplay UI is in the foreground, so the background
            // service start restrictions do not apply -- but a failure here must never take
            // the session down with it, hence the catch.
            try {
                context.startService(Intent(context, NetplayTaskWatcherService::class.java))
            } catch (e: Exception) {
                Log.w(TAG, "Could not start netplay task watcher", e)
            }
        }

        @JvmStatic
        fun stop(context: Context) {
            try {
                context.stopService(Intent(context, NetplayTaskWatcherService::class.java))
            } catch (e: Exception) {
                Log.w(TAG, "Could not stop netplay task watcher", e)
            }
        }
    }
}
