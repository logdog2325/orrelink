// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay.input

import org.dolphinemu.dolphinemu.features.input.model.ControllerInterface
import org.dolphinemu.dolphinemu.features.input.model.MappingCommon
import org.dolphinemu.dolphinemu.features.input.model.controlleremu.Control
import org.dolphinemu.dolphinemu.features.input.model.controlleremu.ControlGroup
import org.dolphinemu.dolphinemu.features.input.model.controlleremu.EmulatedController

/**
 * Automatically maps a physical Android gamepad (e.g. the built-in controller of gaming
 * handhelds like the AYN Thor) to the emulated controllers used by XD netplay:
 *
 * - GameCube controller 1 (A/B/X/Y/Z/Start, D-Pad, Control Stick, C Stick, L/R triggers)
 * - GBA pads 2 and 3 (A/B/L/R/SELECT/START, D-Pad)
 *
 * The mapping template uses the control names exposed by the Android ControllerInterface
 * backend (Source/Core/InputCommon/ControllerInterface/Android/Android.cpp):
 * button inputs are named after their Android keycodes ("Button A", "Button L1", "Start",
 * "Select", "Up", ...) and gamepad axes are named "Axis <id><sign>" where <id> is the
 * android.view.MotionEvent axis constant and <sign> is '+' or '-' for the half of the axis
 * ("Axis 1-" is left-stick up, because Android's Y axis is negative upwards).
 *
 * Only controls that actually exist on the detected device are mapped, so the same template
 * degrades gracefully on gamepads with e.g. digital-only triggers or a key-based D-Pad.
 *
 * Persistence works exactly like the mapping UI: expressions are applied through
 * [org.dolphinemu.dolphinemu.features.input.model.controlleremu.ControlReference.setExpression]
 * + [EmulatedController.updateSingleControlReference], and then [MappingCommon.save] writes
 * the pad configs (GCPadNew.ini / GBA.ini) to disk, so the mapping survives app restarts.
 *
 * Must only be called after the native library and user directory are initialized
 * (i.e. any point at which the existing mapping UI would also work).
 */
object AutoMapper {
    private const val TOUCHSCREEN_SUFFIX = "/Touchscreen"

    // ControlGroup UI names (see GCPadEmu.cpp / GBAPadEmu.cpp; no string translator is
    // registered on Android, so getUiName() returns these raw strings).
    private const val GROUP_BUTTONS = "Buttons"
    private const val GROUP_MAIN_STICK = "Control Stick" // ui_name of the "Main Stick" group
    private const val GROUP_C_STICK = "C Stick" // ui_name of the "C-Stick" group
    private const val GROUP_TRIGGERS = "Triggers"
    private const val GROUP_DPAD = "D-Pad"

    // Android gamepad button controls (Android.cpp KEYCODE_NAMES).
    private const val BTN_A = "Button A"
    private const val BTN_B = "Button B"
    private const val BTN_X = "Button X"
    private const val BTN_Y = "Button Y"
    private const val BTN_Z = "Button Z"
    private const val BTN_L1 = "Button L1"
    private const val BTN_R1 = "Button R1"
    private const val BTN_L2 = "Button L2"
    private const val BTN_R2 = "Button R2"
    private const val BTN_START = "Start" // KEYCODE_BUTTON_START
    private const val BTN_SELECT = "Select" // KEYCODE_BUTTON_SELECT
    private const val BTN_MENU = "Menu"
    private const val BTN_BACK = "Back"
    private const val KEY_DPAD_UP = "Up" // KEYCODE_DPAD_UP etc.
    private const val KEY_DPAD_DOWN = "Down"
    private const val KEY_DPAD_LEFT = "Left"
    private const val KEY_DPAD_RIGHT = "Right"

