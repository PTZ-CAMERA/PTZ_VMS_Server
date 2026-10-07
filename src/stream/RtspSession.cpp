#include "stream/RtspSession.h"
#include "core/Logger.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}
#include <cerrno>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
namespace vms {
namespace {
struct Input {
    AVFormatContext* context = avformat_alloc_context();
    Input() { if (!context) throw std::bad_alloc(); }
    ~Input() { avformat_close_input(&context); }
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
};
struct Dictionary {
    AVDictionary* value = nullptr;
    ~Dictionary() { av_dict_free(&value); }
    void set(const char* key, const std::string& text) {
        if (av_dict_set(&value, key, text.c_str(), 0) < 0) throw std::bad_alloc();
    }
};
struct PacketDeleter { void operator()(AVPacket* packet) const { av_packet_free(&packet); } };
std::string errorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}
void check(int code, const char* action) {
    if (code < 0) throw std::runtime_error(std::string(action) + ": " + errorText(code));
}
}
RtspSession::RtspSession(StreamConfig config, StreamCallbacks callbacks)
    : config_(std::move(config)), callbacks_(std::move(callbacks)) {}
RtspSession::~RtspSession() { requestStop(); join(); }
void RtspSession::start() {
    if (worker_.joinable()) throw std::logic_error("RTSP worker already started");
    stopping_.store(false);
    worker_ = std::thread(&RtspSession::run, this);
}
void RtspSession::requestStop() noexcept {
    {
        std::lock_guard<std::mutex> lock(waitMutex_);
        stopping_.store(true);
    }
    wake_.notify_all();
}
void RtspSession::join() { if (worker_.joinable()) worker_.join(); }
void RtspSession::deadline(std::chrono::milliseconds timeout) {
    deadline_ = std::chrono::steady_clock::now() + timeout;
}
int RtspSession::interrupt(void* opaque) noexcept {
    const auto* self = static_cast<RtspSession*>(opaque);
    return self->stopping_.load() || std::chrono::steady_clock::now() >= self->deadline_;
}
void RtspSession::setStatus(CameraStatus status) {
    status_.store(status);
    if (callbacks_.onStatus) callbacks_.onStatus(config_.cameraId, status);
}
void RtspSession::run() noexcept {
    while (!stopping_.load()) {
        try {
            setStatus(CameraStatus::CONNECTING);
            log(config_.cameraId, "RTSP", "Connecting");
            receive();
        } catch (const std::exception& error) {
            if (!stopping_.load()) {
                status_.store(CameraStatus::ERROR);
                try {
                    log(config_.cameraId, "RTSP", error.what());
                    if (callbacks_.onStatus) callbacks_.onStatus(config_.cameraId, CameraStatus::ERROR);
                } catch (...) {} // A consumer failure cannot terminate the server.
            }
        } catch (...) {
            status_.store(CameraStatus::ERROR);
        }
        if (!stopping_.load()) {
            try { log(config_.cameraId, "RTSP", "Reconnecting after " + std::to_string(config_.reconnectDelay.count()) + " ms"); } catch (...) {}
            std::unique_lock<std::mutex> lock(waitMutex_);
            wake_.wait_for(lock, config_.reconnectDelay, [this] { return stopping_.load(); });
        }
    }
    status_.store(CameraStatus::OFFLINE);
    try {
        if (callbacks_.onStatus) callbacks_.onStatus(config_.cameraId, CameraStatus::OFFLINE);
        log(config_.cameraId, "RTSP", "Stopped");
    } catch (...) {}
}
void RtspSession::receive() {
    Input input;
    input.context->interrupt_callback = {&RtspSession::interrupt, this};
    Dictionary options;
    options.set("rtsp_transport", config_.transport);
    options.set("timeout", std::to_string(config_.readTimeout.count() * 1000));
    deadline(config_.connectTimeout);
    const AVInputFormat* rtsp = av_find_input_format("rtsp");
    if (!rtsp) throw std::runtime_error("FFmpeg RTSP demuxer is unavailable");
    check(avformat_open_input(&input.context, config_.rtspUrl.c_str(), rtsp, &options.value), "RTSP open failed");
    deadline(config_.connectTimeout);
    check(avformat_find_stream_info(input.context, nullptr), "Stream probe failed");
    const int index = av_find_best_stream(input.context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    check(index, "No video stream");
    const AVStream* stream = input.context->streams[index];
    const AVCodecParameters* codec = stream->codecpar;
    if (codec->codec_id != AV_CODEC_ID_H264) throw std::runtime_error("M1 requires an H.264 video stream");
    const AVRational fps = av_guess_frame_rate(input.context, input.context->streams[index], nullptr);
    std::ostringstream info;
    info << "Connected\nCodec: H264\nResolution: " << codec->width << 'x' << codec->height << "\nFPS: ";
    if (fps.num > 0 && fps.den > 0) info << av_q2d(fps) << " (" << fps.num << '/' << fps.den << ')';
    else info << "unknown (not provided by stream)";
    info << "\nTime Base: " << stream->time_base.num << '/' << stream->time_base.den;
    log(config_.cameraId, "RTSP", info.str());
    if (callbacks_.onStream) callbacks_.onStream(config_.cameraId, input.context, index);
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (!packet) throw std::bad_alloc();
    std::uint64_t packets = 0, bytes = 0, corrupt = 0;
    bool online = false;
    auto nextStats = std::chrono::steady_clock::now() + config_.statsInterval;
    // One deadline until the next valid VIDEO packet: audio/EAGAIN cannot hide a stalled camera.
    deadline(config_.readTimeout);
    while (!stopping_.load()) {
        const int result = av_read_frame(input.context, packet.get());
        if (result == AVERROR(EAGAIN)) {
            if (interrupt(this)) throw std::runtime_error("Video packet timeout");
            std::unique_lock<std::mutex> lock(waitMutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(10), [this] { return stopping_.load(); });
            continue;
        }
        check(result, "Connection lost / packet read failed");
        if (packet->stream_index == index) {
            if ((packet->flags & AV_PKT_FLAG_CORRUPT) || packet->size <= 0) ++corrupt;
            else {
                if (!online) { setStatus(CameraStatus::ONLINE); online = true; }
                ++packets;
                bytes += static_cast<std::uint64_t>(packet->size);
                if (callbacks_.onPacket) callbacks_.onPacket(config_.cameraId, packet.get());
                deadline(config_.readTimeout);
            }
        }
        av_packet_unref(packet.get());
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextStats) {
            log(config_.cameraId, "RTSP", "Packets received=" + std::to_string(packets) + " bytes=" + std::to_string(bytes) + " corrupt/dropped=" + std::to_string(corrupt));
            nextStats = now + config_.statsInterval;
        }
        if (now >= deadline_) throw std::runtime_error("Video packet timeout");
    }
    log(config_.cameraId, "RTSP", "Session packets=" + std::to_string(packets));
}
}
