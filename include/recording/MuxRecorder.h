#pragma once
#include "recording/Recorder.h"
#include "database/RecordingRepository.h"
extern "C" {
#include <libavcodec/codec_par.h>
#include <libavutil/rational.h>
}
#include <memory>
namespace vms {
struct RecordingSource {
    std::shared_ptr<AVCodecParameters> parameters;
    AVRational timeBase{1, 90000}, frameRate{0, 1};
    int videoIndex = 0;
};
RecordingSource copyRecordingSource(const AVFormatContext*, int index);
class MuxRecorder final : public Recorder {
public:
    explicit MuxRecorder(RecordingRepository&);
    ~MuxRecorder() override;
    Result start(const std::string&, const AVFormatContext*, int, const RecordingOptions&) override;
    Result start(const std::string&, const RecordingSource&, const RecordingOptions&);
    void setPacketTime(std::int64_t utcMs);
    Result writePacket(const AVPacket*) override;
    Result stop() override;
    void abort() noexcept;
    bool active() const;
    std::string filePath() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
