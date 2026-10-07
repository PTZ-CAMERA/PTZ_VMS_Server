#include "stream/RtspRelayServer.h"
#include "core/Logger.h"
#include <boost/asio.hpp>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/base64.h>
#include <libavutil/mathematics.h>
}
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>
namespace vms {
namespace {
namespace net = boost::asio;
using tcp = net::ip::tcp;
using udp = net::ip::udp;
using Error = boost::system::error_code;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr std::size_t MaxAccessUnit = 4 * 1024 * 1024;
constexpr std::size_t MaxPendingMedia = 8 * 1024 * 1024;
constexpr std::size_t MaxClientQueue = 8 * 1024 * 1024;
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
bool decimal(const std::string& value) {
    return !value.empty() && value.size() <= 10
        && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}
void u16(std::string& out, std::uint16_t n) { out.push_back(static_cast<char>(n >> 8)); out.push_back(static_cast<char>(n)); }
void u32(std::string& out, std::uint32_t n) { u16(out, static_cast<std::uint16_t>(n >> 16)); u16(out, static_cast<std::uint16_t>(n)); }
std::string base64(const Bytes& bytes) {
    if (bytes.empty()) return {};
    std::string result(AV_BASE64_SIZE(bytes.size()), '\0');
    av_base64_encode(result.data(), static_cast<int>(result.size()), bytes.data(), static_cast<int>(bytes.size()));
    result.resize(result.find('\0')); return result;
}
struct Nal { std::size_t offset, length; };
std::size_t startCode(const Bytes& bytes, std::size_t pos) {
    if (pos + 3 <= bytes.size() && bytes[pos] == 0 && bytes[pos + 1] == 0) {
        if (bytes[pos + 2] == 1) return 3;
        if (pos + 4 <= bytes.size() && bytes[pos + 2] == 0 && bytes[pos + 3] == 1) return 4;
    }
    return 0;
}
std::vector<Nal> nals(const Bytes& bytes, int lengthSize) {
    std::vector<Nal> result;
    if (bytes.empty()) return result;
    // AVCC length prefixes can resemble start codes; configured avcC takes precedence.
    if (lengthSize > 0) {
        std::size_t pos = 0;
        while (pos < bytes.size()) {
            if (pos + static_cast<std::size_t>(lengthSize) > bytes.size()) return {};
            std::size_t size = 0;
            for (int i = 0; i < lengthSize; ++i) size = (size << 8) | bytes[pos++];
            if (size == 0 || size > bytes.size() - pos || result.size() >= 1024) return {};
            result.push_back({pos, size}); pos += size;
        }
        return result;
    }
    std::size_t pos = 0;
    while (pos < bytes.size() && !startCode(bytes, pos)) ++pos;
    if (pos == bytes.size()) return {}; // RTSP demux path is Annex B or explicit AVCC.
    while (pos < bytes.size()) {
        const auto begin = pos + startCode(bytes, pos);
        pos = begin;
        while (pos < bytes.size() && !startCode(bytes, pos)) ++pos;
        auto end = pos;
        while (end > begin && bytes[end - 1] == 0) --end;
        if (end > begin) result.push_back({begin, end - begin});
        if (result.size() > 1024) return {};
    }
    return result;
}
struct Description {
    AVRational timeBase{1, 90000};
    int lengthSize = 0;
    Bytes sps, pps;
};
void parameters(Description& description, const Bytes& data) {
    if (data.size() > 6 && data[0] == 1) { // AVCDecoderConfigurationRecord
        description.lengthSize = (data[4] & 3) + 1;
        std::size_t pos = 6;
        const unsigned count = data[5] & 31;
        for (unsigned i = 0; i < count; ++i) {
            if (pos + 2 > data.size()) return;
            const auto size = (static_cast<std::size_t>(data[pos]) << 8) | data[pos + 1]; pos += 2;
            if (size > data.size() - pos) return;
            if (i == 0) description.sps.assign(data.begin() + pos, data.begin() + pos + size);
            pos += size;
        }
        if (pos >= data.size()) return;
        const unsigned ppsCount = data[pos++];
        for (unsigned i = 0; i < ppsCount; ++i) {
            if (pos + 2 > data.size()) return;
            const auto size = (static_cast<std::size_t>(data[pos]) << 8) | data[pos + 1]; pos += 2;
            if (size > data.size() - pos) return;
            if (i == 0) description.pps.assign(data.begin() + pos, data.begin() + pos + size);
            pos += size;
        }
    } else {
        for (const auto& nal : nals(data, 0)) {
            const int type = data[nal.offset] & 31;
            if (type == 7) description.sps.assign(data.begin() + nal.offset, data.begin() + nal.offset + nal.length);
            if (type == 8) description.pps.assign(data.begin() + nal.offset, data.begin() + nal.offset + nal.length);
        }
    }
}
struct AccessUnit { Bytes data; std::int64_t pts = AV_NOPTS_VALUE; };
struct MediaTask {
    enum class Kind { Packet, Configure, Offline, Resync };
    Kind kind = Kind::Packet;
    std::string id;
    std::shared_ptr<AccessUnit> unit;
    Description description;
};
struct Media {
    Description description;
    bool hasIdr = false;
    bool havePtsOrigin = false;
    std::int64_t ptsOrigin = 0;
    Clock::time_point firstArrival = Clock::now();
};
class Reader;
struct State : std::enable_shared_from_this<State> {
    explicit State(net::io_context& io) : context(io), acceptor(io), timer(io), random(std::random_device{}()) {}
    net::io_context& context;
    tcp::acceptor acceptor;
    net::steady_timer timer;
    std::map<std::uint64_t, std::shared_ptr<Reader>> readers;
    std::map<std::string, Media> media;
    std::mt19937 random;
    std::uint64_t nextReader = 0;
    RtspRelayOptions options;
    bool stopping = false;
    std::atomic<bool> running{false};
    std::mutex readyMutex;
    std::map<std::string, bool> readyCameras;
    // Bounded cross-thread ingress; at most one pending drain handler.
    std::mutex queueMutex;
    std::deque<MediaTask> pending;
    std::size_t pendingBytes = 0;
    bool drainPosted = false;
    bool overflow = false;
    bool knows(const std::string& id) {
        std::lock_guard<std::mutex> lock(readyMutex); return readyCameras.count(id) != 0;
    }
    bool isReady(const std::string& id) {
        std::lock_guard<std::mutex> lock(readyMutex);
        const auto it = readyCameras.find(id);
        return running.load() && it != readyCameras.end() && it->second;
    }
    void setReady(const std::string& id, bool ready) {
        std::lock_guard<std::mutex> lock(readyMutex); readyCameras[id] = ready;
    }
    std::string uri(const std::string& id) const {
        auto host = options.publicHost;
        if (host.find(':') != std::string::npos && host.front() != '[') host = '[' + host + ']';
        return "rtsp://" + host + ':' + std::to_string(options.port) + '/' + id;
    }
    void accept();
    void tick();
    void closeCamera(const std::string& id);
    void process(const std::string& id, const AccessUnit& unit);
    void enqueue(MediaTask task);
    void drain();
    void shutdown();
};
class Reader : public std::enable_shared_from_this<Reader> {
public:
    Reader(tcp::socket socket, const std::shared_ptr<State>& owner, std::uint64_t id)
        : socket_(std::move(socket)), udpRtp_(socket_.get_executor()), udpRtcp_(socket_.get_executor()), owner_(owner), id_(id), lastActivity_(Clock::now()) {
        sequence_ = static_cast<std::uint16_t>(owner->random());
        ssrc_ = owner->random(); timestampOffset_ = owner->random();
        session_ = std::to_string(id) + '-' + std::to_string(ssrc_);
    }
    const std::string& cameraId() const { return cameraId_; }
    void start() {
        Error ignored; socket_.set_option(tcp::no_delay(true), ignored); read();
    }
    void close() {
        if (closed_) return;
        closed_ = true;
        Error ignored; socket_.cancel(ignored); socket_.shutdown(tcp::socket::shutdown_both, ignored); socket_.close(ignored);
        udpRtp_.cancel(ignored); udpRtp_.close(ignored); udpRtcp_.cancel(ignored); udpRtcp_.close(ignored);
        if (auto owner = owner_.lock()) owner->readers.erase(id_);
        // In-flight write still owns the front buffer through shared_from_this().
    }
    void timer() {
        if (Clock::now() - lastActivity_ > std::chrono::seconds(setup_ ? 65 : 5)) return close();
        if (!playing_ || !packetCount_) return;
        std::string report;
        report.push_back(static_cast<char>(0x80)); report.push_back(static_cast<char>(200)); u16(report, 6); u32(report, ssrc_);
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
        const auto fraction = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds).count();
        u32(report, static_cast<std::uint32_t>(seconds.count() + 2208988800LL));
        u32(report, static_cast<std::uint32_t>((static_cast<std::uint64_t>(fraction) << 32) / 1000000000ULL));
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - lastRtpTime_).count();
        u32(report, lastTimestamp_ + static_cast<std::uint32_t>(elapsed * 9 / 100));
        u32(report, packetCount_); u32(report, octetCount_);
        const auto cname = std::string("mini-vms-") + std::to_string(ssrc_);
        std::string chunk; u32(chunk, ssrc_); chunk.push_back(1); chunk.push_back(static_cast<char>(cname.size())); chunk += cname; chunk.push_back(0);
        while (chunk.size() % 4) chunk.push_back(0);
        report.push_back(static_cast<char>(0x81)); report.push_back(static_cast<char>(202)); u16(report, static_cast<std::uint16_t>(chunk.size() / 4)); report += chunk;
        if (udpMode_) enqueueDatagram(true, std::move(report));
        else { std::string frame; interleave(frame, report, rtcpChannel_); send(std::move(frame)); }
    }
    void accessUnit(const AccessUnit& unit, const std::vector<Nal>& units, const Description& description, std::uint32_t timestamp, bool idr) {
        if (!playing_ || closed_ || (waitingIdr_ && !idr)) return;
        waitingIdr_ = false;
        std::string frames;
        const auto rtpTimestamp = timestamp + timestampOffset_;
        // Bootstrap every IDR with current parameter sets; joining clients wait for an IDR.
        if (idr) {
            packetize(frames, description.sps.data(), description.sps.size(), rtpTimestamp, false);
            packetize(frames, description.pps.data(), description.pps.size(), rtpTimestamp, false);
        }
        for (std::size_t i = 0; i < units.size(); ++i) {
            const auto& nal = units[i];
            packetize(frames, unit.data.data() + nal.offset, nal.length, rtpTimestamp, i + 1 == units.size());
        }
        if (!udpMode_) {
            lastTimestamp_ = rtpTimestamp; lastRtpTime_ = Clock::now(); send(std::move(frames));
        }
    }
private:
    static void interleave(std::string& destination, const std::string& payload, unsigned channel) {
        destination.push_back('$'); destination.push_back(static_cast<char>(channel));
        u16(destination, static_cast<std::uint16_t>(payload.size())); destination += payload;
    }
    void rtp(std::string& frames, const std::uint8_t* data, std::size_t size, std::uint32_t timestamp, bool marker, int fuHeader = -1, int fuIndicator = -1) {
        if (closed_) return;
        std::string packet;
        packet.push_back(static_cast<char>(0x80)); packet.push_back(static_cast<char>((marker ? 0x80 : 0) | 96));
        u16(packet, sequence_++); u32(packet, timestamp); u32(packet, ssrc_);
        if (fuHeader >= 0) { packet.push_back(static_cast<char>(fuIndicator)); packet.push_back(static_cast<char>(fuHeader)); }
        packet.append(reinterpret_cast<const char*>(data), size);
        if (udpMode_) enqueueDatagram(false, std::move(packet));
        else {
            ++packetCount_; octetCount_ += static_cast<std::uint32_t>(size + (fuHeader >= 0 ? 2 : 0));
            interleave(frames, packet, rtpChannel_);
        }
    }
    void packetize(std::string& frames, const std::uint8_t* nal, std::size_t size, std::uint32_t timestamp, bool marker) {
        if (!size) return;
        constexpr std::size_t MaxPayload = 1200;
        if (size <= MaxPayload) { rtp(frames, nal, size, timestamp, marker); return; }
        const int indicator = (nal[0] & 0xe0) | 28;
        for (std::size_t pos = 1; pos < size;) {
            const auto length = std::min(MaxPayload - 2, size - pos);
            const bool end = pos + length == size;
            const int header = (nal[0] & 31) | (pos == 1 ? 0x80 : 0) | (end ? 0x40 : 0);
            rtp(frames, nal + pos, length, timestamp, marker && end, header, indicator); pos += length;
        }
    }
    bool allocateUdp(unsigned rtpPort, unsigned rtcpPort) {
        Error error;
        const auto peer = socket_.remote_endpoint(error); if (error) return false;
        const auto protocol = peer.address().is_v6() ? udp::v6() : udp::v4();
        const net::ip::address bind = peer.address().is_v6() ? net::ip::address(net::ip::address_v6::any()) : net::ip::address(net::ip::address_v4::any());
        for (int attempt = 0; attempt < 64; ++attempt) {
            Error ignored; udpRtp_.close(ignored); udpRtcp_.close(ignored);
            udpRtp_.open(protocol, error); if (error) return false;
            udpRtp_.bind(udp::endpoint(bind, 0), error); if (error) continue;
            const auto port = udpRtp_.local_endpoint(error).port();
            if (error || (port & 1) || port == 65535) continue;
            udpRtcp_.open(protocol, error); if (error) continue;
            udpRtcp_.bind(udp::endpoint(bind, static_cast<unsigned short>(port + 1)), error); if (error) continue;
            udpRtp_.connect(udp::endpoint(peer.address(), static_cast<unsigned short>(rtpPort)), error); if (error) continue;
            udpRtcp_.connect(udp::endpoint(peer.address(), static_cast<unsigned short>(rtcpPort)), error); if (error) continue;
            udpRtp_.set_option(net::socket_base::send_buffer_size(1024 * 1024), ignored);
            return true;
        }
        Error ignored; udpRtp_.close(ignored); udpRtcp_.close(ignored); return false;
    }
    void enqueueDatagram(bool rtcp, std::string packet) {
        if (closed_) return;
        auto& queue = rtcp ? udpRtcpQueue_ : udpRtpQueue_;
        if (queue.size() >= (rtcp ? 32u : 8192u) || udpQueuedBytes_ + packet.size() > MaxClientQueue) return close();
        udpQueuedBytes_ += packet.size(); queue.push_back(std::move(packet));
        if (queue.size() == 1) writeDatagram(rtcp);
    }
    void writeDatagram(bool rtcp) {
        auto self = shared_from_this();
        auto& socket = rtcp ? udpRtcp_ : udpRtp_;
        auto& queue = rtcp ? udpRtcpQueue_ : udpRtpQueue_;
        socket.async_send(net::buffer(queue.front()), [self, rtcp](Error error, std::size_t size) {
            auto& queue = rtcp ? self->udpRtcpQueue_ : self->udpRtpQueue_;
            if (error || size != queue.front().size()) return self->close();
            if (!rtcp) {
                const auto& packet = queue.front();
                self->lastTimestamp_ = (std::uint32_t(static_cast<unsigned char>(packet[4])) << 24)
                    | (std::uint32_t(static_cast<unsigned char>(packet[5])) << 16)
                    | (std::uint32_t(static_cast<unsigned char>(packet[6])) << 8) | static_cast<unsigned char>(packet[7]);
                self->lastRtpTime_ = Clock::now(); ++self->packetCount_; self->octetCount_ += static_cast<std::uint32_t>(size - 12);
            }
            self->udpQueuedBytes_ -= queue.front().size(); queue.pop_front();
            if (!self->closed_ && !queue.empty()) self->writeDatagram(rtcp);
        });
    }
    void receiveRtcp() {
        auto self = shared_from_this();
        udpRtcp_.async_receive(net::buffer(rtcpInput_), [self](Error error, std::size_t size) {
            if (error) { if (error == net::error::message_size && !self->closed_) self->receiveRtcp(); else self->close(); return; }
            bool valid = size >= 8, bye = false; std::size_t pos = 0;
            while (valid && pos < size) {
                if (size - pos < 4 || (self->rtcpInput_[pos] >> 6) != 2) { valid = false; break; }
                const auto length = ((std::size_t(self->rtcpInput_[pos + 2]) << 8) | self->rtcpInput_[pos + 3]) * 4 + 4;
                if (length < 8 || length > size - pos || self->rtcpInput_[pos + 1] < 200 || self->rtcpInput_[pos + 1] > 206) { valid = false; break; }
                bye = bye || self->rtcpInput_[pos + 1] == 203; pos += length;
            }
            // Connected UDP socket accepts reports only from the negotiated RTCP endpoint.
            if (valid) { self->lastActivity_ = Clock::now(); if (bye) { self->close(); return; } }
            if (!self->closed_) self->receiveRtcp();
        });
    }
    void discardQueuedUdp() {
        for (auto* queue : {&udpRtpQueue_, &udpRtcpQueue_}) {
            while (queue->size() > 1) { udpQueuedBytes_ -= queue->back().size(); queue->pop_back(); }
        }
    }
    void send(std::string bytes) {
        if (closed_ || bytes.empty()) return;
        if (outgoing_.size() >= 128 || queuedBytes_ + bytes.size() > MaxClientQueue) return close();
        queuedBytes_ += bytes.size(); outgoing_.push_back(std::move(bytes));
        if (outgoing_.size() == 1) write();
    }
    void write() {
        auto self = shared_from_this();
        net::async_write(socket_, net::buffer(outgoing_.front()), [self](Error ec, std::size_t) {
            if (ec) return self->close();
            self->queuedBytes_ -= self->outgoing_.front().size(); self->outgoing_.pop_front();
            if (self->closeAfterWrite_ && self->outgoing_.empty()) return self->close();
            if (!self->closed_ && !self->outgoing_.empty()) self->write();
        });
    }
    void read() {
        auto self = shared_from_this();
        socket_.async_read_some(net::buffer(incoming_), [self](Error ec, std::size_t size) {
            if (ec) return self->close();
            self->buffer_.append(self->incoming_.data(), size);
            if (self->buffer_.size() > 131072) return self->close();
            self->parse();
            if (!self->closed_ && !self->closeAfterWrite_) self->read();
        });
    }
    void reply(const std::string& cseq, int code, const std::string& reason, const std::string& headers = {}, const std::string& body = {}) {
        send("RTSP/1.0 " + std::to_string(code) + ' ' + reason + "\r\nCSeq: " + cseq
            + "\r\nServer: MiniVmsServer\r\n" + headers + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    }
    void parse() {
        while (!closed_ && !closeAfterWrite_ && !buffer_.empty()) {
            if (buffer_[0] == '$') { // Receiver RTCP reports/control data, not upstream media.
                if (buffer_.size() < 4) return;
                const auto length = (static_cast<std::size_t>(static_cast<unsigned char>(buffer_[2])) << 8) | static_cast<unsigned char>(buffer_[3]);
                if (buffer_.size() < length + 4) return;
                lastActivity_ = Clock::now(); buffer_.erase(0, length + 4); continue;
            }
            const auto end = buffer_.find("\r\n\r\n");
            if (end == std::string::npos) { if (buffer_.size() > 16384) close(); return; }
            if (end > 16384) return close();
            std::istringstream lines(buffer_.substr(0, end));
            std::string first; std::getline(lines, first);
            std::istringstream line(first); std::string method, url, version, extra;
            line >> method >> url >> version;
            if (line >> extra || method.empty() || url.empty() || version != "RTSP/1.0") return close();
            std::map<std::string, std::string> headers;
            std::string header;
            while (std::getline(lines, header)) {
                const auto colon = header.find(':'); if (colon == std::string::npos) return close();
                if (!headers.emplace(lower(trim(header.substr(0, colon))), trim(header.substr(colon + 1))).second) return close();
            }
            const auto cseq = headers["cseq"]; if (!decimal(cseq)) return close();
            std::size_t bodySize = 0;
            if (headers.count("content-length")) {
                if (!decimal(headers["content-length"])) return close();
                const auto size = std::stoull(headers["content-length"]); if (size > 16384) return close(); bodySize = static_cast<std::size_t>(size);
            }
            if (buffer_.size() < end + 4 + bodySize) return;
            buffer_.erase(0, end + 4 + bodySize); lastActivity_ = Clock::now();
            handle(method, url, cseq, headers);
        }
    }
    void handle(const std::string& method, const std::string& url, const std::string& cseq, const std::map<std::string, std::string>& headers) {
        const auto owner = owner_.lock(); if (!owner) return close();
        if (method == "OPTIONS") { reply(cseq, 200, "OK", "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, GET_PARAMETER, TEARDOWN\r\n"); return; }
        std::string path = url;
        if (path.rfind("rtsp://", 0) == 0) {
            const auto slash = path.find('/', 7); path = slash == std::string::npos ? "/" : path.substr(slash);
        }
        const auto query = path.find('?');
        if (query != std::string::npos) {
            std::istringstream parameters(path.substr(query + 1)); std::string parameter;
            bool seenTransport = false;
            while (std::getline(parameters, parameter, '&')) if (parameter.rfind("transport=", 0) == 0) {
                const auto requested = lower(parameter.substr(10));
                if (seenTransport || (requested != "udp" && requested != "tcp") || (!forcedTransport_.empty() && forcedTransport_ != requested)) {
                    reply(cseq, 400, "Bad Request"); return;
                }
                seenTransport = true; forcedTransport_ = requested;
            }
            path.resize(query);
        }
        if (path.empty() || path[0] != '/') { reply(cseq, 400, "Bad Request"); return; }
        if (path.size() > 1 && path.back() == '/') path.pop_back();
        const auto slash = path.find('/', 1);
        const auto id = path.substr(1, slash == std::string::npos ? std::string::npos : slash - 1);
        if (slash != std::string::npos && path.substr(slash) != "/trackID=0") { reply(cseq, 404, "Not Found"); return; }
        const auto media = owner->media.find(id);
        if (media == owner->media.end()) {
            if (owner->knows(id)) reply(cseq, 503, "Service Unavailable", "Retry-After: 1\r\n");
            else reply(cseq, 404, "Not Found");
            return;
        }
        if (!owner->isReady(id)) { reply(cseq, 503, "Service Unavailable", "Retry-After: 1\r\n"); return; }
        if (!cameraId_.empty() && cameraId_ != id) { reply(cseq, 459, "Aggregate Operation Not Allowed"); return; }
        const auto sessionHeader = headers.find("session");
        if (setup_ && method != "DESCRIBE" && method != "SETUP") {
            if (sessionHeader == headers.end() || trim(sessionHeader->second.substr(0, sessionHeader->second.find(';'))) != session_) {
                reply(cseq, 454, "Session Not Found"); return;
            }
        }
        if (method == "DESCRIBE") {
            const auto& description = media->second.description;
            std::ostringstream profile;
            if (description.sps.size() >= 4) profile << std::hex << std::setfill('0') << std::setw(2) << unsigned(description.sps[1]) << std::setw(2) << unsigned(description.sps[2]) << std::setw(2) << unsigned(description.sps[3]);
            const auto uri = owner->uri(id);
            const auto sdp = "v=0\r\no=- " + std::to_string(id_) + " 1 IN IP4 127.0.0.1\r\ns=Mini VMS " + id
                + "\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\na=control:*\r\na=range:npt=now-\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;profile-level-id="
                + profile.str() + ";sprop-parameter-sets=" + base64(description.sps) + ',' + base64(description.pps) + "\r\na=control:trackID=0\r\n";
            reply(cseq, 200, "OK", "Content-Type: application/sdp\r\nContent-Base: " + uri + "/\r\n", sdp); return;
        }
        if (method == "SETUP") {
            if (setup_) { reply(cseq, 455, "Method Not Valid in This State"); return; }
            const auto transport = headers.find("transport");
            if (transport == headers.end()) { reply(cseq, 461, "Unsupported Transport"); return; }
            if (transport->second.find(',') != std::string::npos) { reply(cseq, 461, "Unsupported Transport"); return; }
            std::istringstream fields(lower(transport->second)); std::string token;
            std::getline(fields, token, ';'); const auto protocol = trim(token);
            const bool udp = protocol == "rtp/avp" || protocol == "rtp/avp/udp";
            if ((!udp && protocol != "rtp/avp/tcp") || (!forcedTransport_.empty() && forcedTransport_ != (udp ? "udp" : "tcp"))) {
                reply(cseq, 461, "Unsupported Transport"); return;
            }
            std::map<std::string, std::string> options;
            while (std::getline(fields, token, ';')) {
                token = trim(token); const auto equal = token.find('=');
                const auto key = trim(token.substr(0, equal));
                const auto value = equal == std::string::npos ? std::string{} : trim(token.substr(equal + 1));
                if (!options.emplace(key, value).second) { reply(cseq, 461, "Unsupported Transport"); return; }
            }
            const auto mode = options.find("mode");
            if (options.count("multicast") || (mode != options.end() && mode->second != "play" && mode->second != "\"play\"")) {
                reply(cseq, 461, "Unsupported Transport"); return;
            }
            if (options.count("destination")) {
                Error error; const auto destination = net::ip::make_address(options["destination"], error);
                if (error || destination != socket_.remote_endpoint().address()) { reply(cseq, 461, "Unsupported Transport"); return; }
            }
            auto pair = [&](const std::string& value, unsigned max, unsigned& first, unsigned& second) {
                const auto dash = value.find('-');
                if (dash == std::string::npos || !decimal(value.substr(0, dash)) || !decimal(value.substr(dash + 1))) return false;
                const auto a = std::stoull(value.substr(0, dash)), b = std::stoull(value.substr(dash + 1));
                if (a > max || b > max || a == b) return false;
                first = static_cast<unsigned>(a); second = static_cast<unsigned>(b); return true;
            };
            std::string replyTransport;
            if (udp) {
                unsigned rtpPort = 0, rtcpPort = 0;
                if (options.count("interleaved") || !options.count("client_port") || !pair(options["client_port"], 65535, rtpPort, rtcpPort) || !rtpPort || !rtcpPort) {
                    reply(cseq, 461, "Unsupported Transport"); return;
                }
                if (!allocateUdp(rtpPort, rtcpPort)) { reply(cseq, 453, "Not Enough Bandwidth"); return; }
                udpMode_ = true;
                std::ostringstream ssrc; ssrc << std::hex << std::setw(8) << std::setfill('0') << ssrc_;
                replyTransport = "RTP/AVP/UDP;unicast;client_port=" + std::to_string(rtpPort) + '-' + std::to_string(rtcpPort)
                    + ";server_port=" + std::to_string(udpRtp_.local_endpoint().port()) + '-' + std::to_string(udpRtcp_.local_endpoint().port())
                    + ";source=" + udpRtp_.local_endpoint().address().to_string() + ";ssrc=" + ssrc.str();
            } else {
                if (options.count("interleaved") && (!pair(options["interleaved"], 255, rtpChannel_, rtcpChannel_) || rtcpChannel_ != rtpChannel_ + 1)) {
                    reply(cseq, 461, "Unsupported Transport"); return;
                }
                replyTransport = "RTP/AVP/TCP;unicast;interleaved=" + std::to_string(rtpChannel_) + '-' + std::to_string(rtcpChannel_);
            }
            cameraId_ = id; setup_ = true;
            reply(cseq, 200, "OK", "Transport: " + replyTransport + "\r\nSession: " + session_ + ";timeout=60\r\n");
            if (udpMode_) receiveRtcp();
            log(id, "RELAY", udpMode_ ? "Client SETUP: UDP RTP/RTCP" : "Client SETUP: TCP interleaved"); return;
        }
        if (!setup_) { reply(cseq, 455, "Method Not Valid in This State"); return; }
        const auto session = "Session: " + session_ + "\r\n";
        if (method == "PLAY") {
            const auto range = headers.find("range");
            if (range != headers.end() && range->second != "npt=0.000-" && range->second != "npt=0-" && range->second != "npt=now-") {
                reply(cseq, 457, "Invalid Range"); return;
            }
            reply(cseq, 200, "OK", session + "Range: npt=now-\r\n"); playing_ = true; waitingIdr_ = true; return;
        }
        if (method == "PAUSE") { playing_ = false; if (udpMode_) discardQueuedUdp(); reply(cseq, 200, "OK", session); return; }
        if (method == "GET_PARAMETER") { reply(cseq, 200, "OK", session); return; }
        if (method == "TEARDOWN") { playing_ = false; reply(cseq, 200, "OK", session); closeAfterWrite_ = true; return; }
        reply(cseq, 405, "Method Not Allowed");
    }
    tcp::socket socket_;
    udp::socket udpRtp_, udpRtcp_;
    std::array<std::uint8_t, 4096> rtcpInput_{};
    std::deque<std::string> udpRtpQueue_, udpRtcpQueue_;
    std::size_t udpQueuedBytes_ = 0;
    bool udpMode_ = false;
    std::string forcedTransport_;
    std::weak_ptr<State> owner_;
    std::uint64_t id_;
    std::array<char, 8192> incoming_{};
    std::string buffer_, cameraId_, session_;
    std::deque<std::string> outgoing_;
    std::size_t queuedBytes_ = 0;
    Clock::time_point lastActivity_, lastRtpTime_{};
    std::uint16_t sequence_ = 0;
    std::uint32_t ssrc_ = 0, timestampOffset_ = 0, lastTimestamp_ = 0, packetCount_ = 0, octetCount_ = 0;
    unsigned rtpChannel_ = 0, rtcpChannel_ = 1;
    bool setup_ = false, playing_ = false, waitingIdr_ = true, closed_ = false, closeAfterWrite_ = false;
};
void State::accept() {
    auto self = shared_from_this();
    acceptor.async_accept([self](Error ec, tcp::socket socket) {
        if (!ec && !self->stopping && self->readers.size() < 32) {
            const auto id = ++self->nextReader;
            auto reader = std::make_shared<Reader>(std::move(socket), self, id);
            self->readers.emplace(id, reader); reader->start();
        }
        if (!self->stopping && ec != net::error::operation_aborted) self->accept();
    });
}
void State::tick() {
    timer.expires_after(std::chrono::seconds(2));
    auto self = shared_from_this();
    timer.async_wait([self](Error ec) {
        if (ec || self->stopping) return;
        const auto readers = self->readers;
        for (const auto& entry : readers) entry.second->timer();
        self->tick();
    });
}
void State::closeCamera(const std::string& id) {
    const auto copy = readers;
    for (const auto& entry : copy) if (entry.second->cameraId() == id) entry.second->close();
}
void State::process(const std::string& id, const AccessUnit& unit) {
    const auto it = media.find(id); if (it == media.end()) return;
    auto& stream = it->second;
    const auto units = nals(unit.data, stream.description.lengthSize);
    auto resync = [&] { closeCamera(id); stream.hasIdr = false; setReady(id, false); };
    if (units.empty()) { resync(); return; }
    for (const auto& nal : units) {
        const auto type = unit.data[nal.offset] & 31;
        if ((unit.data[nal.offset] & 0x80) || type == 0 || type >= 24) { resync(); return; }
    }
    bool idr = false;
    const auto oldSps = stream.description.sps, oldPps = stream.description.pps;
    for (const auto& nal : units) {
        const auto type = unit.data[nal.offset] & 31;
        if (type == 5) idr = true;
        if (type == 7) stream.description.sps.assign(unit.data.begin() + nal.offset, unit.data.begin() + nal.offset + nal.length);
        if (type == 8) stream.description.pps.assign(unit.data.begin() + nal.offset, unit.data.begin() + nal.offset + nal.length);
    }
    if ((!oldSps.empty() && oldSps != stream.description.sps) || (!oldPps.empty() && oldPps != stream.description.pps)) {
        closeCamera(id); stream.hasIdr = false;
    }
    if (idr && stream.description.sps.size() >= 4 && !stream.description.pps.empty() && !stream.hasIdr) {
        stream.hasIdr = true;
        log(id, "RELAY", "H264 stream ready; RTSP/TCP readers can connect");
    }
    setReady(id, stream.hasIdr);
    if (!stream.hasIdr) return;
    std::uint32_t ticks;
    if (unit.pts != AV_NOPTS_VALUE && stream.description.timeBase.num > 0 && stream.description.timeBase.den > 0) {
        const auto pts = av_rescale_q(unit.pts, stream.description.timeBase, AVRational{1, 90000});
        if (!stream.havePtsOrigin) { stream.ptsOrigin = pts; stream.havePtsOrigin = true; }
        ticks = static_cast<std::uint32_t>(static_cast<std::uint64_t>(pts) - static_cast<std::uint64_t>(stream.ptsOrigin));
    } else {
        ticks = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - stream.firstArrival).count() * 9 / 100);
    }
    const auto copy = readers;
    for (const auto& entry : copy) if (entry.second->cameraId() == id)
        entry.second->accessUnit(unit, units, stream.description, static_cast<std::uint32_t>(ticks), idr);
}
void State::enqueue(MediaTask task) {
    bool post = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (!running.load()) return;
        if (task.kind != MediaTask::Kind::Packet) {
            // A new generation supersedes queued media/control for this camera.
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->id == task.id && (task.kind != MediaTask::Kind::Resync
                    || it->kind == MediaTask::Kind::Packet || it->kind == MediaTask::Kind::Resync)) {
                    if (it->unit) pendingBytes -= it->unit->data.size();
                    it = pending.erase(it);
                } else ++it;
            }
        } else {
            if (pending.size() >= 256 || pendingBytes + task.unit->data.size() > MaxPendingMedia) {
                for (auto it = pending.begin(); it != pending.end();) {
                    if (it->kind == MediaTask::Kind::Packet) it = pending.erase(it); else ++it;
                }
                pendingBytes = 0; overflow = true;
            }
            pendingBytes += task.unit->data.size();
        }
        pending.push_back(std::move(task));
        if (!drainPosted) { drainPosted = true; post = true; }
    }
    if (post) net::post(context, [self = shared_from_this()] { self->drain(); });
}
void State::drain() {
    std::deque<MediaTask> batch;
    bool dropped;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        batch.swap(pending); pendingBytes = 0; dropped = overflow; overflow = false; drainPosted = false;
    }
    if (stopping) return;
    if (dropped) {
        for (auto& entry : media) { closeCamera(entry.first); entry.second.hasIdr = false; setReady(entry.first, false); }
        log("SERVER", "RELAY", "Ingress queue overflow; readers reset, waiting for IDR");
    }
    for (const auto& task : batch) {
        if (task.kind == MediaTask::Kind::Configure) {
            closeCamera(task.id); Media stream; stream.description = task.description;
            media[task.id] = std::move(stream); setReady(task.id, false);
        } else if (task.kind == MediaTask::Kind::Offline) {
            closeCamera(task.id); media.erase(task.id); setReady(task.id, false);
        } else if (task.kind == MediaTask::Kind::Resync) {
            closeCamera(task.id);
            const auto it = media.find(task.id); if (it != media.end()) it->second.hasIdr = false;
            setReady(task.id, false);
        } else process(task.id, *task.unit);
    }
}
void State::shutdown() {
    stopping = true; running.store(false);
    Error ignored; acceptor.cancel(ignored); acceptor.close(ignored); timer.cancel();
    const auto copy = readers; for (const auto& entry : copy) entry.second->close();
    { std::lock_guard<std::mutex> lock(queueMutex); pending.clear(); pendingBytes = 0; }
    { std::lock_guard<std::mutex> lock(readyMutex); readyCameras.clear(); }
}
}
struct RtspRelayServer::Impl {
    net::io_context context{1};
    std::shared_ptr<State> state = std::make_shared<State>(context);
    std::thread worker;
};
RtspRelayServer::RtspRelayServer() : impl_(std::make_unique<Impl>()) {}
RtspRelayServer::~RtspRelayServer() { stop(); }
Result RtspRelayServer::start(const RtspRelayOptions& options) {
    if (impl_->worker.joinable()) return {false, "RTSP relay is already started"};
    try {
        impl_->context.restart(); auto& state = *impl_->state;
        state.options = options; state.stopping = false;
        const tcp::endpoint endpoint(net::ip::make_address(options.bindAddress), options.port);
        state.acceptor.open(endpoint.protocol()); state.acceptor.set_option(net::socket_base::reuse_address(true));
        state.acceptor.bind(endpoint); state.acceptor.listen(net::socket_base::max_listen_connections);
        state.options.port = state.acceptor.local_endpoint().port();
        state.accept(); state.tick(); state.running.store(true);
        impl_->worker = std::thread([this] {
            try { impl_->context.run(); }
            catch (const std::exception& error) {
                try { log("SERVER", "RELAY", std::string("Worker error: ") + error.what()); } catch (...) {}
                impl_->state->shutdown();
            }
        });
        log("SERVER", "RELAY", "RTSP TCP/UDP listening on " + options.bindAddress + ':' + std::to_string(state.options.port));
        return {true, {}};
    } catch (const std::exception& error) {
        impl_->state->shutdown(); impl_->context.poll(); return {false, error.what()};
    }
}
void RtspRelayServer::stop() {
    if (impl_->worker.joinable()) {
        if (impl_->state->running.load()) net::post(impl_->context, [state = impl_->state] { state->shutdown(); });
        impl_->worker.join();
    }
}
void RtspRelayServer::configure(const std::string& id, const AVFormatContext* input, int index) {
    if (!impl_->state->running.load() || !input || index < 0 || static_cast<unsigned>(index) >= input->nb_streams) return;
    const auto* stream = input->streams[index]; if (stream->codecpar->codec_id != AV_CODEC_ID_H264) return;
    Description description; description.timeBase = stream->time_base;
    if (stream->codecpar->extradata && stream->codecpar->extradata_size > 0) {
        Bytes data(stream->codecpar->extradata, stream->codecpar->extradata + stream->codecpar->extradata_size); parameters(description, data);
    }
    impl_->state->setReady(id, false);
    MediaTask task; task.kind = MediaTask::Kind::Configure; task.id = id; task.description = std::move(description);
    impl_->state->enqueue(std::move(task));
}
void RtspRelayServer::pushPacket(const std::string& id, const AVPacket* packet) {
    if (!impl_->state->running.load() || !packet) return;
    if (packet->size <= 0 || !packet->data || static_cast<std::size_t>(packet->size) > MaxAccessUnit
        || (packet->flags & AV_PKT_FLAG_CORRUPT)) {
        MediaTask reset; reset.kind = MediaTask::Kind::Resync; reset.id = id;
        impl_->state->enqueue(std::move(reset)); return;
    }
    auto unit = std::make_shared<AccessUnit>(); unit->pts = packet->pts;
    unit->data.assign(packet->data, packet->data + packet->size);
    MediaTask task; task.id = id; task.unit = std::move(unit);
    impl_->state->enqueue(std::move(task));
}

void RtspRelayServer::setOffline(const std::string& id) {
    impl_->state->setReady(id, false);
    MediaTask task; task.kind = MediaTask::Kind::Offline; task.id = id;
    impl_->state->enqueue(std::move(task));
}

bool RtspRelayServer::ready(const std::string& id) const { return impl_->state->isReady(id); }
std::string RtspRelayServer::uri(const std::string& id) const { return impl_->state->uri(id); }
}
