#include "core/Config.h"
#include <fstream>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <cctype>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
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
        else if (key == "chat_enabled") {
            if (value != "true" && value != "false") throw std::runtime_error("chat_enabled must be true/false");
            result.chatEnabled = value == "true";
        }
        else if (key == "chat_model") result.chatModel = value;
        else if (key == "chat_api_base") result.chatApiBase = value;
        else if (key == "chat_api_key_file") result.chatApiKeyFile = value;
        else if (key == "chat_timeout_ms") result.chatTimeoutMs = static_cast<unsigned>(duration(value).count());
        else if (key == "events_enabled") {
            if (value != "true" && value != "false") throw std::runtime_error("events_enabled must be true/false");
            result.eventsEnabled = value == "true";
        }
        else if (key == "events_field_map") {
            const auto map = nlohmann::json::parse(value);
            if (!map.is_object() || map.size() > 32) throw std::runtime_error("Invalid events_field_map");
            for (const auto& item : map.items()) if (!item.value().is_string() || item.value().get<std::string>().size() > 128)
                throw std::runtime_error("Invalid metadata field name");
            result.eventsFieldMap = value;
        }
        else if (key == "events_pull_seconds" || key == "events_message_limit" || key == "events_retry_ms"
            || key == "metadata_sample_ms" || key == "metadata_retention_days" || key == "event_retention_days") {
            std::size_t used = 0; const auto number = std::stoll(value,&used);
            if (used != value.size() || number < 0 || number > 86400000) throw std::runtime_error("Invalid Events setting");
            if (key == "events_pull_seconds") { if (number < 1 || number > 10) throw std::runtime_error("Pi Pull seconds must be 1..10"); result.eventsPullSeconds = static_cast<unsigned>(number); }
            else if (key == "events_message_limit") { if (number < 1 || number > 64) throw std::runtime_error("Pi Message limit must be 1..64"); result.eventsMessageLimit = static_cast<unsigned>(number); }
            else if (key == "events_retry_ms") { if (number < 100 || number > 300000) throw std::runtime_error("Retry must be 100..300000ms"); result.eventsRetryMs = static_cast<unsigned>(number); }
            else if (key == "metadata_sample_ms") { if (number != 0 && number < 100) throw std::runtime_error("Sample must be 0 or >=100ms"); result.metadataSampleMs = static_cast<unsigned>(number); }
            else { if (number < 1 || number > 3650) throw std::runtime_error("Retention must be 1..3650 days");
                if (key == "metadata_retention_days") result.metadataRetentionDays = static_cast<unsigned>(number); else result.eventRetentionDays = static_cast<unsigned>(number); }
        }
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
        else if (key == "webrtc_gateway_url") result.webRtcBaseUrl = value;
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
    if (!result.webRtcBaseUrl.empty()) {
        auto* url = curl_url();
        if (!url) throw std::runtime_error("Cannot validate gateway URL");
        const auto part = [&](CURLUPart key) { char* raw = nullptr; std::string value;
            if (curl_url_get(url, key, &raw, 0) == CURLUE_OK) { value = raw; curl_free(raw); } return value; };
        const bool parsed = curl_url_set(url, CURLUPART_URL, result.webRtcBaseUrl.c_str(), 0) == CURLUE_OK;
        const auto scheme = part(CURLUPART_SCHEME), host = part(CURLUPART_HOST), pathPart = part(CURLUPART_PATH);
        const bool valid = parsed && (scheme == "http" || scheme == "https") && !host.empty()
            && part(CURLUPART_USER).empty() && part(CURLUPART_PASSWORD).empty()
            && part(CURLUPART_QUERY).empty() && part(CURLUPART_FRAGMENT).empty() && (pathPart.empty() || pathPart == "/");
        curl_url_cleanup(url);
        if (!valid) throw std::runtime_error("webrtc_gateway_url must be an HTTP(S) origin without credentials, path, query or fragment");
        while (result.webRtcBaseUrl.back() == '/') result.webRtcBaseUrl.pop_back();
    }
    if (result.recordingRoot.empty() || result.recordingDatabase.empty() || (result.recordingContainer != "mkv" && result.recordingContainer != "mp4"))
        throw std::runtime_error("Invalid recording configuration");
    const auto configDirectory = std::filesystem::absolute(std::filesystem::u8path(path)).parent_path();
    result.recordingRoot = (configDirectory / std::filesystem::u8path(result.recordingRoot)).lexically_normal().u8string();
    result.recordingDatabase = (configDirectory / std::filesystem::u8path(result.recordingDatabase)).lexically_normal().u8string();
    if (!result.chatApiKeyFile.empty()) result.chatApiKeyFile = (configDirectory / std::filesystem::u8path(result.chatApiKeyFile)).lexically_normal().u8string();
    if (result.chatModel.empty() || result.chatModel.size()>128 || result.chatModel.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")!=std::string::npos)
        throw std::runtime_error("Invalid chat_model");
    if (result.chatTimeoutMs<1000 || result.chatTimeoutMs>60000) throw std::runtime_error("chat_timeout_ms must be 1000..60000");
    return result;
}
}
