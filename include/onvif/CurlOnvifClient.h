#pragma once
#include "onvif/OnvifClient.h"
#include <atomic>
#include <map>
namespace vms {
struct CameraEndpoint { std::string host, username, password; };
CameraEndpoint parseCameraEndpoint(const std::string& url);
std::string authenticatedRtspUrl(const std::string& url, const std::string& username, const std::string& password);
class CurlOnvifClient final : public OnvifClient {
public:
    explicit CurlOnvifClient(const std::atomic<bool>& cancelled);
    ~CurlOnvifClient() override;
    std::vector<CameraInfo> discoverDevices(std::chrono::milliseconds timeout) override;
    DeviceInformation getDeviceInformation(const CameraInfo&) override;
    OnvifCapabilities getCapabilities(const CameraInfo&) override;
    std::vector<MediaProfile> getProfiles(const CameraInfo&) override;
    std::string getStreamUri(const CameraInfo&, const std::string& profileToken) override;
    Result continuousMove(const CameraInfo&, const std::string&, float, float) override;
    Result relativeMove(const CameraInfo&, const std::string&, float, float) override;
    Result absoluteMove(const CameraInfo&, const std::string&, float, float) override;
    Result stop(const CameraInfo&, const std::string&) override;
    PtzStatus getStatus(const CameraInfo&, const std::string&) override;
    OnvifCapabilities getServices(const CameraInfo&);
    PtzConfiguration getConfigurationOptions(const CameraInfo&, const std::string& profileToken);
    void setPtzConfiguration(const CameraInfo& camera, const std::string& token, const PtzConfiguration& options);
private:
    Result ptzCommand(const CameraInfo&, const std::string&, const std::string&, float = 0, float = 0);
    std::map<std::pair<std::string, std::string>, PtzConfiguration> ptz_;
    std::string soap(const CameraInfo&, const std::string& url, const std::string& serviceNamespace,
                     const std::string& operation, const std::string& body);
    const std::atomic<bool>& cancelled_;
    std::map<std::string, OnvifCapabilities> capabilities_;
};
}
