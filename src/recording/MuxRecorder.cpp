#include "recording/MuxRecorder.h"
#include "core/Logger.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
namespace vms {
namespace {
void check(int code, const char* operation) {
    if (code >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(code, error, sizeof(error));
    throw std::runtime_error(std::string(operation) + ": " + error);
}
std::int64_t offset(std::int64_t value, std::int64_t origin) {
    if ((origin < 0 && value > std::numeric_limits<std::int64_t>::max() + origin)
        || (origin > 0 && value < std::numeric_limits<std::int64_t>::min() + origin))
        throw std::runtime_error("Recording timestamp overflow");
    return value - origin;
}
struct Output {
    AVFormatContext* context = nullptr;
    ~Output() { if (context) { if (context->pb) avio_closep(&context->pb); avformat_free_context(context); } }
};
struct PacketFree { void operator()(AVPacket* p) const { av_packet_free(&p); } };
std::int64_t utcMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::pair<std::string, std::string> names(std::int64_t milliseconds) {
    const auto seconds = static_cast<std::time_t>(milliseconds / 1000); std::tm time{};
#ifdef _WIN32
    localtime_s(&time, &seconds);
#else
    localtime_r(&seconds, &time);
#endif
    std::ostringstream day, file; day << std::put_time(&time, "%Y-%m-%d"); file << std::put_time(&time, "%H-%M-%S") << '-' << std::setw(3) << std::setfill('0') << milliseconds % 1000;
    static std::atomic<unsigned> sequence{0}; file << '-' << ++sequence; return {day.str(), file.str()};
}
}
RecordingSource copyRecordingSource(const AVFormatContext* input, int index) {
    if (!input || index < 0 || static_cast<unsigned>(index) >= input->nb_streams) throw std::runtime_error("Invalid recording video stream");
    RecordingSource source; auto* raw = avcodec_parameters_alloc(); if (!raw) throw std::bad_alloc();
    source.parameters = std::shared_ptr<AVCodecParameters>(raw, [](AVCodecParameters* p) { avcodec_parameters_free(&p); });
    const auto* stream = input->streams[index]; check(avcodec_parameters_copy(raw, stream->codecpar), "Copy recording codec parameters");
    if (raw->codec_id != AV_CODEC_ID_H264) throw std::runtime_error("Recording supports H264 video only");
    source.timeBase = stream->time_base; source.frameRate = stream->avg_frame_rate;
    if (source.frameRate.num <= 0 || source.frameRate.den <= 0) source.frameRate = stream->r_frame_rate;
    if (source.timeBase.num <= 0 || source.timeBase.den <= 0) throw std::runtime_error("Invalid recording time base");
    source.videoIndex = index; return source;
}
struct MuxRecorder::Impl {
    RecordingRepository& repository;
    RecordingSource source;
    RecordingOptions options;
    std::string cameraId;
    std::unique_ptr<Output> output;
    std::filesystem::path temporary, final;
    std::string lastFile;
    std::int64_t firstDts = 0, firstPts = 0, lastDts = AV_NOPTS_VALUE, packetMs = 0, startMs = 0;
    double duration = 0;
    std::uint64_t packetCount = 0;
    bool enabled = false;
    explicit Impl(RecordingRepository& db) : repository(db) {}
    void open(const AVPacket* packet) {
        startMs = packetMs ? packetMs : utcMs(); const auto parts = names(startMs);
        const auto directory = std::filesystem::u8path(options.root) / cameraId / parts.first;
        std::filesystem::create_directories(directory);
        final = directory / (parts.second + '.' + options.container); temporary = final; temporary += ".part";
        while (std::filesystem::exists(final) || std::filesystem::exists(temporary)) {
            const auto next = names(startMs); final = directory / (next.second + '.' + options.container); temporary = final; temporary += ".part";
        }
        output = std::make_unique<Output>();
        check(avformat_alloc_output_context2(&output->context, nullptr, options.container == "mkv" ? "matroska" : "mp4", temporary.u8string().c_str()), "Create recording muxer");
        if (!output->context) throw std::bad_alloc();
        auto* stream = avformat_new_stream(output->context, nullptr); if (!stream) throw std::bad_alloc();
        check(avcodec_parameters_copy(stream->codecpar, source.parameters.get()), "Copy output codec parameters");
        stream->codecpar->codec_tag = 0; stream->time_base = source.timeBase; stream->avg_frame_rate = source.frameRate;
        check(avio_open(&output->context->pb, temporary.u8string().c_str(), AVIO_FLAG_WRITE), "Open recording file");
        check(avformat_write_header(output->context, nullptr), "Write recording header");
        firstDts = packet->dts; firstPts = packet->pts; lastDts = AV_NOPTS_VALUE; duration = 0; packetCount = 0;
        lastFile = temporary.u8string();
        log(cameraId, "REC", "Segment opened: " + final.filename().u8string());
    }
    void finish() {
        if (!output) return;
        if (!packetCount) { output.reset(); std::error_code error; std::filesystem::remove(temporary, error); return; }
        check(av_write_trailer(output->context), "Finalize recording");
        avio_flush(output->context->pb); check(output->context->pb->error, "Flush recording");
        check(avio_closep(&output->context->pb), "Close recording file"); output.reset();
        std::filesystem::rename(temporary, final); lastFile = final.u8string();
        RecordingEntry entry; entry.cameraId = cameraId; entry.filePath = lastFile;
        entry.startMs = startMs; entry.endMs = startMs + static_cast<std::int64_t>(std::ceil(duration * 1000));
        entry.duration = duration; entry.codec = "H264"; entry.width = source.parameters->width; entry.height = source.parameters->height;
        // 실제 캡처 UTC가 아니라 수신 UTC임을 별도 anchor로 보존한다.
        entry.hasTimeAnchor = true; entry.firstPts = firstPts; entry.firstDts = firstDts;
        entry.timeBaseNum = source.timeBase.num; entry.timeBaseDen = source.timeBase.den;
        repository.insert(entry);
        log(cameraId, "REC", "Segment saved: " + final.filename().u8string());
    }
};
MuxRecorder::MuxRecorder(RecordingRepository& db) : impl_(std::make_unique<Impl>(db)) {}
MuxRecorder::~MuxRecorder() { (void)stop(); }
Result MuxRecorder::start(const std::string& id, const AVFormatContext* input, int index, const RecordingOptions& options) {
    try { return start(id, copyRecordingSource(input, index), options); } catch (const std::exception& e) { return {false, e.what()}; }
}
Result MuxRecorder::start(const std::string& id, const RecordingSource& source, const RecordingOptions& options) {
    const auto stopped = stop(); if (!stopped.ok) return stopped;
    if (!source.parameters || options.segmentDuration.count() < 1 || (options.container != "mkv" && options.container != "mp4")) return {false, "Invalid recording options"};
    impl_->cameraId = id; impl_->source = source; impl_->options = options; impl_->lastFile.clear(); impl_->enabled = true; return {true, {}};
}
void MuxRecorder::setPacketTime(std::int64_t time) { impl_->packetMs = time; }
Result MuxRecorder::writePacket(const AVPacket* input) {
    try {
        if (!impl_->enabled || !input || !impl_->source.parameters || input->stream_index != impl_->source.videoIndex) return {true, {}};
        if (input->size <= 0 || (input->flags & AV_PKT_FLAG_CORRUPT)) { impl_->finish(); return {true, {}}; }
        std::unique_ptr<AVPacket, PacketFree> packet(av_packet_clone(input)); if (!packet) throw std::bad_alloc();
        if (packet->dts == AV_NOPTS_VALUE && impl_->source.parameters->video_delay > 0) { impl_->finish(); return {true, {}}; }
        if (packet->dts == AV_NOPTS_VALUE) packet->dts = packet->pts;
        if (packet->pts == AV_NOPTS_VALUE) packet->pts = packet->dts;
        if (packet->dts == AV_NOPTS_VALUE) { impl_->finish(); return {true, {}}; }
        if (impl_->output && impl_->lastDts != AV_NOPTS_VALUE && packet->dts <= impl_->lastDts) impl_->finish();
        const double elapsed = impl_->output ? (static_cast<double>(packet->dts) - static_cast<double>(impl_->firstDts)) * av_q2d(impl_->source.timeBase) : 0;
        if (impl_->output && elapsed >= impl_->options.segmentDuration.count() && (packet->flags & AV_PKT_FLAG_KEY)) impl_->finish();
        if (!impl_->output) {
            if (!(packet->flags & AV_PKT_FLAG_KEY)) return {true, {}};
            impl_->open(packet.get());
        }
        const auto rawDts = packet->dts;
        const double frameDuration = packet->duration > 0 ? packet->duration * av_q2d(impl_->source.timeBase)
            : impl_->source.frameRate.num > 0 ? av_q2d(av_inv_q(impl_->source.frameRate)) : 0;
        const double end = (static_cast<double>(packet->pts) - static_cast<double>(impl_->firstPts)) * av_q2d(impl_->source.timeBase) + frameDuration;
        if (!std::isfinite(end) || end > 86400 || end < -60) throw std::runtime_error("Recording timestamp discontinuity");
        packet->pts = offset(packet->pts, impl_->firstDts); packet->dts = offset(packet->dts, impl_->firstDts);
        av_packet_rescale_ts(packet.get(), impl_->source.timeBase, impl_->output->context->streams[0]->time_base);
        packet->stream_index = 0; packet->pos = -1;
        check(av_interleaved_write_frame(impl_->output->context, packet.get()), "Write recording packet");
        check(impl_->output->context->pb->error, "Recording disk I/O");
        impl_->duration = std::max(impl_->duration, end); impl_->lastDts = rawDts; ++impl_->packetCount;
        return {true, {}};
    } catch (const std::exception& e) { abort(); return {false, e.what()}; }
}
Result MuxRecorder::stop() {
    impl_->enabled = false;
    try { impl_->finish(); return {true, {}}; } catch (const std::exception& e) { abort(); return {false, e.what()}; }
}
void MuxRecorder::abort() noexcept { impl_->enabled = false; impl_->output.reset(); }
bool MuxRecorder::active() const { return impl_->output && impl_->packetCount > 0; }
std::string MuxRecorder::filePath() const { return impl_->lastFile; }
}