    // Android gamepad axis controls: "Axis <MotionEvent axis id><+/->" for joystick-source
    // axes (Android.cpp ConstructAxisName). The '-' input reads positive when the raw axis
    // value is negative.
    private const val AXIS_LSTICK_LEFT = "Axis 0-" // AXIS_X
    private const val AXIS_LSTICK_RIGHT = "Axis 0+"
    private const val AXIS_LSTICK_UP = "Axis 1-" // AXIS_Y (negative = up)
    private const val AXIS_LSTICK_DOWN = "Axis 1+"
    private const val AXIS_RSTICK_LEFT = "Axis 11-" // AXIS_Z
    private const val AXIS_RSTICK_RIGHT = "Axis 11+"
    private const val AXIS_RSTICK_LEFT_ALT = "Axis 12-" // AXIS_RX
    private const val AXIS_RSTICK_RIGHT_ALT = "Axis 12+"
    private const val AXIS_RSTICK_UP = "Axis 14-" // AXIS_RZ
    private const val AXIS_RSTICK_DOWN = "Axis 14+"
    private const val AXIS_RSTICK_UP_ALT = "Axis 13-" // AXIS_RY
    private const val AXIS_RSTICK_DOWN_ALT = "Axis 13+"
    private const val AXIS_HAT_LEFT = "Axis 15-" // AXIS_HAT_X
    private const val AXIS_HAT_RIGHT = "Axis 15+"
    private const val AXIS_HAT_UP = "Axis 16-" // AXIS_HAT_Y (negative = up)
    private const val AXIS_HAT_DOWN = "Axis 16+"
    private const val AXIS_LTRIGGER = "Axis 17+" // AXIS_LTRIGGER
    private const val AXIS_RTRIGGER = "Axis 18+" // AXIS_RTRIGGER
    private const val AXIS_GAS = "Axis 22+" // AXIS_GAS (alternative right trigger)
    private const val AXIS_BRAKE = "Axis 23+" // AXIS_BRAKE (alternative left trigger)

    // Inputs whose presence identifies a device as a gamepad.
    private val GAMEPAD_MARKER_CONTROLS = arrayOf(BTN_A, "Button 1")

    /**
     * Returns the device string (e.g. "Android/1/AYN Thor Gamepad") of the first available
     * input device that is a real gamepad, or null if none is connected.
     *
     * The legacy Touchscreen device, the Device Sensors device, keyboards and mice are
     * excluded: a device qualifies only if it exposes a gamepad button input
     * ("Button A" or "Button 1").
     */
    fun physicalGamepadDevice(): String? =
        ControllerInterface.getAllDeviceStrings().firstOrNull { isPhysicalGamepad(it) }

    /**
     * Returns true if GameCube pad 1 already has a non-touchscreen default device and a
     * mapped A button.
     */
    fun isMapped(): Boolean = isControllerMapped(EmulatedController.getGcPad(0))

    /**
     * Maps GameCube controller 1 and GBA pads 2 and 3 to the first detected physical
     * gamepad, then saves the pad configs to disk.
     *
     * Safe to call on every app launch: controllers that already have a non-touchscreen
     * mapping are never overwritten, and nothing is written to disk if no controller
     * needed mapping.
     *
     * @return true if GameCube pad 1 is mapped to a physical gamepad after this call
     * (whether it already was or was just mapped), false if no physical gamepad is available.
     */
    fun autoMap(): Boolean {
        val gcPad = EmulatedController.getGcPad(0)
        // Map ALL four GBA pads, not just 1 and 2. In netplay the JOINER's local
        // GBA is pad index 0, which was left unmapped -- so the joiner's physical
        // buttons reached nothing and only on-screen touch worked. Mapping the
        // unused pads is harmless (only the active local GBA's input is read).
        val gbaPads = (0..3).map { EmulatedController.getGbaPad(it) }

        val gcNeedsMapping = !isControllerMapped(gcPad)
        val gbaNeedingMapping = gbaPads.filterNot { isControllerMapped(it) }
        if (!gcNeedsMapping && gbaNeedingMapping.isEmpty()) {
            return true
        }

        val device = physicalGamepadDevice() ?: return !gcNeedsMapping
        val coreDevice = ControllerInterface.getDevice(device) ?: return !gcNeedsMapping
        val inputs = coreDevice.getInputs().mapTo(HashSet()) { it.getName() }

        if (gcNeedsMapping) {
            mapGcPad(gcPad, device, inputs)
        }
        for (gbaPad in gbaNeedingMapping) {
            mapGbaPad(gbaPad, device, inputs)
        }

        // Persist the same way the mapping UI does when settings are saved
        // (Settings.saveSettings -> MappingCommon.save -> Pad::GetConfig()->SaveConfig()
        // and Pad::GetGBAConfig()->SaveConfig()).
        MappingCommon.save()

        return isMapped()
    }

    private fun isPhysicalGamepad(deviceString: String): Boolean {
        if (deviceString.endsWith(TOUCHSCREEN_SUFFIX)) {
            return false
        }
        val device = ControllerInterface.getDevice(deviceString) ?: return false
        val inputs = device.getInputs()
        return inputs.any { it.getName() in GAMEPAD_MARKER_CONTROLS }
    }

