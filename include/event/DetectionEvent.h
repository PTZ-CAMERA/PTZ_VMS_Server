#pragma once
#include "recording/PlaybackManager.h"
#include <cstdint>
#include <string>
#include <optional>
namespace vms {
enum class EventType { PERSON_DETECTED, PERSON_LOST, TRACKING_STARTED, TRACKING_STOPPED, CAMERA_CONNECTED, CAMERA_DISCONNECTED };
struct DetectionEvent {
    std::int64_t id = 0;
    std::string cameraId;
    UtcTime timestamp{};
    EventType type = EventType::PERSON_DETECTED;
    // 응답에 없는 값을 0으로 만들지 않는다. 원본 시각과 수신 시각을 구분한다.
    std::optional<std::int64_t> sourceTimeMs;
    std::int64_t receivedTimeMs = 0;
    std::optional<double> confidence, errorX, errorY, pan, tilt;
    std::optional<int> x, y, width, height, imageWidth, imageHeight;
};
}
