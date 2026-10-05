#include "device-enumerator.hpp"

#include <obs-module.h>
#include <obs.h>
#include <util/platform.h>

#include <set>
#include <cstring>

#include "utils.hpp"

namespace DeviceEnumerator {

static DeviceList g_cache;
static bool g_cached = false;

DeviceList Refresh() {
    DeviceList result;
    result.videoDevices.clear();
    result.audioDevices.clear();

    // ── Method 1: Enumerate via "dshow_input" source properties ──
    // OBS DirectShow input exposes video_device_id and audio_device_id properties.

    // Get the "dshow_input" source type
    size_t idx = 0;
    const char* typeId;
    bool foundDShow = false;
    while (obs_enum_input_types(idx, &typeId)) {
        if (typeId && strcmp(typeId, "dshow_input") == 0) {
            foundDShow = true;
            break;
        }
        ++idx;
    }

    if (foundDShow) {
        // Create temporary properties to read device lists
        obs_data_t* settings = obs_data_create();
        obs_data_t* hotkey   = nullptr;
        obs_properties_t* props = obs_get_source_properties("dshow_input");

        if (props) {
            // Video device
            obs_property_t* videoProp = obs_properties_get(props, "video_device_id");
            if (videoProp) {
                size_t count = obs_property_list_item_count(videoProp);
                for (size_t i = 0; i < count; ++i) {
                    const char* name = obs_property_list_item_string(videoProp, i);
                    if (name && name[0] != '\0')
                        result.videoDevices.push_back(name);
                }
            }

            // Audio device
            obs_property_t* audioProp = obs_properties_get(props, "audio_device_id");
            if (audioProp) {
                size_t count = obs_property_list_item_count(audioProp);
                for (size_t i = 0; i < count; ++i) {
                    const char* name = obs_property_list_item_string(audioProp, i);
                    if (name && name[0] != '\0')
                        result.audioDevices.push_back(name);
                }
            }

            obs_properties_destroy(props);
        }
        obs_data_release(settings);
    }

    // ── Method 2: Enumerate existing sources of known types ──
    // Fallback: look at existing "dshow_input" and "wasapi_input_capture" sources
    if (result.videoDevices.empty() || result.audioDevices.empty()) {
        auto enumCallback = [](void* data, obs_source_t* source) {
            auto* devices = static_cast<DeviceList*>(data);
            const char* id = obs_source_get_id(source);
            const char* name = obs_source_get_name(source);

            if (id && name) {
                if (strcmp(id, "dshow_input") == 0 || strcmp(id, "av_capture_input") == 0) {
                    // This is a video capture source
                    if (std::find(devices->videoDevices.begin(), devices->videoDevices.end(), name)
                        == devices->videoDevices.end()) {
                        devices->videoDevices.push_back(name);
                    }
                }
                if (strcmp(id, "wasapi_input_capture") == 0 ||
                    strcmp(id, "wasapi_output_capture") == 0 ||
                    strcmp(id, "coreaudio_input_capture") == 0 ||
                    strcmp(id, "pulse_input_capture") == 0) {
                    if (std::find(devices->audioDevices.begin(), devices->audioDevices.end(), name)
                        == devices->audioDevices.end()) {
                        devices->audioDevices.push_back(name);
                    }
                }
            }
            return true;
        };
        obs_enum_sources(enumCallback, &result);
    }

    // ── Write to cache ──────────────────────────────────────────
    g_cache = result;
    g_cached = true;

    MP_LOG(LOG_INFO, "[obs-multipusher] device enumeration: %zu video, %zu audio devices",
         result.videoDevices.size(), result.audioDevices.size());

    // Log device names for debugging
    for (auto& v : result.videoDevices)
        MP_LOG(LOG_DEBUG, "[obs-multipusher]   video: %s", v.c_str());
    for (auto& a : result.audioDevices)
        MP_LOG(LOG_DEBUG, "[obs-multipusher]   audio: %s", a.c_str());

    return result;
}

DeviceList Enumerate() {
    if (!g_cached)
        return Refresh();
    return g_cache;
}

} // namespace DeviceEnumerator
