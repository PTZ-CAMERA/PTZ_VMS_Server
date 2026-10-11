#pragma once
#include "camera/CameraInfo.h"
#include "core/Result.h"
#include <optional>
#include <vector>
namespace vms {
// 초기 설계의 추상 계약이다. 현재 실행 경로는 CameraService의 context와
// WebSocketServer의 snapshot을 사용한다. 이 선언을 구현된 manager로 해석하지 않는다.
class CameraManager {
public:
    virtual ~CameraManager() = default;
    virtual Result registerCamera(const CameraInfo&) = 0;
    virtual Result removeCamera(const std::string& id) = 0;
    virtual std::optional<CameraInfo> findCamera(const std::string& id) const = 0;
    virtual std::vector<CameraInfo> listCameras() const = 0;
    virtual Result updateStatus(const std::string& id, CameraStatus) = 0;
};
}
