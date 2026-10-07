#pragma once
#include "core/Result.h"
#include <memory>
#include <string>
struct AVFormatContext;
struct AVPacket;
namespace vms {
struct RtspRelayOptions {
    std::string bindAddress = "127.0.0.1";
    std::string publicHost = "127.0.0.1";
    unsigned short port = 8555;
};
// H.264 live relay, RTP/RTCP over UDP or interleaved TCP. No decoder/encoder.
// Lifecycle on main thread, media callbacks on the camera's ingest worker.
class RtspRelayServer {
public:
    RtspRelayServer();
    ~RtspRelayServer();
    RtspRelayServer(const RtspRelayServer&) = delete;
    RtspRelayServer& operator=(const RtspRelayServer&) = delete;
    Result start(const RtspRelayOptions&);
    void stop();
    void configure(const std::string& cameraId, const AVFormatContext*, int videoIndex);
    void pushPacket(const std::string& cameraId, const AVPacket*);
    void setOffline(const std::string& cameraId);
    bool ready(const std::string& cameraId) const;
    std::string uri(const std::string& cameraId) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
