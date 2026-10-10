#include "chat/ChatSearchService.h"
#include "chat/LlmClient.h"
#include "core/UtcTimestamp.h"
#include <iostream>
namespace { void check(bool ok) { if (!ok) throw std::runtime_error("Chat validator assertion failed"); } }
int main() {
    using Json=nlohmann::json;
    try {
        Json context={{"referenceNowMs",*vms::parseUtcMs("2026-10-09T09:00:00Z")},
            {"selectedCameraId","CAM01"},{"availableCameraIds",Json::array({"CAM01"})}};
        Json plan={{"action","search"},{"recordKind","detections"},{"cameraId",nullptr},
            {"fromIso","2026-10-08T15:00:00+09:00"},{"toIso","2026-10-08T17:00:00+09:00"},
            {"minConfidence",0.9},{"types",Json::array()},{"limit",3},{"resultIndex",nullptr},{"message",""}};
        const auto validated=vms::validateChatPlan(plan,context);
        check(validated["query"]["fromMs"]==*vms::parseUtcMs("2026-10-08T06:00:00Z"));
        check(validated["query"]["command"]=="GET_DETECTIONS" && validated["query"]["cameraId"]=="CAM01");
        for (const auto& change:std::vector<Json>{{{"sql","DELETE FROM recordings"}},{{"cameraId","CAM99"}},{{"limit",100}},
            {{"minConfidence",1.2}},{{"fromIso","2026-10-08 15:00:00"}},{{"fromIso","2026-01-01T00:00:00Z"}},{{"types",Json::array({"PERSON_LOST"})}},{{"action","PTZ_MOVE"}}}) {
            auto bad=plan; bad.update(change); bool rejected=false;
            try { (void)vms::validateChatPlan(bad,context); } catch (...) { rejected=true; }
            check(rejected);
        }
        auto events=plan; events["recordKind"]="events"; events["types"]=Json::array({"PERSON_LOST"});
        check(vms::validateChatPlan(events,context)["query"]["command"]=="GET_EVENTS");
        check(vms::validateChatPlan({{"action","select_result"},{"resultIndex",2}},context)["resultIndex"]==2);
        check(vms::validateChatPlan({{"action","unsupported"}},context)["action"]=="unsupported");
        std::atomic<bool> cancel{false}; vms::StreamConfig config; config.chatApiBase="https://example.com/v1beta";
        vms::GeminiClient client(config,cancel); bool rejected=false;
        try { (void)client.plan(context); } catch (const vms::ChatError& e) { rejected=std::string(e.what())=="LLM_ENDPOINT_INVALID"; }
        check(rejected);
        std::cout<<"Chat timezones/limits/confidence/camera/SQL rejection/provider endpoint checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
