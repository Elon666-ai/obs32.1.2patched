#pragma once

#include <string>
#include <vector>

/// Enumerate video/audio capture devices from OBS.
namespace DeviceEnumerator {

struct DeviceList {
    std::vector<std::string> videoDevices;
    std::vector<std::string> audioDevices;
};

/// Query OBS for available DirectShow video and audio input devices.
/// Uses OBS source properties introspection.
DeviceList Enumerate();

/// Refresh the cached device list (call when UI dropdown opens).
DeviceList Refresh();

} // namespace DeviceEnumerator
