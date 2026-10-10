#include "chat/ChatSearchService.h"
#include "chat/LlmClient.h"
#include "core/UtcTimestamp.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>
namespace vms {
using Json=nlohmann::json;
namespace {
Json response(const Json& request,bool ok,Json data,const std::string& error={}) {
    Json reply={{"version",1},{"type","response"},{"requestId",request.value("requestId",std::string{})},{"ok",ok}};
    if (ok) reply["data"]=std::move(data); else reply["error"]={{"code",error},{"message",error}};
    return reply;
}
}
Json validateChatPlan(const Json& p,const Json& context) {
    if (!p.is_object()) throw ChatError("LLM_INVALID_PLAN");
    const std::set<std::string> allowed={"action","recordKind","cameraId","fromIso","toIso","minConfidence","types","limit","resultIndex","message"};
    for (const auto& field:p.items()) if (!allowed.count(field.key())) throw ChatError("LLM_INVALID_PLAN");
    if (!p.contains("action") || !p["action"].is_string()) throw ChatError("LLM_INVALID_PLAN");
    const auto action=p["action"].get<std::string>();
    if (action!="search" && action!="clarify" && action!="unsupported" && action!="select_result" && action!="next_page") throw ChatError("LLM_INVALID_PLAN");
    Json result={{"action",action}};
    if (action=="unsupported") { result["message"]="현재는 사람 탐지와 추적 상태 기록만 검색할 수 있어요. 인물·옷 색상·행동 분석은 아직 지원하지 않아요."; return result; }
    if (action=="clarify") {
        const auto message=p.value("message",std::string{});
        if (message.size()>512) throw ChatError("LLM_INVALID_PLAN");
        result["message"]=message.empty() ? "검색할 카메라와 날짜·시간 범위를 알려주세요." : message; return result;
    }
    if (action=="select_result") {
        if (!p.contains("resultIndex") || !p["resultIndex"].is_number_integer()) throw ChatError("LLM_INVALID_PLAN");
        const auto index=p["resultIndex"].get<int>(); if (index<1 || index>20) throw ChatError("CHAT_RESULT_INDEX_INVALID");
        result["resultIndex"]=index; return result;
    }
    if (action=="next_page") return result;
    const auto kind=p.value("recordKind",std::string{});
    if (kind!="detections" && kind!="events") throw ChatError("LLM_INVALID_PLAN");
    std::string camera;
    if (p.contains("cameraId") && p["cameraId"].is_string()) camera=p["cameraId"].get<std::string>();
    if (camera.empty()) camera=context.value("selectedCameraId",std::string{});
    bool exists=false; for (const auto& id:context.at("availableCameraIds")) if (id==camera) exists=true;
    if (!exists) throw ChatError("CHAT_CAMERA_NOT_FOUND");
    if (!p.contains("fromIso") || !p["fromIso"].is_string() || !p.contains("toIso") || !p["toIso"].is_string()) throw ChatError("CHAT_TIME_REQUIRED");
    const auto from=parseUtcMs(p["fromIso"].get<std::string>()), to=parseUtcMs(p["toIso"].get<std::string>());
    const auto now=context.at("referenceNowMs").get<std::int64_t>();
    if (!from || !to || *from>=*to || *to-*from>31LL*86400000 || *to>now+86400000) throw ChatError("CHAT_TIME_RANGE_INVALID");
    if (!p.contains("limit") || !p["limit"].is_number_integer()) throw ChatError("LLM_INVALID_PLAN");
    const auto limit=p["limit"].get<int>(); if (limit<1 || limit>20) throw ChatError("CHAT_LIMIT_INVALID");
    Json query={{"command",kind=="detections" ? "GET_DETECTIONS" : "GET_EVENTS"},{"cameraId",camera},{"fromMs",*from},{"toMs",*to},{"limit",limit}};
    if (p.contains("minConfidence") && !p["minConfidence"].is_null()) {
        if (!p["minConfidence"].is_number()) throw ChatError("CHAT_CONFIDENCE_INVALID");
        const auto n=p["minConfidence"].get<double>(); if (!std::isfinite(n) || n<0 || n>1) throw ChatError("CHAT_CONFIDENCE_INVALID");
        query["minConfidence"]=n;
    }
    if (!p.contains("types") || !p["types"].is_array() || p["types"].size()>4) throw ChatError("LLM_INVALID_PLAN");
    if (kind=="detections" && !p["types"].empty()) throw ChatError("CHAT_RECORD_KIND_INVALID");
    for (const auto& type:p["types"]) if (type!="PERSON_DETECTED" && type!="PERSON_LOST" && type!="TRACKING_STARTED" && type!="TRACKING_STOPPED") throw ChatError("CHAT_EVENT_TYPE_INVALID");
    if (kind=="events") query["types"]=p["types"];
    result["query"]=query; result["recordKind"]=kind; return result;
}
struct ChatSearchService::Impl {
    StreamConfig config;
    EventManager& events;
    Cameras cameras;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    struct Job { Json request; std::function<void(Json)> done; };
    std::deque<Job> jobs;
    struct Conversation { Json query=nullptr,cursor=nullptr,results=Json::array(); };
    std::map<std::uint64_t,Conversation> sessions;
    std::set<std::uint64_t> disconnected;
    std::set<std::uint64_t> pendingSessions;
    Impl(StreamConfig c,EventManager& e,Cameras ids):config(std::move(c)),events(e),cameras(std::move(ids)) {}
    Json database(Json request) {
        auto completion=std::make_shared<std::promise<Json>>(); auto future=completion->get_future();
        request["version"]=1; request["requestId"]="chat-internal";
        events.request(request,[completion](Json result){completion->set_value(std::move(result));});
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while (future.wait_for(std::chrono::milliseconds(50))!=std::future_status::ready) {
            if (stopping.load()) throw ChatError("SHUTTING_DOWN");
            if (std::chrono::steady_clock::now()>deadline) throw ChatError("CHAT_DATABASE_TIMEOUT");
        }
        const auto result=future.get(); if (!result.value("ok",false)) throw ChatError("CHAT_SEARCH_FAILED");
        return result.at("data");
    }
    Json playback(const Json& record) {
        Json query={{"command","GET_EVENT_PLAYBACK"},{"allowEstimated",true}};
        if (record.value("recordKind",std::string{})=="detection_sample") query["sampleId"]=record.at("sampleId"); else query["eventId"]=record.at("id");
        return database(query);
    }
    Json process(const Json& request,GeminiClient& llm) {
        const auto session=request.at("_sessionId").get<std::uint64_t>();
        if (!request.contains("message") || !request["message"].is_string() || request["message"].get_ref<const std::string&>().empty()
            || request["message"].get_ref<const std::string&>().size()>2048) throw ChatError("CHAT_MESSAGE_INVALID");
        const auto timezone=request.value("timezone",std::string{"Asia/Seoul"});
        if (timezone!="Asia/Seoul" && timezone!="UTC") throw ChatError("CHAT_TIMEZONE_UNSUPPORTED");
        Conversation previous;
        { std::lock_guard<std::mutex> lock(mutex); if (disconnected.count(session)) throw ChatError("CHAT_SESSION_CLOSED"); previous=sessions[session]; }
        std::string selected=request.value("cameraId",std::string{});
        if (selected.empty() && previous.query.is_object()) selected=previous.query.value("cameraId",std::string{});
        const auto now=utcNowMs();
        Json context={{"message",request.at("message")},{"referenceNowMs",now},{"localNow",formatUtcMs(now,timezone=="Asia/Seoul" ? 540 : 0)},
            {"timezone",timezone},{"selectedCameraId",selected},{"availableCameraIds",cameras()},{"previousQuery",previous.query},{"previousResultCount",previous.results.size()}};
        const auto plan=validateChatPlan(llm.plan(context),context); const auto action=plan.at("action").get<std::string>();
        if (action=="clarify" || action=="unsupported") return {{"action",action},{"answer",plan.at("message")},{"results",Json::array()}};
        if (action=="select_result") {
            const auto index=plan.at("resultIndex").get<std::size_t>();
            if (index>previous.results.size()) throw ChatError("CHAT_RESULT_INDEX_INVALID");
            const auto& record=previous.results[index-1]; const auto location=playback(record);
            return {{"action","select_result"},{"answer",location.value("playable",false) ? "선택한 기록의 녹화 위치를 찾았어요. 재생 시각은 추정 위치예요." : "선택한 시각에 재생 가능한 완료 녹화가 없어요."},
                {"selectedIndex",index},{"record",record},{"playback",location},{"results",Json::array()}};
        }
        Json query;
        if (action=="next_page") {
            if (!previous.query.is_object() || previous.cursor.is_null()) return {{"action","next_page"},{"answer","다음 검색 결과가 없어요."},{"results",Json::array()},{"nextCursor",nullptr}};
            query=previous.query; query["cursor"]=previous.cursor;
        } else query=plan.at("query");
        const auto data=database(query); const bool detections=query.at("command")=="GET_DETECTIONS";
        const auto& records=data.at(detections ? "detections" : "events");
        auto cards=Json::array(); unsigned playableCount=0;
        for (const auto& record:records) {
            Json card={{"id",record.at("id")},{"recordKind",record.value("recordKind",std::string{"state_event"})},{"cameraId",record.at("cameraId")},
                {"type",record.at("type")},{"searchTimeMs",record.at("searchTimeMs")},{"sourceTimeMs",record.at("sourceTimeMs")},
                {"receivedTimeMs",record.at("receivedTimeMs")},{"confidence",record.at("confidence")},
                {"captureTimeMs",record.value("captureTimeMs",Json(nullptr))},{"analysisTimeMs",record.value("analysisTimeMs",Json(nullptr))}};
            if (detections) card["sampleId"]=record.at("sampleId");
            card["playback"]=playback(card); if (card["playback"].value("playable",false)) ++playableCount;
            cards.push_back(std::move(card));
        }
        Conversation updated; updated.query=query; updated.query.erase("cursor"); updated.cursor=data.at("nextCursor"); updated.results=cards;
        { std::lock_guard<std::mutex> lock(mutex); if (!disconnected.count(session)) sessions[session]=updated; }
        // 결과 수/파일 존재는 LLM이 생성하지 않고 실제 DB 결과로만 답한다.
        const auto answer=cards.empty() ? "조건에 맞는 저장된 기록이 없어요. 이벤트 수신·저장 여부와 검색 시간을 확인해주세요."
            : "현재 페이지에서 "+std::to_string(cards.size())+"개 기록을 찾았고, "+std::to_string(playableCount)+"개는 완료 녹화를 재생할 수 있어요. 재생 위치는 추정 시각이에요.";
        return {{"action","search"},{"answer",answer},{"query",updated.query},{"results",cards},{"nextCursor",updated.cursor},
            {"timezone",timezone},{"model",config.chatModel},{"timeMapping","receive_estimated"}};
    }
    void run() {
        GeminiClient llm(config,stopping);
        for (;;) {
            Job job;
            { std::unique_lock<std::mutex> lock(mutex); wake.wait(lock,[&]{return stopping.load() || !jobs.empty();});
                if (stopping.load()) break;
                job=std::move(jobs.front()); jobs.pop_front(); }
            try { job.done(response(job.request,true,process(job.request,llm))); }
            catch (const ChatError& error) { job.done(response(job.request,false,{},error.what())); }
            catch (...) { job.done(response(job.request,false,{},"CHAT_REQUEST_FAILED")); }
            { std::lock_guard<std::mutex> lock(mutex); const auto id=job.request.value("_sessionId",std::uint64_t{}); pendingSessions.erase(id);
                if (disconnected.erase(id)) sessions.erase(id); }
        }
        std::deque<Job> remaining;
        { std::lock_guard<std::mutex> lock(mutex); remaining.swap(jobs); sessions.clear(); }
        for (auto& job:remaining) job.done(response(job.request,false,{},"SHUTTING_DOWN"));
    }
};
ChatSearchService::ChatSearchService(StreamConfig config,EventManager& events,Cameras cameras):impl_(std::make_unique<Impl>(std::move(config),events,std::move(cameras))) {}
ChatSearchService::~ChatSearchService() { stop(); }
void ChatSearchService::start() { if (impl_->config.chatEnabled) impl_->worker=std::thread([this]{impl_->run();}); }
void ChatSearchService::stop() {
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->stopping.store(true); }
    impl_->wake.notify_all(); if (impl_->worker.joinable()) impl_->worker.join();
}
void ChatSearchService::disconnect(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(impl_->mutex); impl_->sessions.erase(id);
    if (impl_->pendingSessions.count(id)) impl_->disconnected.insert(id);
}
void ChatSearchService::request(const Json& request,std::function<void(Json)> done) {
    std::string error;
    { std::lock_guard<std::mutex> lock(impl_->mutex); const auto session=request.value("_sessionId",std::uint64_t{});
        if (!impl_->config.chatEnabled) error="CHAT_DISABLED";
        else if (impl_->stopping.load() || !session) error="CHAT_UNAVAILABLE";
        else if (impl_->jobs.size()>=32 || impl_->pendingSessions.count(session)) error="CHAT_BUSY";
        else { impl_->pendingSessions.insert(session); impl_->jobs.push_back({request,done}); } }
    if (!error.empty()) done(response(request,false,{},error)); else impl_->wake.notify_one();
}
}
