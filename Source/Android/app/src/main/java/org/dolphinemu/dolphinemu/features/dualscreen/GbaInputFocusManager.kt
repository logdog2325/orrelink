// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.dualscreen

import org.dolphinemu.dolphinemu.features.input.model.InputOverrider

object GbaInputFocusManager {
    private var focusedDevice = GbaHostBridge.NO_DEVICE
    private val registeredGameCubePorts = mutableSetOf<Int>()
    private var blockedGameCubePorts = IntArray(0)

    fun onGbaInputRegistered(deviceNumber: Int) {
        if (deviceNumber in GBA_DEVICE_RANGE &&
            GbaLinkSettings.hasIndependentPhysicalController(deviceNumber)
        ) {
            InputOverrider.setGbaInputEnabled(deviceNumber, true)
        }
    }

    fun requestFocus(deviceNumber: Int) {
        if (focusedDevice == deviceNumber) return  // already focused: nothing to redo per event
        if (deviceNumber !in GBA_DEVICE_RANGE) {
            return
        }

        if (focusedDevice != deviceNumber) {
            clearFocus()
            focusedDevice = deviceNumber
            blockedGameCubePorts =
                GbaLinkSettings.getGameCubePortsSharingPhysicalController(deviceNumber)
        }

        setGameCubeInputEnabled(blockedGameCubePorts, false)
        InputOverrider.setGbaInputEnabled(deviceNumber, true)
    }

    fun clearFocus(deviceNumber: Int? = null) {
        val currentDevice = focusedDevice
        if (currentDevice !in GBA_DEVICE_RANGE ||
            deviceNumber != null && deviceNumber != currentDevice
        ) {
            return
        }

        InputOverrider.setGbaInputEnabled(
            currentDevice,
            GbaLinkSettings.hasIndependentPhysicalController(currentDevice)
        )
        setGameCubeInputEnabled(blockedGameCubePorts, true)
        blockedGameCubePorts = IntArray(0)
        focusedDevice = GbaHostBridge.NO_DEVICE
    }

    private fun setGameCubeInputEnabled(controllerIndices: IntArray, enabled: Boolean) {
        for (controllerIndex in controllerIndices) {
            // Register the override ONCE per port. registerGameCube re-assigns the core's
            // std::function; the enable flag below is an atomic store and is all that has to
            // run per event. (Re-registering on every ACTION_MOVE raced the CPU thread's pad
            // poll and aborted the app at team preview.)
            if (registeredGameCubePorts.add(controllerIndex)) {
                InputOverrider.registerGameCube(controllerIndex)
            }
            InputOverrider.setGameCubeInputEnabled(controllerIndex, enabled)
        }
    }

    private val GBA_DEVICE_RANGE = 0..3
}
