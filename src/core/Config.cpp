#include "core/Config.h"
#include <fstream>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <cctype>
namespace vms {
namespace {
std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
std::chrono::milliseconds duration(const std::string& value) {
    std::size_t used = 0;
    const auto n = std::stoll(value, &used);
    if (used != value.size() || n < 100 || n > 300000) throw std::runtime_error("Timeout/interval must be 100..300000 ms");
    return std::chrono::milliseconds(n);
}
}
StreamConfig loadConfig(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open configuration file");
    StreamConfig result;
    std::map<std::string, bool> seen;
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto pos = line.find('=');
        if (pos == std::string::npos) throw std::runtime_error("Expected key=value in configuration");
        const auto key = trim(line.substr(0, pos));
        const auto value = trim(line.substr(pos + 1));
        if (!seen.emplace(key, true).second) throw std::runtime_error("Duplicate configuration key: " + key);
        if (key == "camera_id") result.cameraId = value;
        else if (key == "onvif_url") result.onvifUrl = value;
        else if (key == "onvif_username") result.onvifUsername = value;
        else if (key == "onvif_password") result.onvifPassword = value;
        else if (key == "rtsp_url") result.rtspUrl = value;
        else if (key == "client_bind") result.clientBind = value;
        else if (key == "recording_root") result.recordingRoot = value;
        else if (key == "recording_database") result.recordingDatabase = value;
        else if (key == "recording_container") result.recordingContainer = value;
        else if (key == "recording_segment_seconds") {
            std::size_t used = 0; const auto seconds = std::stoll(value, &used);
            if (used != value.size() || seconds < 1 || seconds > 86400) throw std::runtime_error("recording_segment_seconds must be 1..86400");
            result.recordingSegmentDuration = std::chrono::seconds(seconds);
        }
        else if (key == "rtsp_relay_bind") result.relayBind = value;
        else if (key == "rtsp_public_host") result.relayPublicHost = value;
        else if (key == "client_port" || key == "rtsp_relay_port") {
            std::size_t used = 0;
            const auto port = std::stol(value, &used);
            if (used != value.size() || port < 1 || port > 65535) throw std::runtime_error("Listener port must be 1..65535");
            if (key == "client_port") result.clientPort = static_cast<unsigned short>(port);
            else result.relayPort = static_cast<unsigned short>(port);
        }
        else if (key == "rtsp_transport") result.transport = value;
        else if (key == "connect_timeout_ms") result.connectTimeout = duration(value);
        else if (key == "read_timeout_ms") result.readTimeout = duration(value);
        else if (key == "reconnect_delay_ms") result.reconnectDelay = duration(value);
        else if (key == "stats_interval_ms") result.statsInterval = duration(value);
        else throw std::runtime_error("Unknown configuration key");
    }
    if (input.bad()) throw std::runtime_error("Configuration read failed");
    if (result.cameraId.empty()) throw std::runtime_error("camera_id is required");
    for (unsigned char c : result.cameraId)
        if (!std::isalnum(c) && c != '_' && c != '-') throw std::runtime_error("Invalid camera_id");
    if (!result.rtspUrl.empty() && (result.rtspUrl.rfind("rtsp://", 0) != 0 || result.rtspUrl.size() <= 7 || result.rtspUrl.find_first_of(" \t\r\n") != std::string::npos))
        throw std::runtime_error("rtsp_url must be a nonempty rtsp:// URL");
    if (result.transport != "tcp" && result.transport != "udp") throw std::runtime_error("rtsp_transport must be tcp or udp");
    if (result.relayPublicHost.empty() || result.relayPublicHost == "0.0.0.0" || result.relayPublicHost == "::")
        throw std::runtime_error("rtsp_public_host must be a reachable VMS host");
    for (unsigned char c : result.relayPublicHost)
        if (!std::isalnum(c) && c != '.' && c != '-' && c != ':' && c != '[' && c != ']')
            throw std::runtime_error("Invalid rtsp_public_host");
    if (result.recordingRoot.empty() || result.recordingDatabase.empty() || (result.recordingContainer != "mkv" && result.recordingContainer != "mp4"))
        throw std::runtime_error("Invalid recording configuration");
    const auto configDirectory = std::filesystem::absolute(std::filesystem::u8path(path)).parent_path();
    result.recordingRoot = (configDirectory / std::filesystem::u8path(result.recordingRoot)).lexically_normal().u8string();
    result.recordingDatabase = (configDirectory / std::filesystem::u8path(result.recordingDatabase)).lexically_normal().u8string();
    return result;
}
}
