// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.netplay

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.dolphinemu.dolphinemu.DolphinApplication

object NetplayManager {

    private val mutex = Mutex()

    @Volatile
    private var closeComplete: CompletableDeferred<Unit>? = null

    @Volatile
    var activeSession: NetplaySession? = null
        private set

    suspend fun createSession(): NetplaySession = mutex.withLock {
        closeComplete?.await()

        // Sessions should be closed by UI navigation, but just in case.
        activeSession?.closeBlocking()

        closeComplete = CompletableDeferred()

        val session = try {
            NetplaySession(
                onClosed = {
                    activeSession = null
                    NetplayTaskWatcherService.stop(DolphinApplication.getAppContext())
                    closeComplete?.complete(Unit)
                }
            )
        } catch (t: Throwable) {
            // No session was created, so onClosed will never run to open the gate again.
            // Leaving the deferred armed would wedge every future createSession() on the
            // await() at the top of this function.
            closeComplete?.complete(Unit)
            throw t
        }

        activeSession = session

        // Watch for the task being swiped out of recents while this session is live. That is
        // the one teardown path Android will not run onCleared()/onDestroy() for, so without
        // this the other players never receive an ENet disconnect and are left waiting on a
        // console that no longer exists. See NetplayTaskWatcherService.
        //
        // Started here, after construction succeeded AND after activeSession is published,
        // rather than before the constructor. The service is useless without a session --
        // onTaskRemoved does nothing but read activeSession -- and starting it first meant a
        // throwing constructor left it running for the rest of the process lifetime with
        // nothing left to stop it. The service double-checks activeSession itself and stops
        // when there is none, so neither half depends on the other being right.
        NetplayTaskWatcherService.start(DolphinApplication.getAppContext())

        session
    }
}
