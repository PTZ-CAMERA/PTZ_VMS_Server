#pragma once
#include "core/Result.h"
#include <chrono>
#include <string>
struct AVFormatContext;
struct AVPacket;
namespace vms {
struct RecordingOptions { std::string root = "recordings"; std::chrono::seconds segmentDuration{600}; std::string container = "mkv"; };
// M2: called on recorder worker. Copy codecpar/time_base at start; do not retain inputContext.
class Recorder {
public:
    virtual ~Recorder() = default;
    virtual Result start(const std::string& cameraId, const AVFormatContext* inputContext, int videoIndex, const RecordingOptions&) = 0;
    virtual Result writePacket(const AVPacket*) = 0;
    virtual Result stop() = 0;
};
}
