#pragma once
#include "camera/CameraInfo.h"
#include "core/Result.h"
#include <optional>
#include <vector>
namespace vms {
// M3 contract. Discovery results enter through registerCamera after credential setup.
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
