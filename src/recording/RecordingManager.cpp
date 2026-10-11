// 파일·muxer·녹화 정책은 이 worker가 소유한다. 수신 스레드는 참조 복제한 packet을
// 전달하고, 다른 모듈은 mutex로 보호한 snapshot만 읽는다.
#include "recording/RecordingManager.h"
#include "core/Logger.h"
extern "C" {
#include <libavcodec/packet.h>
}
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
namespace vms {
namespace {
using Json = nlohmann::json;
using Packet = std::shared_ptr<AVPacket>;
std::int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
Json response(const Json& request, bool ok, Json data, const std::string& error = {}) {
    Json result{{"version", 1}, {"type", "response"}, {"requestId", request.value("requestId", std::string{})}, {"ok", ok}};
    if (ok) result["data"] = std::move(data); else result["error"] = {{"code", "RECORDING_ERROR"}, {"message", error}};
    return result;
}
Json statusJson(const std::string& id, const RecordingStatus& s) {
    return {{"cameraId", id}, {"requested", s.requested}, {"recording", s.active}, {"state", s.state}, {"filePath", s.filePath}, {"error", s.error}};
}
}
struct RecordingManager::Impl {
    RecordingOptions options;
    std::string databasePath;
    StatusCallback callback;
    bool defaultAuto;
    std::chrono::seconds postRoll;
    std::unique_ptr<RecordingRepository> repository; // Worker only; opened lazily.
    struct Track {
        std::shared_ptr<RecordingSource> source;
        std::unique_ptr<MuxRecorder> recorder;
        RecordingStatus status;
        bool initialized=false, autoEnabled=false, manual=false, autoActive=false, personPresent=false, eventsHealthy=false, trackingEnabled=false, trackingKnown=false;
        std::chrono::steady_clock::time_point deadline{};
    };
    std::map<std::string, Track> tracks;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> stopping{false};
    enum class Kind { Configure, Offline, Packet, Command, Fault, Detection, Tracking };
    struct Task {
        Kind kind; std::string id;
        std::shared_ptr<RecordingSource> source;
        Packet packet;
        std::int64_t arrivalMs = 0;
        Json request;
        std::function<void(Json)> done;
    };
    std::deque<Task> tasks;
    std::size_t queuedBytes = 0;
    bool overflow = false;
    std::map<std::string, bool> armed;
    std::map<std::string, Json> snapshots; // 다른 worker에는 복사한 상태만 전달한다.
    Impl(RecordingOptions config, std::string path, StatusCallback sink,bool automatic,unsigned seconds)
        : options(std::move(config)), databasePath(std::move(path)), callback(std::move(sink)),defaultAuto(automatic),postRoll(seconds) {}
    RecordingRepository& db() {
        if (!repository) repository = std::make_unique<RecordingRepository>(databasePath);
        return *repository;
    }
    void publish(const std::string& id, Track& track) {
        auto& status = track.status;
        status.active = track.recorder && track.recorder->active();
        if (track.recorder) status.filePath = track.recorder->filePath();
        if (status.state != "ERROR") status.state = !status.requested ? "STOPPED" : !track.source ? "WAITING_FOR_STREAM" : status.active ? "RECORDING" : "WAITING_KEYFRAME";
        { std::lock_guard<std::mutex> lock(mutex); armed[id] = status.requested; }
        snapshotTrack(id, track);
        if (callback) callback(id, status);
    }
    void snapshotTrack(const std::string& id, const Track& track) {
        // muxer 자체는 공유하지 않는다. 미확정 파일 경로도 외부 조회에 제공하지 않는다.
        auto data = statusJson(id, track.status);
        data.erase("filePath"); // 아직 확정되지 않은 파일 경로는 클라이언트에 제공하지 않는다.
        data["autoRecordingEnabled"]=track.autoEnabled; data["manualRecording"]=track.manual;
        data["autoRecordingActive"]=track.autoActive; data["eventsHealthy"]=track.eventsHealthy;
        data["trackingEnabled"]=track.trackingKnown ? Json(track.trackingEnabled) : Json(nullptr);
        data["postSeconds"]=postRoll.count();
        const bool active = track.recorder && track.recorder->active();
        data["recording"] = active;
        data["startTimeMs"] = active ? Json(track.recorder->startTimeMs()) : Json(nullptr);
        data["endTimeMs"] = active ? Json(track.recorder->endTimeMs()) : Json(nullptr);
        std::lock_guard<std::mutex> lock(mutex); snapshots[id] = std::move(data);
    }
    void failed(const std::string& id, Track& track, const std::string& error) {
        if (track.recorder) track.recorder->abort();
        track.status.requested = false; track.status.state = "ERROR"; track.status.error = error;
        track.manual=false; track.autoActive=false;
        log(id, "REC", error); publish(id, track);
    }
    void checkResult(const Result& result) { if (!result.ok) throw std::runtime_error(result.error); }
    bool enqueue(Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping.load()) return false;
            // 탐지 신호도 무제한으로 쌓지 않는다. 동일 카메라의 마지막 대기 신호만 교체한다.
            if (task.kind==Kind::Detection) {
                for (auto it=tasks.rbegin();it!=tasks.rend();++it) {
                    if (it->id!=task.id) continue;
                    if (it->kind!=Kind::Detection) break;
                    *it=std::move(task); wake.notify_one(); return true;
                }
                if (tasks.size()>=1024) return false;
            }
            if (task.kind == Kind::Command && std::count_if(tasks.begin(), tasks.end(), [](const Task& item) { return item.kind == Kind::Command; }) >= 64) return false;
            if (task.kind == Kind::Configure || task.kind == Kind::Offline) {
                for (auto it = tasks.begin(); it != tasks.end();) {
                    if (it->id == task.id && (it->kind == Kind::Packet || it->kind == Kind::Configure || it->kind == Kind::Offline)) {
                        if (it->packet) queuedBytes -= static_cast<std::size_t>(it->packet->size);
                        it = tasks.erase(it);
                    } else ++it;
                }
            }
            if (task.packet) {
                // 과부하를 숨기지 않는다. 대기 영상은 비우되 명령은 보존하고,
                // worker가 현재 구간을 확정한 뒤 다음 keyframe부터 새 구간을 시작한다.
                if (tasks.size() >= 1024 || queuedBytes + static_cast<std::size_t>(task.packet->size) > 16 * 1024 * 1024) {
                    for (auto it = tasks.begin(); it != tasks.end();) { if (it->packet) it = tasks.erase(it); else ++it; }
                    queuedBytes = 0; overflow = true;
                }
                queuedBytes += static_cast<std::size_t>(task.packet->size);
            }
            tasks.push_back(std::move(task));
        }
        wake.notify_one(); return true;
    }
    void process(Task& task) {
        if (task.kind == Kind::Command && task.request.value("command", std::string{}) == "GET_RECORDINGS") {
            const auto from = task.request.value("fromMs", std::numeric_limits<std::int64_t>::min());
            const auto to = task.request.value("toMs", std::numeric_limits<std::int64_t>::max());
            const auto limit = task.request.value("limit", 100);
            if (from >= to || limit < 1 || limit > 100) throw std::runtime_error("Invalid recording query range or limit");
            Json entries = Json::array();
            for (const auto& e : db().list(task.id, from, to, static_cast<unsigned>(limit))) entries.push_back({{"id", e.id}, {"cameraId", e.cameraId},
                {"startTimeMs", e.startMs}, {"endTimeMs", e.endMs}, {"filePath", e.filePath}, {"duration", e.duration}, {"codec", e.codec}, {"width", e.width}, {"height", e.height}});
            task.done(response(task.request, true, {{"recordings", entries}})); return;
        }
        auto& track = tracks[task.id];
        if (!track.initialized) { track.autoEnabled=defaultAuto; track.initialized=true; }
        if (task.kind == Kind::Configure || task.kind == Kind::Offline) {
            if (track.recorder) checkResult(track.recorder->stop());
            if (task.kind==Kind::Offline) { track.autoActive=false; track.personPresent=false; track.trackingKnown=false; track.status.requested=track.manual; }
            track.source = task.source;
            if (track.status.requested && track.source) {
                if (!track.recorder) track.recorder = std::make_unique<MuxRecorder>(db());
                checkResult(track.recorder->start(task.id, *track.source, options));
            }
        } else if (task.kind==Kind::Tracking) {
            track.trackingKnown=true;
            track.trackingEnabled=task.request.at("enabled").get<bool>();
            if (!track.trackingEnabled) {
                track.autoActive=false; track.personPresent=false; track.deadline={};
                if (!track.manual) {
                    track.status.requested=false;
                    if (track.recorder) checkResult(track.recorder->stop());
                    track.status.state.clear();
                }
            }
        } else if (task.kind==Kind::Detection) {
            const auto value=task.request.value("detected",Json(nullptr));
            const bool recovering=!track.eventsHealthy;
            track.eventsHealthy=task.request.value("available",false) && value.is_boolean();
            if (!track.eventsHealthy) { track.trackingKnown=false; snapshotTrack(task.id,track); return; }
            const bool present=value.get<bool>();
            // 타임아웃/통신 오류를 사람이 떠났다는 신호로 해석하지 않는다.
            if (!present && (track.personPresent || recovering)) track.deadline=std::chrono::steady_clock::now()+postRoll;
            if (!present && track.autoActive && !track.personPresent && track.deadline==std::chrono::steady_clock::time_point{})
                track.deadline=std::chrono::steady_clock::now()+postRoll;
            track.personPresent=present;
            if (present && track.trackingKnown && track.trackingEnabled && track.autoEnabled && track.source) {
                track.autoActive=true; track.deadline=std::chrono::steady_clock::time_point{};
                if (!track.status.requested) {
                    if (!track.recorder) track.recorder=std::make_unique<MuxRecorder>(db());
                    checkResult(track.recorder->start(task.id,*track.source,options));
                    track.status.requested=true; track.status.state.clear(); track.status.error.clear();
                    log(task.id,"REC","Person detected: automatic recording requested");
                }
            }
        } else if (task.kind == Kind::Fault) {
            throw std::runtime_error("Could not copy recording packet or codec parameters");
        } else if (task.kind == Kind::Packet) {
            if (!track.status.requested || !track.source || !track.recorder) return;
            const auto before = track.recorder->active(); const auto path = track.recorder->filePath();
            track.recorder->setPacketTime(task.arrivalMs); checkResult(track.recorder->writePacket(task.packet.get()));
            snapshotTrack(task.id, track);
            if (before == track.recorder->active() && path == track.recorder->filePath()) return;
        } else {
            const auto command = task.request.value("command", std::string{});
            if (command == "START_RECORDING") {
                if (!track.source) throw std::runtime_error("Camera stream information is not ready");
                track.manual=true;
                if (!track.status.requested) {
                    if (!track.recorder) track.recorder = std::make_unique<MuxRecorder>(db());
                    checkResult(track.recorder->start(task.id, *track.source, options));
                    track.status.requested = true; track.status.state.clear(); track.status.error.clear();
                }
            } else if (command == "STOP_RECORDING") {
                track.manual=false; track.autoActive=false; track.personPresent=false; track.deadline={};
                track.status.requested = false;
                if (track.recorder) checkResult(track.recorder->stop());
                track.status.state.clear(); track.status.error.clear();
            } else if (command=="SET_AUTO_RECORDING") {
                if (!task.request.contains("enabled") || !task.request["enabled"].is_boolean()) throw std::runtime_error("enabled must be boolean");
                track.autoEnabled=task.request["enabled"].get<bool>();
                if (!track.autoEnabled) {
                    track.autoActive=false;
                    if (!track.manual) {
                        track.status.requested=false;
                        if (track.recorder) checkResult(track.recorder->stop());
                        track.status.state.clear();
                    }
                }
            }
        }
        publish(task.id, track);
        if (task.done) {
            auto data=statusJson(task.id,track.status); data["autoRecordingEnabled"]=track.autoEnabled;
            task.done(response(task.request,true,data));
        }
    }
    void run() {
        for (;;) {
            Task task; bool gap;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock,std::chrono::milliseconds(100), [&] { return stopping.load() || !tasks.empty(); });
                if (tasks.empty() && stopping.load()) break;
                if (tasks.empty()) { lock.unlock(); expireAutomatic(); continue; }
                task = std::move(tasks.front()); tasks.pop_front();
                if (task.packet) queuedBytes -= static_cast<std::size_t>(task.packet->size);
                gap = overflow; overflow = false;
            }
            if (gap) {
                for (auto& entry : tracks) if (entry.second.status.requested && entry.second.recorder) {
                    auto& track = entry.second;
                    try {
                        checkResult(track.recorder->stop());
                        if (track.source) checkResult(track.recorder->start(entry.first, *track.source, options));
                        track.status.error = "Recording queue overflow; gap in footage";
                        log(entry.first, "REC", "Queue overflow; next segment waits for keyframe"); publish(entry.first, track);
                    } catch (const std::exception& e) { failed(entry.first, track, e.what()); }
                }
            }
            try { process(task); }
            catch (const std::exception& error) {
                if (task.kind != Kind::Command || task.request.value("command",std::string{})!="GET_RECORDINGS") failed(task.id, tracks[task.id], error.what());
                if (task.done) task.done(response(task.request, false, {}, error.what()));
            }
            expireAutomatic();
        }
        for (auto& entry : tracks) {
            auto& track = entry.second; track.status.requested = false;
            if (track.recorder) { const auto result = track.recorder->stop(); if (!result.ok) { failed(entry.first, track, result.error); continue; } }
            publish(entry.first, track);
        }
        tracks.clear(); repository.reset();
    }
    void expireAutomatic() {
        // 상태 판단은 worker에서 직렬화한다. 수동 소유권·사람 있음·Events 오류는
        // 자동 종료 대상이 아니며, 타이머에는 시스템 UTC 대신 steady_clock을 쓴다.
        const auto now=std::chrono::steady_clock::now();
        for (auto& entry:tracks) {
            auto& t=entry.second;
            if (!t.autoActive || t.personPresent || !t.eventsHealthy || t.deadline==std::chrono::steady_clock::time_point{} || now<t.deadline) continue;
            t.autoActive=false;
            try {
                if (!t.manual) {
                    t.status.requested=false;
                    if (t.recorder) checkResult(t.recorder->stop());
                    t.status.state.clear(); log(entry.first,"REC","Person absent: automatic recording finalized");
                }
                publish(entry.first,t);
            } catch (const std::exception& e) { failed(entry.first,t,e.what()); }
        }
    }
};
RecordingManager::RecordingManager(RecordingOptions options, std::string db, StatusCallback callback,bool automatic,unsigned seconds)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(db), std::move(callback),automatic,seconds)) {}
RecordingManager::~RecordingManager() { stop(); }
nlohmann::json RecordingManager::snapshot(const std::string& id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->snapshots.find(id);
    return it == impl_->snapshots.end() ? Json{{"cameraId",id},{"recording",false},{"state","STOPPED"}} : it->second;
}
void RecordingManager::start() { impl_->worker = std::thread([this] { impl_->run(); }); }
void RecordingManager::stop() {
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->stopping.store(true); }
    impl_->wake.notify_all(); if (impl_->worker.joinable()) impl_->worker.join();
}
void RecordingManager::configure(const std::string& id, const AVFormatContext* input, int index) {
    Impl::Task task{}; task.kind = Impl::Kind::Configure; task.id = id;
    try { task.source = std::make_shared<RecordingSource>(copyRecordingSource(input, index)); }
    catch (...) { task.kind = Impl::Kind::Fault; }
    impl_->enqueue(std::move(task));
}
void RecordingManager::offline(const std::string& id) { Impl::Task task{}; task.kind = Impl::Kind::Offline; task.id = id; impl_->enqueue(std::move(task)); }
void RecordingManager::pushPacket(const std::string& id, const AVPacket* input) {
    if (!input || input->size <= 0 || impl_->stopping.load()) return;
    { std::lock_guard<std::mutex> lock(impl_->mutex); if (!impl_->armed[id]) return; }
    Impl::Task task{}; task.kind = Impl::Kind::Packet; task.id = id; task.arrivalMs = nowMs();
    if (input->size > 4 * 1024 * 1024) task.kind = Impl::Kind::Fault;
    else {
        auto* raw = av_packet_clone(input);
        if (!raw) task.kind = Impl::Kind::Fault;
        else task.packet = Packet(raw, [](AVPacket* p) { av_packet_free(&p); });
    }
    impl_->enqueue(std::move(task));
}
void RecordingManager::request(const Json& request, std::function<void(Json)> done) {
    // 불량 설정 요청은 기존 녹화 상태에 영향을 주지 않는다. 실제 저장 오류는 worker에서 처리한다.
    if (request.value("command",std::string{})=="SET_AUTO_RECORDING" && (!request.contains("enabled") || !request["enabled"].is_boolean())) {
        done(response(request,false,{},"enabled must be boolean")); return;
    }
    Impl::Task task{}; task.kind = Impl::Kind::Command; task.id = request.value("cameraId", std::string{}); task.request = request; task.done = done;
    if (!impl_->enqueue(std::move(task))) done(response(request, false, {}, "Recording worker queue is unavailable"));
}
void RecordingManager::detection(const std::string& id,const Json& detected,bool available) {
    Impl::Task task{}; task.kind=Impl::Kind::Detection; task.id=id;
    task.request={{"detected",detected},{"available",available}};
    impl_->enqueue(std::move(task));
}
void RecordingManager::tracking(const std::string& id,bool enabled) {
    Impl::Task task{}; task.kind=Impl::Kind::Tracking; task.id=id; task.request={{"enabled",enabled}};
    impl_->enqueue(std::move(task));
}
}
