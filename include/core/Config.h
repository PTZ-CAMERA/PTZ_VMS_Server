#pragma once
#include <chrono>
#include <string>
namespace vms {
struct StreamConfig {
    std::string cameraId = "CAM01";
    std::string rtspUrl;
    std::string onvifUrl = "http://192.168.0.92:8080/onvif/device_service";
    std::string onvifUsername, onvifPassword;
    std::string transport = "tcp";
    std::string clientBind = "127.0.0.1";
    unsigned short clientPort = 5000;
    std::string relayBind = "127.0.0.1";
    std::string relayPublicHost = "127.0.0.1";
    unsigned short relayPort = 8555;
    std::string recordingRoot = "../recordings", recordingDatabase = "../data/vms.db", recordingContainer = "mkv";
    std::chrono::seconds recordingSegmentDuration{600};
    std::chrono::milliseconds connectTimeout{5000}, readTimeout{5000}, reconnectDelay{2000}, statsInterval{5000};
};
StreamConfig loadConfig(const std::string& path);
}
