#pragma once
#include "core/Config.h"
#include <atomic>
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace vms {
struct ChatError : std::runtime_error {
    explicit ChatError(const std::string& code) : std::runtime_error(code) {}
};
class LlmClient {
public:
    virtual ~LlmClient() = default;
    virtual nlohmann::json plan(const nlohmann::json& context) = 0;
};
// HTTP는 chat worker에서만 수행한다. 키/Google 응답 원문을 로그에 남기지 않는다.
class GeminiClient final : public LlmClient {
public:
    GeminiClient(StreamConfig,const std::atomic<bool>& cancelled);
    ~GeminiClient() override;
    nlohmann::json plan(const nlohmann::json&) override;
private:
    StreamConfig config_;
    const std::atomic<bool>& cancelled_;
};
nlohmann::json chatPlanSchema();
}
