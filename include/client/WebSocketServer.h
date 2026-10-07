#pragma once
#include "camera/CameraInfo.h"
#include "client/ClientServer.h"
#include <cstdint>
#include <memory>
#include <functional>
#include <nlohmann/json.hpp>
namespace vms {
struct CameraSnapshot {
    std::string id, name, ipAddress;
    std::string onvifStatus = "NOT_CONFIGURED";
    CameraStatus status = CameraStatus::OFFLINE;
    std::string codec;
    int width = 0, height = 0;
    double fps = 0;
    int timeBaseNum = 0, timeBaseDen = 1;
    std::uint64_t packets = 0, bytes = 0;
    std::string streamUri;
    bool streamReady = false;
    bool ptzReady = false, ptzCenter = false;
    bool recording = false, recordingRequested = false;
    std::string recordingState = "STOPPED", recordingError, recordingFile;

};
// All sockets run on one Asio thread; updateCamera is safe from RTSP workers.
class WebSocketServer final : public ClientServer {
public:
    WebSocketServer();
    ~WebSocketServer() override;
    Result start(const ClientServerOptions&) override;
    void stop() override;
    void publish(const std::string& cameraId, const std::string& jsonNotification) override;
    using CommandHandler = std::function<void(const nlohmann::json&, std::function<void(nlohmann::json)>)>;
    void setCommandHandler(CommandHandler);
    void setDisconnectHandler(std::function<void(std::uint64_t)>); // Before start; dispatches to camera control executor.
    void updateRecording(const std::string& id, bool requested, bool active, const std::string& state, const std::string& error, const std::string& file);
    void updateCamera(const CameraSnapshot&);
    unsigned short port() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