    private fun isControllerMapped(controller: EmulatedController): Boolean {
        val defaultDevice = controller.getDefaultDevice()
        if (defaultDevice.isEmpty() || defaultDevice.endsWith(TOUCHSCREEN_SUFFIX)) {
            return false
        }
        val aButton = findControl(controller, GROUP_BUTTONS, "A") ?: return false
        return aButton.getControlReference().getExpression().isNotBlank()
    }

    private fun mapGcPad(pad: EmulatedController, device: String, inputs: Set<String>) {
        pad.setDefaultDevice(device)

        // Triggers first, so that the Z mapping can avoid colliding with a digital-only
        // trigger fallback that claims R1.
        val lAnalog = firstPresent(inputs, AXIS_LTRIGGER, AXIS_BRAKE)
        val rAnalog = firstPresent(inputs, AXIS_RTRIGGER, AXIS_GAS)
        val lDigitalParts = listOfNotNull(lAnalog, firstPresent(inputs, BTN_L2))
        val rDigitalParts = listOfNotNull(rAnalog, firstPresent(inputs, BTN_R2))

        var r1ClaimedByTrigger = false
        val lDigital: String?
        val rDigital: String?
        if (lDigitalParts.isNotEmpty() || rDigitalParts.isNotEmpty()) {
            lDigital = alternation(device, lDigitalParts)
            rDigital = alternation(device, rDigitalParts)
        } else {
            // No analog trigger axes and no L2/R2 keys: fall back to the shoulder buttons.
            lDigital = firstPresent(inputs, BTN_L1)?.let { expression(device, it) }
            rDigital = firstPresent(inputs, BTN_R1)?.let { expression(device, it) }
            r1ClaimedByTrigger = rDigital != null
        }

        val zButton = when {
            !r1ClaimedByTrigger && BTN_R1 in inputs -> BTN_R1
            else -> firstPresent(inputs, BTN_Z, BTN_SELECT)
        }

        val buttons = findGroup(pad, GROUP_BUTTONS)
        setMapping(pad, buttons, "A", singleExpression(device, inputs, BTN_A))
        setMapping(pad, buttons, "B", singleExpression(device, inputs, BTN_B))
        setMapping(pad, buttons, "X", singleExpression(device, inputs, BTN_X))
        setMapping(pad, buttons, "Y", singleExpression(device, inputs, BTN_Y))
        setMapping(pad, buttons, "Z", zButton?.let { expression(device, it) })
        setMapping(pad, buttons, "START", singleExpression(device, inputs, BTN_START, BTN_MENU))

        val triggers = findGroup(pad, GROUP_TRIGGERS)
        setMapping(pad, triggers, "L", lDigital)
        setMapping(pad, triggers, "R", rDigital)
        setMapping(pad, triggers, "L-Analog", lAnalog?.let { expression(device, it) })
        setMapping(pad, triggers, "R-Analog", rAnalog?.let { expression(device, it) })

        val mainStick = findGroup(pad, GROUP_MAIN_STICK)
        setMapping(pad, mainStick, "Up", singleExpression(device, inputs, AXIS_LSTICK_UP))
        setMapping(pad, mainStick, "Down", singleExpression(device, inputs, AXIS_LSTICK_DOWN))
        setMapping(pad, mainStick, "Left", singleExpression(device, inputs, AXIS_LSTICK_LEFT))
        setMapping(pad, mainStick, "Right", singleExpression(device, inputs, AXIS_LSTICK_RIGHT))

        // Right stick: prefer AXIS_Z/AXIS_RZ (the standard Android gamepad layout),
        // fall back to AXIS_RX/AXIS_RY.
        val cStick = findGroup(pad, GROUP_C_STICK)
        setMapping(
            pad, cStick, "Up",
            singleExpression(device, inputs, AXIS_RSTICK_UP, AXIS_RSTICK_UP_ALT)
        )
        setMapping(
            pad, cStick, "Down",
            singleExpression(device, inputs, AXIS_RSTICK_DOWN, AXIS_RSTICK_DOWN_ALT)
        )
        setMapping(
            pad, cStick, "Left",
            singleExpression(device, inputs, AXIS_RSTICK_LEFT, AXIS_RSTICK_LEFT_ALT)
        )
        setMapping(
            pad, cStick, "Right",
            singleExpression(device, inputs, AXIS_RSTICK_RIGHT, AXIS_RSTICK_RIGHT_ALT)
        )

        mapDpad(pad, device, inputs)
    }

