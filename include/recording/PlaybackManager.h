#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <cstdint>
namespace vms {
using UtcTime = std::chrono::system_clock::time_point;
struct RecordingInfo {
    std::int64_t id = 0;
    std::string cameraId, filePath, codec;
    UtcTime startTime{}, endTime{};
    double duration = 0;
    int width = 0, height = 0;
};
// M5: database failures are exceptions; nullopt means no matching completed segment.
class PlaybackManager {
public:
    virtual ~PlaybackManager() = default;
    virtual std::optional<RecordingInfo> findRecording(const std::string& cameraId, UtcTime timestamp) const = 0;
};
}
