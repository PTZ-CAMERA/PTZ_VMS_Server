#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
namespace vms {
// 이벤트 DB worker만 이 연결을 사용한다. 기존 recorder 연결은 공유하지 않는다.
class EventRepository {
public:
    explicit EventRepository(const std::string& path);
    ~EventRepository();
    EventRepository(const EventRepository&) = delete;
    EventRepository& operator=(const EventRepository&) = delete;
    std::optional<nlohmann::json> transition(const nlohmann::json& metadata);
    void sample(const nlohmann::json& metadata);
    void prune(unsigned eventDays,unsigned sampleDays);
    nlohmann::json search(const nlohmann::json& request);
    nlohmann::json detections(const nlohmann::json& request);
    nlohmann::json playback(const nlohmann::json& request);
private:
    nlohmann::json searchRecords(const nlohmann::json&,bool detections);
    void* db_ = nullptr;
};
}
