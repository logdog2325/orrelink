// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/Quartz/Quartz.h"

#include <IOKit/hidsystem/IOHIDLib.h>

#include "Common/Logging/Log.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/Quartz/QuartzKeyboardAndMouse.h"

namespace ciface::Quartz
{
std::string GetSourceName()
{
  return "Quartz";
}

class InputBackend final : public ciface::InputBackend
{
public:
  using ciface::InputBackend::InputBackend;
  void PopulateDevices() override;
  void HandleWindowChange() override;
};

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<InputBackend>(controller_interface);
}

void InputBackend::HandleWindowChange()
{
  const std::string source_name = GetSourceName();
  GetControllerInterface().RemoveDevice(
      [&](const auto* dev) { return dev->GetSource() == source_name; }, true);

  PopulateDevices();
}

void InputBackend::PopulateDevices()
{
  const WindowSystemInfo wsi = GetControllerInterface().GetWindowSystemInfo();
  if (wsi.type != WindowSystemType::MacOS)
    return;

  // The keyboard is read with CGEventSourceKeyState, which macOS silently
  // answers "nothing is pressed" for until the app holds Input Monitoring
  // permission -- keyboard controls simply do nothing, with no error anywhere.
  // Asking for it here surfaces the system prompt (and the entry in Privacy &
  // Security) the first time input is set up, instead of leaving the user with
  // a dead keyboard. Only prompts once; later launches return the stored answer.
  static bool s_requested_input_monitoring = false;
  if (!s_requested_input_monitoring)
  {
    s_requested_input_monitoring = true;
    if (__builtin_available(macOS 10.15, *))
    {
      if (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) != kIOHIDAccessTypeGranted)
      {
        const bool granted = IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
        if (!granted)
        {
          WARN_LOG_FMT(CONTROLLERINTERFACE,
                       "Input Monitoring permission not granted; keyboard controls will not "
                       "respond. Enable Dolphin under System Settings > Privacy & Security > "
                       "Input Monitoring, then restart Dolphin.");
        }
      }
    }
  }

  GetControllerInterface().AddDevice(std::make_shared<KeyboardAndMouse>(wsi.render_window));
}

}  // namespace ciface::Quartz
