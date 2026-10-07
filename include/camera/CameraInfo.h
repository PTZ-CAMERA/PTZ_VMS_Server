#pragma once
#include <string>
namespace vms {
enum class CameraStatus { OFFLINE, CONNECTING, ONLINE, ERROR };
struct CameraInfo {
    std::string id, name, ipAddress, rtspUrl, onvifUrl, username, password;
    CameraStatus status = CameraStatus::OFFLINE;
};
}
