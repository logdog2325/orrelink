// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.dualscreen

import android.os.Handler
import android.os.Looper
import androidx.annotation.Keep
import org.dolphinemu.dolphinemu.features.netplay.NetplayManager
import java.util.concurrent.CopyOnWriteArraySet

object GbaHostBridge {
    private data class Frame(
        val deviceNumber: Int,
        val width: Int,
        val height: Int,
        val pixels: IntArray
    )

    data class CoreInfo(
        val deviceNumber: Int,
        val width: Int,
        val height: Int,
        val isGba: Boolean,
        val hasRom: Boolean,
        val isLocal: Boolean,
        val title: String
    )

    interface Listener {
        fun onGbaGameChanged(info: CoreInfo) {}
        fun onGbaVisibleDeviceChanged(deviceNumber: Int, info: CoreInfo?) {}
        fun onGbaFrame(deviceNumber: Int, width: Int, height: Int, pixels: IntArray) {}
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private val infos = arrayOfNulls<CoreInfo>(DEVICE_COUNT)

    /** Per-device core state, for the on-screen link diagnostic. */
    fun snapshot(device: Int): CoreInfo? =
        if (device in infos.indices) infos[device] else null

    private val listeners = CopyOnWriteArraySet<Listener>()
    private val frameLock = Any()
    private var pendingFrame: Frame? = null
    private var frameDispatchPosted = false
    private var frameConsumerCount = 0

    @Volatile
    var visibleDeviceNumber: Int = NO_DEVICE
        private set

    /**
     * Manual bottom-screen override set by [cycleVisibleDevice], or [NO_DEVICE]
     * for the automatic pick. Only ever holds a device this player owns; see
     * [isSwitchable].
     */
    private var manualDevice: Int = NO_DEVICE

    @JvmStatic
    external fun setVisibleDevice(deviceNumber: Int)

    /**
     * Packed joybus link diagnostic for one SI channel, or 0 for a device that
     * has none. See the bit layout in AndroidGBAHost.cpp; decode with the
     * `LinkDiag*` helpers below.
     */
    @JvmStatic
    external fun getLinkDiag(deviceNumber: Int): Long

    // Decoders for the packed value above. `ushr` matters: probe_count occupies
    // the top 16 bits, so a busy port makes the Long negative.
    fun linkDiagLinkOpen(diag: Long): Boolean = (diag and 0x1L) != 0L

    fun linkDiagEstablished(diag: Long): Boolean = (diag and 0x2L) != 0L

    fun linkDiagLocked(diag: Long): Boolean = (diag and 0x4L) != 0L

    fun linkDiagLastCommand(diag: Long): Int = ((diag ushr 8) and 0xFFL).toInt()

    fun linkDiagResetCount(diag: Long): Int = ((diag ushr 16) and 0xFFFFL).toInt()

    fun linkDiagWindowCount(diag: Long): Int = ((diag ushr 32) and 0xFFFFL).toInt()

    fun linkDiagProbeCount(diag: Long): Int = ((diag ushr 48) and 0xFFFFL).toInt()

    fun addListener(listener: Listener) {
        listeners.add(listener)
        listener.onGbaVisibleDeviceChanged(visibleDeviceNumber, getVisibleInfo())
    }

    fun removeListener(listener: Listener) {
        listeners.remove(listener)
    }

    fun registerFrameConsumer() {
        frameConsumerCount++
        refreshVisibleDevice()
    }

    fun unregisterFrameConsumer() {
        frameConsumerCount--
        refreshVisibleDevice()
    }

    fun hasActiveCore(): Boolean =
        infos.any { it?.isGba == true || it?.hasRom == true }

    private fun getVisibleInfo(): CoreInfo? =
        if (visibleDeviceNumber in infos.indices) infos[visibleDeviceNumber] else null

    /**
     * Whether the manual switch is allowed to select [device].
     *
     * `isLocal` is the load-bearing term and is NOT optional: it is computed
     * natively from `NetPlay::GetPadDetails(n).is_local` (true for everything
     * outside netplay), so in a netplay battle exactly one link device passes
     * this test -- the one this player owns. Being able to put a remote GBA on
     * the bottom screen would show the opponent's party and their move choices
     * live, which is a total cheat, so the switch must never be able to reach
     * one. In solo every core is local and this reduces to "a core is here".
     */
    private fun isSwitchable(device: Int): Boolean {
        if (device !in LINK_DEVICE_RANGE) {
            return false
        }
        val info = infos[device] ?: return false
        return (info.isGba || info.hasRom) && info.isLocal
    }

