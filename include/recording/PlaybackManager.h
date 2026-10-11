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
// 초기 설계의 추상 계약이다. 현재 완료 구간 조회는 RecordingRepository 및
// EventRepository에서 수행하고 Qt PlaybackWidget이 실제 로컬 재생을 담당한다.
class PlaybackManager {
public:
    virtual ~PlaybackManager() = default;
    virtual std::optional<RecordingInfo> findRecording(const std::string& cameraId, UtcTime timestamp) const = 0;
};
}
