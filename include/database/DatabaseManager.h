#pragma once
#include "camera/CameraInfo.h"
#include "core/Result.h"
#include "event/DetectionEvent.h"
#include <optional>
#include <vector>
namespace vms {
// 초기 설계의 추상 계약이다. 현재 저장/조회 구현은 RecordingRepository와
// EventRepository이며 각각의 작업 worker가 SQLite 연결을 소유한다.
class DatabaseManager {
public:
    virtual ~DatabaseManager() = default;
    virtual Result open(const std::string& path) = 0;
    virtual Result saveCamera(const CameraInfo&) = 0;
    virtual Result deleteCamera(const std::string& cameraId) = 0;
    virtual std::vector<CameraInfo> cameras() const = 0;
    virtual Result saveRecording(const RecordingInfo&) = 0;
    virtual Result saveEvent(const DetectionEvent&) = 0;
    virtual std::vector<RecordingInfo> recordings(const std::string& cameraId, UtcTime from, UtcTime to, unsigned limit) const = 0;
    virtual std::optional<RecordingInfo> findRecording(const std::string& cameraId, UtcTime timestamp) const = 0;
    virtual std::vector<DetectionEvent> events(const std::string& cameraId, UtcTime from, UtcTime to, unsigned limit) const = 0;
};
}
