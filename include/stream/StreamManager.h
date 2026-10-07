#pragma once
#include "stream/RtspSession.h"
#include <map>
#include <memory>
namespace vms {
class StreamManager {
public:
    ~StreamManager();
    void start(const StreamConfig& config, StreamCallbacks callbacks = {});
    void stop(const std::string& cameraId);
    void stopAll();
    CameraStatus status(const std::string& cameraId) const;
private:
    // Owned by main/control thread; RtspSession exposes atomic status.
    std::map<std::string, std::unique_ptr<RtspSession>> sessions_;
};
}
