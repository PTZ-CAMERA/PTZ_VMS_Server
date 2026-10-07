#pragma once
#include "onvif/OnvifClient.h"
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
namespace vms {
// One bounded worker, one pending command per camera. No SOAP on the network thread.
class PtzCommandRouter {
public:
    using Reply = std::function<void(nlohmann::json)>;
    PtzCommandRouter();
    ~PtzCommandRouter();
    void start();
    void shutdown();
    bool configure(const CameraInfo&, const std::string& profile, const PtzConfiguration&);
    void request(const nlohmann::json&, Reply);
    void disconnect(std::uint64_t session);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
