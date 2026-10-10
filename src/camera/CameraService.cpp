#include "camera/CameraService.h"
#include "onvif/CurlOnvifClient.h"
#include "stream/StreamManager.h"
#include "recording/RecordingManager.h"
#include "core/Logger.h"
#include "ptz/PtzCommandRouter.h"
#include "event/EventManager.h"
#include "chat/ChatSearchService.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <atomic>
#include <condition_variable>
#include <deque>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
namespace vms {
using Json = nlohmann::json;
namespace {
Json failure(const Json& request, const std::string& code, const std::string& message) {
    return {{"version", 1}, {"type", "response"}, {"requestId", request.value("requestId", std::string{})},
        {"ok", false}, {"error", {{"code", code}, {"message", message}}}};
}
Json success(const Json& request, const Json& data) {
    return {{"version", 1}, {"type", "response"}, {"requestId", request.value("requestId", std::string{})}, {"ok", true}, {"data", data}};
}
std::string field(const Json& request, const char* name, std::size_t maxSize) {
    if (!request.contains(name)) return {};
    if (!request[name].is_string() || request[name].get_ref<const std::string&>().size() > maxSize) throw std::runtime_error("Invalid registration field");
    return request[name].get<std::string>();
}
struct Context {
    std::atomic<CameraStatus> streamStatus{CameraStatus::OFFLINE};
    CameraInfo camera;
    CameraSnapshot snapshot;
    std::chrono::steady_clock::time_point lastUpdate = std::chrono::steady_clock::now();
};
}
struct CameraService::Impl {
    StreamConfig config;
    WebSocketServer& api;
    RtspRelayServer& relay;
    std::atomic<bool> stopping{false};
    CurlOnvifClient onvif{stopping};
    StreamManager streams;
    PtzCommandRouter ptz;
    RecordingManager recordings;
    EventManager events;
    ChatSearchService chat;
    std::map<std::string, std::shared_ptr<Context>> contexts; // Control thread only.
    std::map<std::string, std::string> discoveredIds; // Reserved URI identity; discovery never starts ingest.
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    struct Job { Json request; std::function<void(Json)> done; };
    std::deque<Job> jobs;
    Impl(StreamConfig options, WebSocketServer& server, RtspRelayServer& relayServer)
        : config(std::move(options)), api(server), relay(relayServer),
          recordings({config.recordingRoot, config.recordingSegmentDuration, config.recordingContainer}, config.recordingDatabase,
              [this](const std::string& id, const RecordingStatus& status) { api.updateRecording(id, status.requested, status.active, status.state, status.error, status.filePath); }),
          events(config,[this](const std::string& id,const std::string& kind,const Json& data){api.notify(id,kind,data);},
              [this](const std::string& id,const std::string& state){api.updateEvents(id,state);}),
          chat(config,events,[this]{return api.cameraIds();}) {}
    void startCamera(CameraInfo camera, const std::string& onvifStatus, bool ptzReady = false, bool ptzCenter = false, bool tracking = false) {
        streams.stop(camera.id); // Join prior generation before modifying its context.
        relay.setOffline(camera.id);
        auto context = std::make_shared<Context>(); context->camera = std::move(camera);
        auto& snapshot = context->snapshot;
        snapshot.id = context->camera.id; snapshot.name = context->camera.name;
        snapshot.ptzReady = ptzReady; snapshot.ptzCenter = ptzCenter;
        snapshot.trackingSupported = tracking;
        snapshot.eventsEnabled = config.eventsEnabled;
        snapshot.chatEnabled = config.chatEnabled;
        snapshot.ipAddress = context->camera.ipAddress; snapshot.onvifStatus = onvifStatus;
        snapshot.streamUri = relay.uri(snapshot.id);
        snapshot.directStreamUri = context->camera.rtspUrl;
        if (!config.webRtcBaseUrl.empty()) snapshot.webRtcUri = config.webRtcBaseUrl + "/" + snapshot.id + "/whep";
        api.updateCamera(snapshot);
        contexts[snapshot.id] = context;
        auto options = config; options.cameraId = snapshot.id; options.rtspUrl = context->camera.rtspUrl;
        StreamCallbacks callbacks;
        callbacks.onStatus = [this, context](const std::string& id, CameraStatus status) {
            context->streamStatus.store(status);
            auto& s = context->snapshot; s.status = status;
            if (status != CameraStatus::ONLINE) {
                recordings.offline(id); relay.setOffline(id); s.streamReady = false; s.codec.clear(); s.width = s.height = 0; s.fps = 0;
                s.timeBaseNum = 0; s.timeBaseDen = 1; s.packets = s.bytes = 0;
            }
            api.updateCamera(s);
        };
        callbacks.onStream = [this, context](const std::string& id, const AVFormatContext* input, int index) {
            recordings.configure(id, input, index); relay.configure(id, input, index); auto& s = context->snapshot; const auto* stream = input->streams[index];
            s.codec = "H264"; s.width = stream->codecpar->width; s.height = stream->codecpar->height;
            auto fps = stream->avg_frame_rate; if (fps.num <= 0 || fps.den <= 0) fps = stream->r_frame_rate;
            s.fps = fps.num > 0 && fps.den > 0 ? av_q2d(fps) : 0;
            s.timeBaseNum = stream->time_base.num; s.timeBaseDen = stream->time_base.den; s.packets = s.bytes = 0;
            api.updateCamera(s);
        };
        callbacks.onPacket = [this, context](const std::string& id, const AVPacket* packet) {
            recordings.pushPacket(id, packet); relay.pushPacket(id, packet); auto& s = context->snapshot; ++s.packets; s.bytes += static_cast<std::uint64_t>(packet->size);
            const auto now = std::chrono::steady_clock::now();
            if (now - context->lastUpdate >= std::chrono::seconds(1)) {
                s.streamReady = relay.ready(id); api.updateCamera(s); context->lastUpdate = now;
            }
        };
        streams.start(options, std::move(callbacks));
    }
    std::string cameraIdForService(const std::string& service) {
        for (const auto& entry : contexts) if (entry.second->camera.onvifUrl == service) return entry.first;
        if (!config.onvifUrl.empty() && service == config.onvifUrl) return config.cameraId;
        const auto known = discoveredIds.find(service);
        if (known != discoveredIds.end()) return known->second;
        if (discoveredIds.size() >= 64) throw std::runtime_error("Discovery identity limit reached");
        for (unsigned number = 1; number <= 1000; ++number) {
            std::ostringstream candidate; candidate << "CAM" << std::setfill('0') << std::setw(2) << number;
            const auto id = candidate.str();
            if (contexts.count(id) || (id == config.cameraId && !config.onvifUrl.empty())) continue;
            bool reserved = false;
            for (const auto& entry : discoveredIds) if (entry.second == id) { reserved = true; break; }
            if (!reserved) { discoveredIds.emplace(service, id); return id; }
        }
        throw std::runtime_error("Camera identity limit reached");
    }
    Json discoveredDevice(const CameraInfo& camera, const std::string& source) {
        const auto id = cameraIdForService(camera.onvifUrl);
        const auto entry = contexts.find(id);
        const bool registered = entry != contexts.end();
        const bool ready = registered && entry->second->streamStatus.load() == CameraStatus::ONLINE && relay.ready(id);
        return {{"deviceServiceUrl", camera.onvifUrl}, {"name", camera.name}, {"address", camera.ipAddress}, {"source", source},
            {"cameraId", id}, {"rtspUri", relay.uri(id)}, {"registered", registered}, {"ready", ready}};
    }
    Json registerCamera(const Json& request) {
        CameraInfo camera;
        camera.onvifUrl = field(request, "deviceServiceUrl", 2048);
        if (camera.onvifUrl.empty()) throw std::runtime_error("ONVIF device service URL is required");
        camera.username = field(request, "username", 256); camera.password = field(request, "password", 256);
        if (camera.username.empty()) { camera.username = config.onvifUsername; camera.password = config.onvifPassword; }
        camera.ipAddress = parseCameraEndpoint(camera.onvifUrl).host;
        const auto information = onvif.getDeviceInformation(camera);
        const auto profiles = onvif.getProfiles(camera); if (profiles.empty()) throw std::runtime_error("Camera returned no ONVIF media profiles");
        auto token = field(request, "profileToken", 256);
        if (token.empty()) token = profiles.front().token;
        bool found = false; for (const auto& profile : profiles) if (profile.token == token) found = true;
        if (!found) throw std::runtime_error("ONVIF profile token was not found");
        camera.rtspUrl = onvif.getStreamUri(camera, token); // Kept privately in context/worker.
        camera.id = cameraIdForService(camera.onvifUrl);
        if (!contexts.count(camera.id) && contexts.size() >= 16) throw std::runtime_error("Camera registration limit reached");
        camera.name = information.model.empty() ? "ONVIF camera" : information.model;
        PtzConfiguration options;
        auto ptzCamera = camera; ptzCamera.username = config.onvifUsername; ptzCamera.password = config.onvifPassword;
        try { options = onvif.getConfigurationOptions(ptzCamera, token); }
        catch (const std::exception&) { log(camera.id, "PTZ", "PTZ configuration unavailable; video remains available"); }
        // PTZ uses server-managed credentials, never credentials supplied in a PTZ request.
        if (!ptz.configure(ptzCamera, token, options)) throw std::runtime_error("Stop PTZ before re-registering camera");
        const auto eventCamera = camera;
        const auto id = camera.id; startCamera(std::move(camera), "VERIFIED", !options.endpoint.empty(), !options.position.uri.empty(), options.supportsTracking);
        // 수신 worker는 영상/PTZ worker와 별도로 시작한다. 장치 오류가 등록 성공을 취소하지 않는다.
        events.configure(eventCamera,token);
        log(id, "ONVIF", "Camera registered; profile=" + token + "; upstream URI kept private");
        return {{"cameraId", id}, {"profileToken", token}, {"onvifStatus", "VERIFIED"}, {"rtspUri", relay.uri(id)}};
    }
    Json discover() {
        std::vector<CameraInfo> devices;
        try { devices = onvif.discoverDevices(std::chrono::milliseconds(2500)); }
        catch (const std::exception&) { log("SERVER", "ONVIF", "Multicast discovery unavailable; checking configured endpoint"); }
        Json result = Json::array();
        bool knownFound = false;
        for (const auto& camera : devices) {
            if (camera.onvifUrl == config.onvifUrl) knownFound = true;
            try { result.push_back(discoveredDevice(camera, "ws-discovery")); }
            catch (const std::exception&) { log("SERVER", "ONVIF", "Discovery result could not reserve a camera identity"); }
        }
        if (!knownFound && !config.onvifUrl.empty() && !stopping.load()) {
            CameraInfo camera; camera.onvifUrl = config.onvifUrl; camera.username = config.onvifUsername; camera.password = config.onvifPassword;
            try {
                const auto information = onvif.getDeviceInformation(camera);
                camera.name = information.model; camera.ipAddress = parseCameraEndpoint(camera.onvifUrl).host;
                result.push_back(discoveredDevice(camera, "configured-endpoint"));
            } catch (const std::exception&) {}
        }
        return {{"devices", result}};
    }
    void run() {
        if (!config.rtspUrl.empty()) {
            try {
                CameraInfo camera; camera.id = config.cameraId; camera.name = "Raspberry Pi PTZ";
                camera.rtspUrl = config.rtspUrl; camera.onvifUrl = config.onvifUrl; camera.ipAddress = parseCameraEndpoint(camera.rtspUrl).host;
                startCamera(std::move(camera), "NOT_CONFIGURED");
            } catch (const std::exception&) { log(config.cameraId, "SERVER", "Initial RTSP session could not be started"); }
        }
        while (!stopping.load()) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stopping.load() || !jobs.empty(); });
                if (stopping.load()) break;
                job = std::move(jobs.front()); jobs.pop_front();
            }
            try {
                const auto command = job.request.value("command", std::string{});
                if (command == "START_RECORDING" || command == "STOP_RECORDING" || command == "GET_RECORDINGS") {
                    const auto id = field(job.request, "cameraId", 128);
                    if (id.empty() || id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) throw std::runtime_error("Invalid recording cameraId");
                    if (command != "GET_RECORDINGS" && !contexts.count(id)) { job.done(failure(job.request, "CAMERA_NOT_FOUND", "Camera is not registered")); continue; }
                    if (command == "START_RECORDING" && contexts.at(id)->streamStatus.load() != CameraStatus::ONLINE) { job.done(failure(job.request, "CAMERA_OFFLINE", "Camera stream is not online")); continue; }
                    recordings.request(job.request, job.done); continue;
                }
                const auto data = command == "DISCOVER_CAMERAS" ? discover() : registerCamera(job.request);
                if (job.done) job.done(success(job.request, data));
            } catch (const std::exception& error) {
                if (job.done) {
                    const auto command = job.request.value("command", std::string{});
                    const bool recording = command == "START_RECORDING" || command == "STOP_RECORDING" || command == "GET_RECORDINGS";
                    job.done(failure(job.request, recording ? "RECORDING_ERROR" : "ONVIF_ERROR", error.what()));
                }
            }
        }
        chat.stop(); events.stop(); ptz.shutdown(); streams.stopAll(); recordings.stop(); contexts.clear();
        std::deque<Job> remaining;
        { std::lock_guard<std::mutex> lock(mutex); remaining.swap(jobs); }
        for (const auto& job : remaining) if (job.done) job.done(failure(job.request, "SHUTTING_DOWN", "VMS is stopping"));
    }
};
CameraService::CameraService(StreamConfig config, WebSocketServer& api, RtspRelayServer& relay)
    : impl_(std::make_unique<Impl>(std::move(config), api, relay)) {
    if (impl_->config.onvifUsername.empty() && !impl_->config.rtspUrl.empty()) {
        const auto credentials = parseCameraEndpoint(impl_->config.rtspUrl);
        impl_->config.onvifUsername = credentials.username; impl_->config.onvifPassword = credentials.password;
    }
    CameraSnapshot snapshot; snapshot.id = impl_->config.cameraId; snapshot.name = "Raspberry Pi PTZ";
    snapshot.eventsEnabled = impl_->config.eventsEnabled;
    snapshot.chatEnabled = impl_->config.chatEnabled;
    snapshot.streamUri = impl_->relay.uri(snapshot.id);
    if (!impl_->config.rtspUrl.empty()) snapshot.ipAddress = parseCameraEndpoint(impl_->config.rtspUrl).host;
    impl_->api.updateCamera(snapshot);
}
CameraService::~CameraService() { stop(); }
void CameraService::start() { impl_->events.start(); impl_->chat.start(); impl_->ptz.start(); impl_->recordings.start(); impl_->worker = std::thread([this] { impl_->run(); }); }
void CameraService::stop() {
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->stopping.store(true); }
    impl_->wake.notify_all(); if (impl_->worker.joinable()) impl_->worker.join();
}
void CameraService::disconnect(std::uint64_t session) { impl_->ptz.disconnect(session); impl_->chat.disconnect(session); }
void CameraService::request(const Json& request, std::function<void(Json)> completion) {
    const auto command = request.value("command", std::string{});
    if (command == "CHAT_SEARCH") { impl_->chat.request(request,std::move(completion)); return; }
    if (command == "GET_EVENTS" || command == "GET_DETECTIONS" || command == "GET_EVENT_PLAYBACK") {
        impl_->events.request(request,std::move(completion)); return;
    }
    if (command == "PTZ_MOVE" || command == "PTZ_STOP" || command == "PTZ_CENTER" || command == "TRACKING_ON" || command == "TRACKING_OFF") {
        impl_->ptz.request(request, std::move(completion)); return;
    }
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->stopping.load() && impl_->jobs.size() < 32) { impl_->jobs.push_back({request, completion}); queued = true; }
    }
    if (queued) impl_->wake.notify_one();
    else completion(failure(request, "BUSY", "VMS camera control queue is unavailable"));
}
}
