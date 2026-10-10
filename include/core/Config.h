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
    std::string webRtcBaseUrl; // 별도 PC MediaMTX의 클라이언트 접근 가능한 HTTP(S) origin.
    unsigned short relayPort = 8555;
    std::string recordingRoot = "../recordings", recordingDatabase = "../data/vms.db", recordingContainer = "mkv";
    std::chrono::seconds recordingSegmentDuration{600};
    std::chrono::milliseconds connectTimeout{5000}, readTimeout{5000}, reconnectDelay{2000}, statsInterval{5000};
    // 실물 수신은 명시적으로 켠다. 기본 실행/fixture 외 시험에서 자동 Pi 호출을 방지한다.
    bool eventsEnabled = false;
    unsigned eventsPullSeconds = 5, eventsMessageLimit = 32, eventsRetryMs = 2000;
    unsigned metadataSampleMs = 1000, metadataRetentionDays = 7, eventRetentionDays = 30;
    std::string eventsFieldMap = "{}";
    bool chatEnabled = false;
    std::string chatModel = "gemini-3.5-flash-lite";
    std::string chatApiBase = "https://generativelanguage.googleapis.com/v1beta";
    std::string chatApiKeyFile = "gemini-key.local.conf";
    unsigned chatTimeoutMs = 15000;
};
StreamConfig loadConfig(const std::string& path);
}
