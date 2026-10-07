#pragma once
#include "recording/PlaybackManager.h"
#include <cstdint>
#include <string>
namespace vms {
enum class EventType { PERSON_DETECTED, PERSON_LOST, TRACKING_STARTED, TRACKING_STOPPED, CAMERA_CONNECTED, CAMERA_DISCONNECTED };
struct DetectionEvent {
    std::int64_t id = 0;
    std::string cameraId;
    UtcTime timestamp{};
    EventType type = EventType::PERSON_DETECTED;
    double confidence = 0;
    int x = 0, y = 0, width = 0, height = 0;
    double errorX = 0, errorY = 0, pan = 0, tilt = 0;
};
}
