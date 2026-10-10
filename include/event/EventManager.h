#pragma once
#include "core/Config.h"
#include "camera/CameraInfo.h"
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include "onvif/OnvifEvents.h"
namespace vms {
nlohmann::json normalizeMetadata(const std::string& cameraId,const std::string& profile,
    const OnvifEventMessage&,const nlohmann::json& fieldMap,std::int64_t receivedMs);
class EventManager {
public:
    using Publish = std::function<void(const std::string&,const std::string&,const nlohmann::json&)>;
    using Status = std::function<void(const std::string&,const std::string&)>;
    EventManager(StreamConfig,Publish,Status);
    ~EventManager();
    void start();
    void configure(const CameraInfo&,const std::string& profile);
    void stop();
    void request(const nlohmann::json&,std::function<void(nlohmann::json)>);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
