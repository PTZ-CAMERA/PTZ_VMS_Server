#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace vms {
struct EventSubscription {
    std::string endpoint, referenceParameters;
    std::int64_t currentMs = 0, terminationMs = 0;
};
struct OnvifEventMessage {
    std::string topic, operation, sourceTimestamp;
    std::optional<std::int64_t> sourceMs;
    nlohmann::json source = nlohmann::json::object(), data = nlohmann::json::object();
};
// 순수 parser: fixture로 검증하며 장비에 접근하지 않는다.
std::vector<OnvifEventMessage> parseEventMessages(const std::string& xml);
}
