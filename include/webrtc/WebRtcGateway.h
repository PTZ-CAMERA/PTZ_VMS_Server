#pragma once
#include "core/Result.h"
#include <string>
struct AVPacket;
struct AVFormatContext;
namespace vms {
struct WebRtcClient { std::string cameraId, clientId, offerSdp; };
class WebRtcGateway {
public:
    virtual ~WebRtcGateway() = default;
    virtual Result startStream(const std::string& cameraId, const AVFormatContext*, int videoIndex) = 0;
    virtual Result stopStream(const std::string& cameraId) = 0;
    virtual Result addClient(const WebRtcClient&) = 0;
    virtual void removeClient(const std::string& clientId) = 0;
    virtual Result pushPacket(const std::string& cameraId, const AVPacket*) = 0;
    virtual Result addIceCandidate(const std::string& clientId, const std::string& candidate) = 0;
    // Answer SDP and local ICE are delivered to ClientServer through a future async sink.
};
}
