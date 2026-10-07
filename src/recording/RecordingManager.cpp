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
    std::unique_ptr<RecordingRepository> repository; // Worker only; opened lazily.
    struct Track {
        std::shared_ptr<RecordingSource> source;
        std::unique_ptr<MuxRecorder> recorder;
        RecordingStatus status;
    };
    std::map<std::string, Track> tracks;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> stopping{false};
    enum class Kind { Configure, Offline, Packet, Command, Fault };
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
    Impl(RecordingOptions config, std::string path, StatusCallback sink)
        : options(std::move(config)), databasePath(std::move(path)), callback(std::move(sink)) {}
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
        if (callback) callback(id, status);
    }
    void failed(const std::string& id, Track& track, const std::string& error) {
        if (track.recorder) track.recorder->abort();
        track.status.requested = false; track.status.state = "ERROR"; track.status.error = error;
        log(id, "REC", error); publish(id, track);
    }
    void checkResult(const Result& result) { if (!result.ok) throw std::runtime_error(result.error); }
    bool enqueue(Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping.load()) return false;
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
        if (task.kind == Kind::Configure || task.kind == Kind::Offline) {
            if (track.recorder) checkResult(track.recorder->stop());
            track.source = task.source;
            if (track.status.requested && track.source) {
                if (!track.recorder) track.recorder = std::make_unique<MuxRecorder>(db());
                checkResult(track.recorder->start(task.id, *track.source, options));
            }
        } else if (task.kind == Kind::Fault) {
            throw std::runtime_error("Could not copy recording packet or codec parameters");
        } else if (task.kind == Kind::Packet) {
            if (!track.status.requested || !track.source || !track.recorder) return;
            const auto before = track.recorder->active(); const auto path = track.recorder->filePath();
            track.recorder->setPacketTime(task.arrivalMs); checkResult(track.recorder->writePacket(task.packet.get()));
            if (before == track.recorder->active() && path == track.recorder->filePath()) return;
        } else {
            const auto command = task.request.value("command", std::string{});
            if (command == "START_RECORDING") {
                if (!track.source) throw std::runtime_error("Camera stream information is not ready");
                if (!track.status.requested) {
                    if (!track.recorder) track.recorder = std::make_unique<MuxRecorder>(db());
                    checkResult(track.recorder->start(task.id, *track.source, options));
                    track.status.requested = true; track.status.state.clear(); track.status.error.clear();
                }
            } else if (command == "STOP_RECORDING") {
                track.status.requested = false;
                if (track.recorder) checkResult(track.recorder->stop());
                track.status.state.clear(); track.status.error.clear();
            }
        }
        publish(task.id, track);
        if (task.done) task.done(response(task.request, true, statusJson(task.id, track.status)));
    }
    void run() {
        for (;;) {
            Task task; bool gap;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stopping.load() || !tasks.empty(); });
                if (tasks.empty() && stopping.load()) break;
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
                if (task.kind != Kind::Command || task.request.value("command", std::string{}) != "GET_RECORDINGS") failed(task.id, tracks[task.id], error.what());
                if (task.done) task.done(response(task.request, false, {}, error.what()));
            }
        }
        for (auto& entry : tracks) {
            auto& track = entry.second; track.status.requested = false;
            if (track.recorder) { const auto result = track.recorder->stop(); if (!result.ok) { failed(entry.first, track, result.error); continue; } }
            publish(entry.first, track);
        }
        tracks.clear(); repository.reset();
    }
};
RecordingManager::RecordingManager(RecordingOptions options, std::string db, StatusCallback callback)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(db), std::move(callback))) {}
RecordingManager::~RecordingManager() { stop(); }
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
    Impl::Task task{}; task.kind = Impl::Kind::Command; task.id = request.value("cameraId", std::string{}); task.request = request; task.done = done;
    if (!impl_->enqueue(std::move(task))) done(response(request, false, {}, "Recording worker queue is unavailable"));
}
}
