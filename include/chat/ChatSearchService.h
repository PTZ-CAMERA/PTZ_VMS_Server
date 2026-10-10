#pragma once
#include "core/Config.h"
#include "event/EventManager.h"
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
namespace vms {
nlohmann::json validateChatPlan(const nlohmann::json& plan,const nlohmann::json& context);
// 한 개의 고정 worker: Gemini/DB 대기가 WS·RTSP·PTZ를 막지 않는다.
class ChatSearchService {
public:
    using Cameras=std::function<nlohmann::json()>;
    ChatSearchService(StreamConfig,EventManager&,Cameras);
    ~ChatSearchService();
    void start();
    void stop();
    void disconnect(std::uint64_t session);
    void request(const nlohmann::json&,std::function<void(nlohmann::json)>);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
