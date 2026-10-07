#pragma once
#include "core/Config.h"
#include "client/WebSocketServer.h"
#include "stream/RtspRelayServer.h"
#include <memory>
namespace vms {
// Control executor owns registration/StreamManager; SOAP never runs on network/UI threads.
class CameraService {
public:
    CameraService(StreamConfig, WebSocketServer&, RtspRelayServer&);
    ~CameraService();
    void start();
    void disconnect(std::uint64_t session);
    void stop();
    void request(const nlohmann::json&, std::function<void(nlohmann::json)> completion);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
