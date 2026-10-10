#pragma once
#include "camera/CameraInfo.h"
#include "core/Result.h"
#include <chrono>
#include <string>
#include <vector>
namespace vms {
struct DeviceInformation { std::string manufacturer, model, firmware, serialNumber, hardwareId; };
struct OnvifCapabilities { std::string deviceUrl, mediaUrl, ptzUrl, eventsUrl; };
struct MediaProfile { std::string token, name; bool supportsPtz = false; std::string ptzConfigurationToken; };
struct PtzSpace { std::string uri; float minX = 0, maxX = 0, minY = 0, maxY = 0; };
struct PtzConfiguration { std::string endpoint; PtzSpace velocity, position; bool supportsTracking = false; };
struct PtzStatus { float pan = 0, tilt = 0; bool moving = false; };
// M6: adapter methods run on bounded ONVIF executor, never the network/RTSP thread.
// Discovery/query failures throw; command failures return Result. All I/O has timeout.
class OnvifClient {
public:
    virtual ~OnvifClient() = default;
    virtual std::vector<CameraInfo> discoverDevices(std::chrono::milliseconds timeout) = 0;
    virtual DeviceInformation getDeviceInformation(const CameraInfo&) = 0;
    virtual OnvifCapabilities getCapabilities(const CameraInfo&) = 0;
    virtual std::vector<MediaProfile> getProfiles(const CameraInfo&) = 0;
    virtual std::string getStreamUri(const CameraInfo&, const std::string& profileToken) = 0;
    virtual Result continuousMove(const CameraInfo&, const std::string& profileToken, float panVelocity, float tiltVelocity) = 0;
    virtual Result relativeMove(const CameraInfo&, const std::string& profileToken, float pan, float tilt) = 0;
    virtual Result absoluteMove(const CameraInfo&, const std::string& profileToken, float pan, float tilt) = 0;
    virtual Result stop(const CameraInfo&, const std::string& profileToken) = 0;
    virtual Result startTracking(const CameraInfo&, const std::string&) { return {false, "NOT_SUPPORTED"}; }
    virtual PtzStatus getStatus(const CameraInfo&, const std::string& profileToken) = 0;
};
}
