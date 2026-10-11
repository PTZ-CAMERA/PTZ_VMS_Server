// 수신 worker는 ONVIF 통신을, DB worker는 검증·중복 제거·저장·검색을 담당한다.
// recording callback에는 유효 관측을 전달하며 누락된 탐지 값을 false로 만들지 않는다.
#include "event/EventManager.h"
#include "database/EventRepository.h"
#include "onvif/CurlOnvifClient.h"
#include "core/Logger.h"
#include "core/UtcTimestamp.h"
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
namespace vms {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
namespace {
std::string canonicalTopic(const std::string& topic) {
    std::string result; std::size_t begin=0;
    for (;;) {
        const auto end=topic.find('/',begin); auto part=topic.substr(begin,end==std::string::npos ? end : end-begin);
        const auto colon=part.find(':'); if (colon!=std::string::npos) part=part.substr(colon+1);
        result+=(result.empty() ? "" : "/")+part;
        if (end==std::string::npos) break;
        begin=end+1;
    } return result;
}
Json reply(const Json& request,bool ok,const Json& data={},const std::string& error={}) {
    Json result={{"version",1},{"type","response"},{"requestId",request.value("requestId",std::string{})},{"ok",ok}};
    if (ok) result["data"]=data; else result["error"]={{"code","EVENT_ERROR"},{"message",error}};
    return result;
}
}
Json normalizeMetadata(const std::string& camera,const std::string& profile,const OnvifEventMessage& message,const Json& configured,std::int64_t received) {
    // Pi 소스의 실제 이름을 사용하며, 응답에 없는 필드는 계속 null로 남긴다.
    auto mapping=Json{{"detected","Detected"},{"tracking","Enabled"},{"available","Available"},
        {"confidence","Confidence"},{"bboxX","BoxX"},{"bboxY","BoxY"},{"bboxWidth","BoxWidth"},{"bboxHeight","BoxHeight"},
        {"imageWidth","ImageWidth"},{"imageHeight","ImageHeight"},{"panCommandAngle","PanAngle"},{"tiltCommandAngle","TiltAngle"},
        {"frameId","FrameId"},{"streamEpoch","StreamEpoch"},{"framePtsNs","FramePtsNs"},
        {"captureTimeMs","CaptureUnixMs"},{"analysisTimeMs","AnalysisUnixMs"},{"captureTimeSource","CaptureTimeSource"},
        {"generation","Generation"},{"schemaVersion","SchemaVersion"},{"selectedTarget","SelectedTarget"},
        {"eventType","Type"},{"class","Class"},{"errorX","ErrorX"},{"errorY","ErrorY"},
        {"autoAllowed","AutoAllowed"},{"targetState","TargetState"},{"ptzMode","Mode"},{"moving","Moving"},{"fault","Fault"}};
    for (const auto& item:configured.items()) mapping[item.key()]=item.value();
    Json out={{"cameraId",camera},{"profileToken",profile},{"topic",canonicalTopic(message.topic)},
        {"propertyOperation",message.operation},{"sourceTimeMs",message.sourceMs ? Json(*message.sourceMs) : Json(nullptr)},
        {"sourceTimestamp",message.sourceTimestamp},{"receivedTimeMs",received},{"timestampSemantics","event_unknown"},
        {"clockOffsetMs",nullptr},{"frameTimestampSemantics","publisher_running_time_ns"},{"rawItems",message.data},{"sourceItems",message.source},
        {"captureTimestampSemantics","pts_estimated_not_sensor_exposure"},{"analysisTimestampSemantics","analysis_result_created"},
        {"ptzAngleSemantics","commanded_not_measured"}};
    out["timestampSemantics"]="onvif_broker_created";
    for (const char* key:{"detected","tracking","available","confidence","bboxX","bboxY","bboxWidth","bboxHeight",
        "imageWidth","imageHeight","panCommandAngle","tiltCommandAngle","frameId","frameTimeMs","streamEpoch","framePtsNs",
        "captureTimeMs","analysisTimeMs","captureTimeSource","generation","schemaVersion","selectedTarget","eventType","class",
        "errorX","errorY","autoAllowed","targetState","ptzMode","moving","fault"}) out[key]=nullptr;
    for (const auto& item:mapping.items()) {
        if (!out.contains(item.key()) || !item.value().is_string()) continue;
        const auto name=item.value().get<std::string>(); if (!message.data.contains(name)) continue;
        const auto value=message.data.at(name).get<std::string>();
        if (item.key()=="detected" || item.key()=="tracking" || item.key()=="available" || item.key()=="selectedTarget"
            || item.key()=="autoAllowed" || item.key()=="moving" || item.key()=="fault") {
            if (value=="true" || value=="1") out[item.key()]=true;
            else if (value=="false" || value=="0") out[item.key()]=false;
        } else if (item.key()=="frameId" || item.key()=="streamEpoch" || item.key()=="framePtsNs" || item.key()=="generation"
            || item.key()=="captureTimeMs" || item.key()=="analysisTimeMs" || item.key()=="schemaVersion") {
            try {
                std::size_t used=0; const auto number=std::stoll(value,&used);
                if (used!=value.size() || number<0) continue;
                if (number==0 && (item.key()=="frameId" || item.key()=="streamEpoch" || item.key()=="captureTimeMs" || item.key()=="analysisTimeMs")) continue;
                // 64bit ns/식별자는 문자열로 전송하여 JS의 2^53 정밀도 한계를 피한다.
                if (item.key()=="frameId" || item.key()=="streamEpoch" || item.key()=="framePtsNs" || item.key()=="generation") out[item.key()]=std::to_string(number);
                else out[item.key()]=number;
            } catch (...) {}
        } else if (item.key()=="captureTimeSource" || item.key()=="eventType" || item.key()=="class" || item.key()=="targetState" || item.key()=="ptzMode") out[item.key()]=value;
        else if (item.key()=="frameTimeMs") { if (auto time=parseUtcMs(value)) out[item.key()]=*time; }
        else {
            try {
                std::size_t used=0; const double number=std::stod(value,&used);
                if (used!=value.size() || !std::isfinite(number)) continue;
                if (item.key()=="confidence" && (number<0 || number>1)) continue;
                if ((item.key()=="bboxWidth" || item.key()=="bboxHeight" || item.key()=="imageWidth" || item.key()=="imageHeight") && number<=0) continue;
                out[item.key()]=number;
            } catch (...) { /* 잘못된 선택 필드만 null로 남긴다. 원본은 rawItems에 보존한다. */ }
        }
    }
    return out;
}
struct EventManager::Impl {
    StreamConfig config;
    Publish publish;
    Status status;
    RecordingState recordingState;
    Observation observation;
    std::atomic<bool> stopping{false};
    struct Receiver {
        std::atomic<bool> cancel{false};
        std::thread thread;
        std::mutex mutex;
        std::condition_variable wake;
        ~Receiver() { stop(); }
        void stop() { cancel.store(true); wake.notify_all(); if (thread.joinable()) thread.join(); }
    };
    std::map<std::string,std::unique_ptr<Receiver>> receivers; // CameraService control worker만 변경한다.
    std::mutex mutex;
    std::condition_variable wake;
    struct Task { Json metadata, request; std::function<void(Json)> done; };
    std::deque<Task> queue;
    std::thread database;
    std::map<std::string,Clock::time_point> sampled,notified;
    std::map<std::string,std::int64_t> latestSource;
    std::map<std::string,std::string> latestPayload;
    std::map<std::string,Json> pendingMetadata;
    std::map<std::string,Json> diagnostics; // DB worker 전용. 초기 상태를 탐지 없음으로 추측하지 않는다.
    Impl(StreamConfig c,Publish p,Status s,RecordingState r,Observation o):config(std::move(c)),publish(std::move(p)),status(std::move(s)),recordingState(std::move(r)),observation(std::move(o)) {}
    bool enqueue(Task task) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping.load() || queue.size()>=512) return false;
        queue.push_back(std::move(task)); wake.notify_one(); return true;
    }
    void receive(Receiver& worker,CameraInfo camera,const std::string& profile) {
        CurlOnvifClient client(worker.cancel);
        const auto mapping=Json::parse(config.eventsFieldMap);
        auto pause=[&](unsigned ms) { std::unique_lock<std::mutex> lock(worker.mutex); worker.wake.wait_for(lock,std::chrono::milliseconds(ms),[&] { return worker.cancel.load(); }); };
        auto announce=[&](const std::string& state,const Json& extra=Json::object()) {
            status(camera.id,state); auto data=extra; data["state"]=state; publish(camera.id,"EVENT_RECEIVER_STATUS",data);
        };
        while (!worker.cancel.load()) {
            EventSubscription sub;
            try {
                announce("CONNECTING"); const auto services=client.getServices(camera);
                if (services.eventsUrl.empty()) throw std::runtime_error("Events service unavailable");
                const auto properties=client.getEventProperties(camera,services.eventsUrl);
                bool runtimeEvents=false;
                for (const auto& topic:properties.at("topics")) if (topic=="Runtime/StateEvent") runtimeEvents=true;
                publish(camera.id,"EVENT_RECEIVER_STATUS",{{"state","PROPERTIES"},{"properties",properties},{"transitionSource",runtimeEvents ? "runtime" : "property_fallback"}});
                // 시계 차이는 측정값일 뿐 캡처 시각이나 정확한 PTS anchor가 아니다.
                std::optional<std::int64_t> clockOffset;
                try {
                    const auto before=utcNowMs(); const auto device=client.getDeviceUtcMs(camera); const auto after=utcNowMs();
                    if (device && after-before<2000) clockOffset=*device-(before+(after-before)/2);
                } catch (...) {}
                sub=client.createPullPoint(camera,services.eventsUrl);
                auto deadline=Clock::now()+std::chrono::milliseconds(sub.terminationMs-sub.currentMs);
                announce("SUBSCRIBED");
                while (!worker.cancel.load()) {
                    // 카메라의 UTC 차이와 무관하게 monotonic lifetime으로 Renew를 판단한다.
                    const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(deadline-Clock::now()).count();
                    if (remaining<=static_cast<long long>(config.eventsPullSeconds)+5) {
                        client.renew(camera,sub); deadline=Clock::now()+std::chrono::milliseconds(sub.terminationMs-sub.currentMs);
                    }
                    const auto messages=client.pullMessages(camera,sub,config.eventsPullSeconds,config.eventsMessageLimit);
                    const auto arrival=utcNowMs();
                    for (const auto& message:messages) {
                        const auto topic=canonicalTopic(message.topic);
                        if (topic!="Analytics/PersonDetection" && topic!="Tracking/State" && topic!="Camera/Publishing"
                            && topic!="Runtime/StateEvent" && topic!="PTZ/State") continue;
                        if (message.source.contains("ProfileToken") && message.source.at("ProfileToken")!=profile) continue;
                        auto metadata=normalizeMetadata(camera.id,profile,message,mapping,arrival);
                        metadata["transitionSource"]=runtimeEvents ? "runtime" : "property_fallback";
                        if (clockOffset) metadata["clockOffsetMs"]=*clockOffset;
                        if (metadata.dump().size()>8192) { announce("PAYLOAD_TOO_LARGE"); continue; }
                        Task task; task.metadata=std::move(metadata);
                        if (!enqueue(std::move(task))) announce("QUEUE_OVERFLOW");
                    }
                    // 즉시 응답하는 Pi에서도 busy loop를 방지한다. 종료 시 CV 대기를 중단한다.
                    pause(100);
                }
            } catch (...) {
                if (!worker.cancel.load()) { log(camera.id,"EVENT","Events request failed; retry scheduled"); announce("RECONNECTING"); }
            }
            if (!sub.endpoint.empty()) {
                try {
                    // 취소 후에도 구독 해제를 한 번 시도하되 종료를 오래 막지 않는다.
                    std::atomic<bool> cleanupCancel{false}; CurlOnvifClient cleanup(cleanupCancel); cleanup.unsubscribe(camera,sub);
                } catch (...) {}
            }
            if (!worker.cancel.load()) pause(config.eventsRetryMs);
        }
        announce("STOPPED");
    }
    void runDatabase() {
        std::unique_ptr<EventRepository> repository;
        auto lastPrune=Clock::time_point{};
        auto flushMetadata=[&] {
            const auto now=Clock::now();
            for (auto it=pendingMetadata.begin();it!=pendingMetadata.end();) {
                if (now-notified[it->first]<std::chrono::milliseconds(100)) { ++it; continue; }
                const auto& m=it->second;
                publish(m.at("cameraId").get<std::string>(),"CAMERA_METADATA",m);
                notified[it->first]=now; it=pendingMetadata.erase(it);
            }
        };
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock,std::chrono::milliseconds(100),[&]{return stopping.load() || !queue.empty();});
                if (queue.empty() && stopping.load()) break;
                if (queue.empty()) { lock.unlock(); flushMetadata(); continue; }
                task=std::move(queue.front()); queue.pop_front();
            }
            try {
                if (!repository) repository=std::make_unique<EventRepository>(config.recordingDatabase);
                const auto now=Clock::now();
                if (now-lastPrune>std::chrono::minutes(10)) { repository->prune(config.eventRetentionDays,config.metadataRetentionDays); lastPrune=now; }
                if (task.done) {
                    const auto command=task.request.value("command",std::string{});
                    Json data;
                    if (command=="GET_METADATA_STATUS") {
                        const auto id=task.request.value("cameraId",std::string{});
                        data=repository->status(id);
                        data["lastReceivedTimeMs"]=nullptr; data["detected"]=nullptr; data["detectedAtMs"]=nullptr;
                        if (diagnostics.count(id)) data.update(diagnostics.at(id));
                        data["samplingEnabled"]=config.metadataSampleMs>0;
                        data["recording"]=recordingState ? recordingState(id) : Json::object();
                    } else if (command=="GET_EVENTS") data=repository->search(task.request);
                    else if (command=="GET_DETECTIONS") data=repository->detections(task.request);
                    else {
                        data=repository->playback(task.request);
                        if (data.value("reason",std::string{})=="NO_RECORDING_AT_TIME" && recordingState) {
                            const auto rec=recordingState(data.at("cameraId").get<std::string>());
                            const auto time=data.at("lookupTimeMs").get<std::int64_t>();
                            if (rec.value("recording",false) && rec["startTimeMs"].is_number_integer() && rec["endTimeMs"].is_number_integer()
                                && time>=rec["startTimeMs"].get<std::int64_t>() && time<rec["endTimeMs"].get<std::int64_t>())
                                data["reason"]="RECORDING_IN_PROGRESS";
                        }
                    }
                    task.done(reply(task.request,true,data));
                    continue;
                }
                const auto& m=task.metadata; const auto id=m.at("cameraId").get<std::string>();
                const auto key=id+":"+m.at("topic").get<std::string>();
                auto& diagnostic=diagnostics[id];
                diagnostic["lastReceivedTimeMs"]=m.at("receivedTimeMs");
                // 반복/오래된 snapshot이 overlay를 과거 위치로 되돌리거나 sample DB를 채우지 않게 한다.
                if (m.at("sourceTimeMs").is_number_integer()) {
                    const auto source=m.at("sourceTimeMs").get<std::int64_t>();
                    const auto previous=latestSource.find(key);
                    if (m.at("topic")!="Runtime/StateEvent" && previous!=latestSource.end() && source<previous->second) continue;
                    if (previous!=latestSource.end() && source==previous->second && latestPayload[key]==m.at("rawItems").dump()) continue;
                } else if (latestPayload.count(key) && latestPayload[key]==m.at("rawItems").dump()) continue;
                if (m.at("topic")=="Analytics/PersonDetection") {
                    diagnostic["detected"]=m.at("detected"); diagnostic["detectedAtMs"]=m.at("receivedTimeMs");
                }
                // 저장 샘플링/overlay 전송 간격과 무관하게 유효한 상태를 녹화 worker에 전달한다.
                if (observation) observation(id,m);
                if (auto event=repository->transition(m)) publish(id,"CAMERA_EVENT",*event);
                if (m.at("topic")=="Runtime/StateEvent") continue;
                if (config.metadataSampleMs && now-sampled[key]>=std::chrono::milliseconds(config.metadataSampleMs)) {
                    repository->sample(m); sampled[key]=now;
                }
                // 클라이언트의 느린 렌더링 때문에 packet/DB queue가 무한히 증가하지 않도록 알림도 제한한다.
                pendingMetadata[key]=m; flushMetadata();
                if (m.at("sourceTimeMs").is_number_integer()) latestSource[key]=m.at("sourceTimeMs").get<std::int64_t>();
                latestPayload[key]=m.at("rawItems").dump();
            } catch (const std::exception& error) {
                if (task.done) task.done(reply(task.request,false,{},error.what()));
                else {
                    const auto id=task.metadata.value("cameraId",std::string{"SERVER"});
                    log(id,"EVENT","Metadata database operation failed"); status(id,"DATABASE_ERROR");
                }
            }
        }
        flushMetadata();
    }
};
EventManager::EventManager(StreamConfig config,Publish publish,Status status,RecordingState recording,Observation observation):impl_(std::make_unique<Impl>(std::move(config),std::move(publish),std::move(status),std::move(recording),std::move(observation))) {}
EventManager::~EventManager() { stop(); }
void EventManager::start() { impl_->database=std::thread([this]{impl_->runDatabase();}); }
void EventManager::configure(const CameraInfo& camera,const std::string& profile) {
    if (!impl_->config.eventsEnabled || impl_->stopping.load()) return;
    auto& target=impl_->receivers[camera.id]; if (target) target->stop();
    target=std::make_unique<Impl::Receiver>(); auto* worker=target.get();
    worker->thread=std::thread([this,worker,camera,profile]{impl_->receive(*worker,camera,profile);});
}
void EventManager::stop() {
    // CameraService control worker를 join한 후 호출하여 receiver map 접근을 직렬화한다.
    for (auto& item:impl_->receivers) item.second->cancel.store(true);
    for (auto& item:impl_->receivers) item.second->stop();
    impl_->receivers.clear();
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->stopping.store(true); }
    impl_->wake.notify_all(); if (impl_->database.joinable()) impl_->database.join();
}
void EventManager::request(const Json& request,std::function<void(Json)> done) {
    Impl::Task task; task.request=request; task.done=done;
    if (!impl_->enqueue(std::move(task))) done(reply(request,false,{},"Events queue unavailable"));
}
}