    private fun mapGbaPad(pad: EmulatedController, device: String, inputs: Set<String>) {
        pad.setDefaultDevice(device)

        val buttons = findGroup(pad, GROUP_BUTTONS)
        setMapping(pad, buttons, "A", singleExpression(device, inputs, BTN_A))
        setMapping(pad, buttons, "B", singleExpression(device, inputs, BTN_B))
        setMapping(pad, buttons, "L", singleExpression(device, inputs, BTN_L1, BTN_L2, AXIS_LTRIGGER, AXIS_BRAKE))
        setMapping(pad, buttons, "R", singleExpression(device, inputs, BTN_R1, BTN_R2, AXIS_RTRIGGER, AXIS_GAS))
        setMapping(pad, buttons, "SELECT", singleExpression(device, inputs, BTN_SELECT, BTN_BACK))
        setMapping(pad, buttons, "START", singleExpression(device, inputs, BTN_START, BTN_MENU))

        mapDpad(pad, device, inputs)
    }

    /**
     * Maps the D-Pad group. Real gamepads deliver their D-Pad either as DPAD key events or
     * as AXIS_HAT_X/AXIS_HAT_Y motion events (Dolphin consumes the motion events, so Android
     * does not synthesize fallback DPAD keys); mapping both forms with an OR covers either.
     */
    private fun mapDpad(pad: EmulatedController, device: String, inputs: Set<String>) {
        val dpad = findGroup(pad, GROUP_DPAD)
        setMapping(pad, dpad, "Up", presentAlternation(device, inputs, KEY_DPAD_UP, AXIS_HAT_UP))
        setMapping(pad, dpad, "Down", presentAlternation(device, inputs, KEY_DPAD_DOWN, AXIS_HAT_DOWN))
        setMapping(pad, dpad, "Left", presentAlternation(device, inputs, KEY_DPAD_LEFT, AXIS_HAT_LEFT))
        setMapping(pad, dpad, "Right", presentAlternation(device, inputs, KEY_DPAD_RIGHT, AXIS_HAT_RIGHT))
    }

    /** Builds a (properly quoted) expression for a control on the default device. */
    private fun expression(device: String, controlName: String): String =
        MappingCommon.getExpressionForControl(controlName, device, device)

    /** Expression for the first of [candidates] that exists on the device, or null. */
    private fun singleExpression(
        device: String,
        inputs: Set<String>,
        vararg candidates: String
    ): String? = firstPresent(inputs, *candidates)?.let { expression(device, it) }

    /** OR-expression of all of [candidates] that exist on the device, or null. */
    private fun presentAlternation(
        device: String,
        inputs: Set<String>,
        vararg candidates: String
    ): String? = alternation(device, candidates.filter { it in inputs })

    private fun alternation(device: String, controlNames: List<String>): String? =
        controlNames
            .takeIf { it.isNotEmpty() }
            ?.joinToString("|") { expression(device, it) }

    private fun firstPresent(inputs: Set<String>, vararg candidates: String): String? =
        candidates.firstOrNull { it in inputs }

    private fun findGroup(controller: EmulatedController, uiName: String): ControlGroup? {
        for (i in 0 until controller.getGroupCount()) {
            val group = controller.getGroup(i)
            if (group.getUiName() == uiName) {
                return group
            }
        }
        return null
    }

    private fun findControl(
        controller: EmulatedController,
        groupUiName: String,
        controlUiName: String
    ): Control? = findGroup(controller, groupUiName)?.let { findControl(it, controlUiName) }

    private fun findControl(group: ControlGroup, controlUiName: String): Control? {
        for (i in 0 until group.getControlCount()) {
            val control = group.getControl(i)
            if (control.getUiName() == controlUiName) {
                return control
            }
        }
        return null
    }

    /**
     * Applies an expression to a control the same way the mapping UI does
     * (setExpression + updateSingleControlReference). Skips controls or expressions that
     * are unavailable, so the template degrades gracefully on unusual gamepads.
     */
    private fun setMapping(
        controller: EmulatedController,
        group: ControlGroup?,
        controlUiName: String,
        expression: String?
    ) {
        if (group == null || expression.isNullOrEmpty()) {
            return
        }
        val control = findControl(group, controlUiName) ?: return
        val controlReference = control.getControlReference()
        if (controlReference.setExpression(expression) != null) {
            // Parse error; leave the control unmapped rather than half-configured.
            return
        }
        controller.updateSingleControlReference(controlReference)
    }
}
