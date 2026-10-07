#pragma once
#include "core/Result.h"
#include <string>
namespace vms {
enum class ClientCommand { GET_CAMERA_LIST, GET_CAMERA_STATUS, START_LIVE, STOP_LIVE, PTZ_MOVE, PTZ_STOP, TRACKING_ON, TRACKING_OFF, GET_EVENTS, GET_RECORDINGS };
struct ClientServerOptions { std::string bindAddress = "127.0.0.1"; unsigned short port = 5000; };
class ClientServer {
public:
    virtual ~ClientServer() = default;
    virtual Result start(const ClientServerOptions&) = 0;
    virtual void stop() = 0;
    virtual void publish(const std::string& cameraId, const std::string& jsonNotification) = 0;
};
}
