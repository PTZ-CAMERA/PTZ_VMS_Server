#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
namespace vms {
// SQLite 저장·조회 책임은 database 모듈에 둔다.
// 이 연결은 EventManager의 DB worker만 사용한다. recorder의 연결을 공유하지 않는다.
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
    nlohmann::json status(const std::string& cameraId);
private:
    nlohmann::json searchRecords(const nlohmann::json&,bool detections);
    void* db_ = nullptr;
};
}