    /** True while a netplay session exists, lobby included. */
    private fun isNetplayActive(): Boolean = NetplayManager.activeSession != null

    /**
     * Advance the bottom screen to the next GBA this player owns, wrapping.
     * No-op if nothing else is switchable -- which is always the case in
     * netplay, where the second belt below refuses outright.
     */
    fun cycleVisibleDevice() {
        // Belt 2: never switch at all while netplay is up. Belt 1 (isSwitchable)
        // already makes a remote GBA unreachable; this makes the whole feature
        // inert in the only situation where peeking would matter.
        if (isNetplayActive()) {
            return
        }

        val deviceCount = LINK_DEVICE_RANGE.count()
        val current = visibleDeviceNumber
        val start = if (current in LINK_DEVICE_RANGE) {
            current - LINK_DEVICE_RANGE.first
        } else {
            deviceCount - 1
        }

        for (step in 1..deviceCount) {
            val candidate = LINK_DEVICE_RANGE.first + (start + step) % deviceCount
            if (isSwitchable(candidate)) {
                manualDevice = candidate
                refreshVisibleDevice()
                return
            }
        }
    }

    private fun refreshVisibleDevice() {
        // Belt 3: a manual pick only survives while it stays a local, present
        // core, and never into a netplay session. Anything else drops straight
        // back to the automatic pick below.
        if (isNetplayActive() || !isSwitchable(manualDevice)) {
            manualDevice = NO_DEVICE
        }

        // In netplay, prefer the GBA WE own (isLocal) so each player sees their
        // own bottom screen -- host port 2, joiner port 3. The fallbacks are
        // load-bearing: in solo every core is local (native passes isLocal=true
        // outside netplay) so the first clause is identical to the old
        // first-with-ROM pick, and a mistimed/edge case degrades to showing *a*
        // GBA rather than a black screen. Never select a non-local GBA outright.
        val nextDevice = when {
            frameConsumerCount == 0 -> NO_DEVICE
            manualDevice != NO_DEVICE -> manualDevice
            infos[GB_PLAYER_DEVICE]?.hasRom == true -> GB_PLAYER_DEVICE
            else -> LINK_DEVICE_RANGE.firstOrNull { infos[it]?.hasRom == true && infos[it]?.isLocal == true }
                ?: LINK_DEVICE_RANGE.firstOrNull { infos[it]?.hasRom == true }
                ?: LINK_DEVICE_RANGE.firstOrNull { infos[it]?.isGba == true }
                ?: NO_DEVICE
        }

        if (nextDevice != visibleDeviceNumber) {
            visibleDeviceNumber = nextDevice
            setVisibleDevice(nextDevice)
            val info = getVisibleInfo()
            for (listener in listeners) {
                listener.onGbaVisibleDeviceChanged(nextDevice, info)
            }
        }
    }

    @Keep
    @JvmStatic
    fun onGameChanged(
        deviceNumber: Int,
        width: Int,
        height: Int,
        isGba: Boolean,
        hasRom: Boolean,
        isLocal: Boolean,
        title: String
    ) {
        mainHandler.post {
            val info = CoreInfo(deviceNumber, width, height, isGba, hasRom, isLocal, title)
            infos[deviceNumber] = info
            refreshVisibleDevice()
            for (listener in listeners) {
                listener.onGbaGameChanged(info)
            }
        }
    }

    @Keep
    @JvmStatic
    fun onCoreStopped(deviceNumber: Int) {
        mainHandler.post {
            infos[deviceNumber] = null
            refreshVisibleDevice()
        }
    }

    @Keep
    @JvmStatic
    fun onFrame(deviceNumber: Int, width: Int, height: Int, pixels: IntArray) {
        if (deviceNumber != visibleDeviceNumber) {
            return
        }

        synchronized(frameLock) {
            pendingFrame = Frame(deviceNumber, width, height, pixels)
            if (frameDispatchPosted) {
                return
            }
            frameDispatchPosted = true
        }
        mainHandler.post(::dispatchPendingFrame)
    }

    private fun dispatchPendingFrame() {
        val frame = synchronized(frameLock) {
            frameDispatchPosted = false
            pendingFrame.also { pendingFrame = null }
        } ?: return

        if (frame.deviceNumber == visibleDeviceNumber) {
            for (listener in listeners) {
                listener.onGbaFrame(
                    frame.deviceNumber,
                    frame.width,
                    frame.height,
                    frame.pixels
                )
            }
        }
    }

    const val NO_DEVICE = -1
    const val GB_PLAYER_DEVICE = 4

    private const val DEVICE_COUNT = 5
    private val LINK_DEVICE_RANGE = 0..3
}
