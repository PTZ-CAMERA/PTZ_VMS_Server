#include "ptz/PtzCommandRouter.h"
#include "onvif/CurlOnvifClient.h"
#include "core/Logger.h"
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
namespace vms {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace {
struct Job {
    Json request;
    PtzCommandRouter::Reply reply;
    Clock::time_point received = Clock::now();
};
void report(const Job& job, const char* phase, bool ok, const std::string& code = {}) {
    if (!job.reply) return;
    Json data{{"cameraId", job.request.value("cameraId", std::string{})}, {"command", job.request.value("command", std::string{})},
        {"phase", phase}, {"motorArrivalConfirmed", false}, {"trackingStateConfirmed", false}};
    Json message{{"version", 1}, {"type", std::string(phase) == "ACCEPTED" || std::string(phase) == "REJECTED" ? "response" : "notification"},
        {"requestId", job.request.value("requestId", std::string{})}, {"ok", ok}, {"data", data}};
    if (message["type"] == "notification") message["event"] = "PTZ_RESULT";
    if (!ok) message["error"] = {{"code", code}, {"message", code}};
    job.reply(std::move(message));
}
}
struct PtzCommandRouter::Impl {
    struct Target {
        CameraInfo camera;
        std::string profile;
        PtzConfiguration options;
        std::uint64_t owner = 0;
        bool busy = false, stopping = false, tracking = false;
        Clock::time_point lease{};
        std::optional<Job> pending;
    };
    std::map<std::string, Target> targets;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool closing = false;
    std::atomic<bool> cancelled{false}; // Stop requests remain usable during graceful shutdown.
    CurlOnvifClient onvif{cancelled};
    std::string lastCamera;
    void stopTarget(Target& target) {
        if (target.pending) report(*target.pending, "SUPERSEDED", false, "SUPERSEDED_BY_STOP");
        Job stop; stop.request = {{"cameraId", target.camera.id}, {"command", "PTZ_STOP"}};
        target.pending = std::move(stop); target.stopping = true; target.tracking = false;
    }
    void run() {
        for (;;) {
            Target copy; Job job; std::string id;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(25));
                const auto now = Clock::now();
                for (auto& entry : targets) {
                    auto& target = entry.second;
                    if (target.owner && !target.stopping && (closing || (!target.tracking && now >= target.lease))) stopTarget(target);
                }
                // Round-robin prevents a held button from starving other cameras.
                auto select = [&](auto begin, auto end) {
                    for (auto it = begin; it != end; ++it) if (it->second.pending) {
                        id = it->first; auto& t = it->second; job = std::move(*t.pending); t.pending.reset(); t.busy = true; copy = t; return true;
                    }
                    return false;
                };
                auto split = targets.upper_bound(lastCamera);
                if (!select(split, targets.end())) select(targets.begin(), split);
                if (id.empty()) { if (closing) break; else continue; }
                lastCamera = id;
            }
            const auto command = job.request.value("command", std::string{});
            Result result;
            if (command == "PTZ_MOVE" && Clock::now() - job.received > std::chrono::milliseconds(400)) {
                result = {false, "STALE_REQUEST"};
            } else {
                onvif.setPtzConfiguration(copy.camera, copy.profile, copy.options);
                if (command == "PTZ_MOVE") result = onvif.continuousMove(copy.camera, copy.profile,
                    job.request.at("panVelocity").get<float>(), job.request.at("tiltVelocity").get<float>());
                else if (command == "PTZ_CENTER") result = onvif.absoluteMove(copy.camera, copy.profile, 0, 0);
                else if (command == "TRACKING_ON") result = onvif.startTracking(copy.camera, copy.profile);
                else result = onvif.stop(copy.camera, copy.profile);
            }
            log(id, "PTZ", command + (result.ok ? " Pi acknowledged" : " failed: " + result.error));
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto& target = targets.at(id); target.busy = false;
                if (command == "PTZ_STOP" || command == "TRACKING_OFF") { target.owner = 0; target.stopping = false; target.tracking = false; }
                else if (!result.ok && !target.stopping) stopTarget(target);
            }
            report(job, result.ok ? "PI_ACKNOWLEDGED" : "FAILED", result.ok, result.error);
            wake.notify_one();
        }
    }
};
PtzCommandRouter::PtzCommandRouter() : impl_(std::make_unique<Impl>()) {}
PtzCommandRouter::~PtzCommandRouter() { shutdown(); }
void PtzCommandRouter::start() { impl_->worker = std::thread([this] { impl_->run(); }); }
void PtzCommandRouter::shutdown() {
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->closing = true; }
    impl_->wake.notify_one(); if (impl_->worker.joinable()) impl_->worker.join();
}
bool PtzCommandRouter::configure(const CameraInfo& camera, const std::string& profile, const PtzConfiguration& options) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->targets.find(camera.id);
    if (impl_->closing || (it != impl_->targets.end() && (it->second.owner || it->second.busy || it->second.pending))) return false;
    if (options.endpoint.empty()) { impl_->targets.erase(camera.id); return true; }
    auto& target = impl_->targets[camera.id]; target.camera = camera; target.profile = profile; target.options = options;
    return true;
}
void PtzCommandRouter::request(const Json& request, Reply reply) {
    Job job{request, std::move(reply), Clock::now()};
    auto reject = [&](const char* code) { report(job, "REJECTED", false, code); };
    if (!request.contains("cameraId") || !request["cameraId"].is_string()) return reject("INVALID_REQUEST");
    const auto id = request["cameraId"].get<std::string>();
    const auto command = request.value("command", std::string{});
    float x = 0, y = 0;
    if (command == "PTZ_MOVE") {
        if (!request.contains("panVelocity") || !request["panVelocity"].is_number()
            || !request.contains("tiltVelocity") || !request["tiltVelocity"].is_number()) return reject("INVALID_VELOCITY");
        x = request["panVelocity"].get<float>(); y = request["tiltVelocity"].get<float>();
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1 || std::abs(y) > 1) return reject("INVALID_VELOCITY");
    } else if (command != "PTZ_STOP" && command != "PTZ_CENTER" && command != "TRACKING_ON" && command != "TRACKING_OFF") return reject("NOT_SUPPORTED");
    const auto session = request.value("_sessionId", std::uint64_t{0});
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!session || impl_->closing) return reject("UNAVAILABLE");
    auto it = impl_->targets.find(id); if (it == impl_->targets.end()) return reject("PTZ_NOT_AVAILABLE");
    auto& target = it->second;
    if (target.owner && target.owner != session) return reject("PTZ_BUSY");
    if (target.stopping) return reject("PTZ_STOPPING");
    const auto& space = target.options.velocity;
    if (command == "PTZ_MOVE" && (x < space.minX || x > space.maxX || y < space.minY || y > space.maxY)) return reject("PTZ_OUT_OF_RANGE");
    if (command == "PTZ_CENTER" && target.options.position.uri.empty()) return reject("CENTER_NOT_SUPPORTED");
    if (command == "TRACKING_ON" && !target.options.supportsTracking) return reject("TRACKING_NOT_SUPPORTED");
    target.owner = session;
    target.lease = Clock::now() + (command == "PTZ_CENTER" ? std::chrono::milliseconds(10000) : std::chrono::milliseconds(600));
    target.tracking = command == "TRACKING_ON";
    target.stopping = command == "PTZ_STOP" || command == "TRACKING_OFF";
    if (target.pending) report(*target.pending, "SUPERSEDED", false, "SUPERSEDED");
    report(job, "ACCEPTED", true); // Enqueued before worker can emit its result.
    target.pending = std::move(job); impl_->wake.notify_one();
}
void PtzCommandRouter::disconnect(std::uint64_t session) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& entry : impl_->targets) if (entry.second.owner == session && !entry.second.stopping) impl_->stopTarget(entry.second);
    impl_->wake.notify_one();
}
}
