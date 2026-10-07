#pragma once
#include "camera/CameraInfo.h"
#include "core/Config.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
struct AVPacket;
struct AVFormatContext;
namespace vms {
// Borrowed values: valid ONLY during callback on the RTSP worker.
// Queue consumers must av_packet_clone/ref and copy stream codecpar + time_base.
struct StreamCallbacks {
    std::function<void(const std::string&, CameraStatus)> onStatus;
    std::function<void(const std::string&, const AVFormatContext*, int)> onStream;
    std::function<void(const std::string&, const AVPacket*)> onPacket;
};
class RtspSession {
public:
    explicit RtspSession(StreamConfig config, StreamCallbacks callbacks = {});
    ~RtspSession();
    RtspSession(const RtspSession&) = delete;
    RtspSession& operator=(const RtspSession&) = delete;
    void start(); // Lifecycle methods are called by the owning main/control thread.
    void requestStop() noexcept;
    void join();
    CameraStatus status() const noexcept { return status_.load(); }
private:
    static int interrupt(void* opaque) noexcept;
    void run() noexcept;
    void receive();
    void setStatus(CameraStatus status);
    void deadline(std::chrono::milliseconds timeout);
    StreamConfig config_;
    StreamCallbacks callbacks_;
    std::atomic<bool> stopping_{false};
    std::atomic<CameraStatus> status_{CameraStatus::OFFLINE};
    std::thread worker_;
    std::mutex waitMutex_;
    std::condition_variable wake_;
    std::chrono::steady_clock::time_point deadline_{}; // Worker + its FFmpeg callback only.
};
}
