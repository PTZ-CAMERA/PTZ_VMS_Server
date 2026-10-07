#include "client/WebSocketServer.h"
#include "core/Logger.h"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <sstream>
#include <utility>
namespace vms {
namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using tcp = net::ip::tcp;
using Json = nlohmann::json;
using Error = boost::system::error_code;
const char* statusName(CameraStatus status) {
    switch (status) {
    case CameraStatus::OFFLINE: return "OFFLINE";
    case CameraStatus::CONNECTING: return "CONNECTING";
    case CameraStatus::ONLINE: return "ONLINE";
    case CameraStatus::ERROR: return "ERROR";
    }
    return "ERROR";
}
Json cameraJson(const CameraSnapshot& s) {
    return {{"id", s.id}, {"name", s.name}, {"ipAddress", s.ipAddress}, {"status", statusName(s.status)},
        {"recording", s.recording}, {"recordingRequested", s.recordingRequested}, {"recordingState", s.recordingState}, {"recordingError", s.recordingError}, {"onvifStatus", s.onvifStatus}, {"webRtcStatus", "NOT_IMPLEMENTED"},
        {"codec", s.codec}, {"width", s.width}, {"height", s.height}, {"fps", s.fps},
        {"timeBase", {{"num", s.timeBaseNum}, {"den", s.timeBaseDen}}},
        {"packets", s.packets}, {"bytes", s.bytes},
        {"capabilities", {{"ptz", s.ptzReady}, {"ptzCenter", s.ptzCenter}, {"tracking", false}, {"live", s.streamReady}, {"recordings", true}, {"events", false}}}};
}
class Session;
struct State : std::enable_shared_from_this<State> {
    explicit State(net::io_context& io) : context(io), acceptor(io), timer(io) {}
    net::io_context& context;
    tcp::acceptor acceptor;
    net::steady_timer timer;
    std::map<std::uint64_t, std::shared_ptr<Session>> sessions;
    std::uint64_t nextSession = 0;
    std::mutex cameraMutex;
    std::map<std::string, CameraSnapshot> cameras;
    std::map<std::string, std::string> lastNotifications;
    bool stopping = false;
    WebSocketServer::CommandHandler commandHandler;
    std::function<void(std::uint64_t)> disconnectHandler;
    std::map<std::string, CameraSnapshot> snapshot() {
        std::lock_guard<std::mutex> lock(cameraMutex);
        return cameras;
    }
    void accept();
    void tick();
    void broadcast(const std::string& text);
    void shutdown();
    std::pair<http::status, Json> streamResponse(const std::string& target) {
        auto failure = [](http::status status, const char* code, const char* message) {
            return std::make_pair(status, Json{{"version", 1}, {"ok", false}, {"error", {{"code", code}, {"message", message}}}});
        };
        const auto question = target.find('?');
        const auto path = target.substr(0, question);
        std::string transport = "tcp"; bool selectedTransport = false;
        if (question != std::string::npos) {
            std::istringstream parameters(target.substr(question + 1)); std::string parameter;
            while (std::getline(parameters, parameter, '&')) {
                if (selectedTransport || parameter.rfind("transport=", 0) != 0) return failure(http::status::bad_request, "INVALID_TRANSPORT", "Use transport=tcp or transport=udp");
                selectedTransport = true; transport = parameter.substr(10);
                if (transport != "tcp" && transport != "udp") return failure(http::status::bad_request, "INVALID_TRANSPORT", "Use transport=tcp or transport=udp");
            }
        }
        const std::string prefix = "/api/v1/cameras/", suffix = "/stream";
        if (path.size() <= prefix.size() + suffix.size() || path.rfind(prefix, 0) != 0
            || path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0)
            return failure(http::status::not_found, "NOT_FOUND", "REST endpoint does not exist");
        const auto id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
        if (id.empty() || id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
            return failure(http::status::bad_request, "INVALID_CAMERA_ID", "Invalid cameraId");
        const auto copy = snapshot();
        const auto entry = copy.find(id);
        if (entry == copy.end()) return failure(http::status::not_found, "CAMERA_NOT_FOUND", "Camera is not registered");
        const auto& camera = entry->second;
        const bool ready = camera.status == CameraStatus::ONLINE && camera.streamReady;
        Json data{{"cameraId", id}, {"ready", ready}, {"protocol", "rtsp"}, {"transport", transport}, {"transports", Json::array({"tcp", "udp"})},
            {"uri", camera.streamUri + (selectedTransport ? "?transport=" + transport : std::string{})}, {"codec", camera.codec}, {"width", camera.width}, {"height", camera.height}, {"fps", camera.fps}};
        if (!ready) {
            auto result = failure(http::status::service_unavailable, "STREAM_NOT_READY", "VMS stream is waiting for camera media and an IDR frame");
            result.second["data"] = std::move(data); return result;
        }
        return {http::status::ok, {{"version", 1}, {"ok", true}, {"data", std::move(data)}}};
    }
    Json dispatch(const Json& request) {
        Json id = nullptr;
        if (request.is_object() && request.contains("requestId") && request["requestId"].is_string()) id = request["requestId"];
        auto failure = [&](const char* code, const char* message) {
            return Json{{"version", 1}, {"type", "response"}, {"requestId", id}, {"ok", false},
                        {"error", {{"code", code}, {"message", message}}}};
        };
        if (!request.is_object() || !request.contains("version") || !request["version"].is_number_integer()
            || request["version"] != 1 || !id.is_string() || id.get_ref<const std::string&>().empty()
            || id.get_ref<const std::string&>().size() > 128 || !request.contains("command") || !request["command"].is_string())
            return failure("INVALID_REQUEST", "Expected version=1, string requestId and command");
        const auto command = request["command"].get<std::string>();
        if (command != "GET_CAMERA_LIST" && command != "GET_CAMERA_STATUS")
            return failure("NOT_SUPPORTED", "Command is not implemented by this VMS version");
        const auto copy = snapshot();
        Json data;
        if (command == "GET_CAMERA_LIST") {
            Json camerasJson = Json::array();
            for (const auto& entry : copy) camerasJson.push_back(cameraJson(entry.second));
            data = {{"cameras", camerasJson}};
        } else {
            if (!request.contains("cameraId") || !request["cameraId"].is_string() || request["cameraId"].get_ref<const std::string&>().empty())
                return failure("INVALID_REQUEST", "cameraId is required");
            const auto camera = copy.find(request["cameraId"].get<std::string>());
            if (camera == copy.end()) return failure("CAMERA_NOT_FOUND", "Camera is not registered");
            data = {{"camera", cameraJson(camera->second)}};
        }
        return {{"version", 1}, {"type", "response"}, {"requestId", id}, {"ok", true}, {"data", data}};
    }
};
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, const std::shared_ptr<State>& owner, std::uint64_t id)
        : socket_(std::move(socket)), owner_(owner), id_(id) {}
    void start() {
        parser_.body_limit(1024);
        parser_.header_limit(8192);
        beast::get_lowest_layer(socket_).expires_after(std::chrono::seconds(5));
        auto self = shared_from_this();
        http::async_read(beast::get_lowest_layer(socket_), buffer_, parser_, [self](Error ec, std::size_t) {
            if (ec) return self->close();
            self->request_ = self->parser_.release();
            if (self->request_.target() != "/ws" || !ws::is_upgrade(self->request_)) {
                self->serveHttp(); return;
            }
            beast::get_lowest_layer(self->socket_).expires_never();
            self->socket_.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
            self->socket_.read_message_max(65536);
            self->socket_.text(true);
            self->socket_.async_accept(self->request_, [self](Error error) {
                if (error) return self->close();
                self->ready_ = true;
                self->read();
            });
        });
    }
    void send(const std::string& text) {
        if (!ready_ || closed_) return;
        // Bounded write queue: a slow client cannot exhaust memory or delay ingest.
        if (outgoing_.size() >= 64 || text.size() > 65536) return close();
        outgoing_.push_back(text);
        if (outgoing_.size() == 1) write();
    }
    void close() {
        if (closed_) return;
        closed_ = true;
        Error ignored;
        auto& tcpSocket = beast::get_lowest_layer(socket_).socket();
        tcpSocket.cancel(ignored);
        tcpSocket.shutdown(tcp::socket::shutdown_both, ignored);
        tcpSocket.close(ignored);
        if (const auto owner = owner_.lock()) {
            if (ready_ && owner->disconnectHandler) owner->disconnectHandler(id_);
            owner->sessions.erase(id_);
        }
        // Keep queued buffers until any in-flight async_write completion runs.
    }
private:
    void serveHttp() {
        const auto owner = owner_.lock(); if (!owner) return close();
        std::pair<http::status, Json> result;
        if (request_.method() != http::verb::get) {
            result = {http::status::method_not_allowed, {{"version", 1}, {"ok", false},
                {"error", {{"code", "METHOD_NOT_ALLOWED"}, {"message", "Use GET"}}}}};
        } else result = owner->streamResponse(std::string(request_.target()));
        httpResponse_.version(request_.version()); httpResponse_.result(result.first);
        httpResponse_.set(http::field::content_type, "application/json; charset=utf-8");
        httpResponse_.set(http::field::cache_control, "no-store");
        if (result.first == http::status::method_not_allowed) httpResponse_.set(http::field::allow, "GET");
        if (result.first == http::status::service_unavailable) httpResponse_.set(http::field::retry_after, "1");
        httpResponse_.keep_alive(false); httpResponse_.body() = result.second.dump(); httpResponse_.prepare_payload();
        auto self = shared_from_this();
        http::async_write(beast::get_lowest_layer(socket_), httpResponse_, [self](Error, std::size_t) { self->close(); });
    }
    void read() {
        auto self = shared_from_this();
        socket_.async_read(buffer_, [self](Error ec, std::size_t) {
            if (ec) return self->close();
            const auto text = beast::buffers_to_string(self->buffer_.data());
            self->buffer_.consume(self->buffer_.size());
            const auto owner = self->owner_.lock();
            if (!owner) return self->close();
            auto request = Json::parse(text, nullptr, false);
            if (!self->socket_.got_text() || request.is_discarded()) request = nullptr;
            const auto preliminary = owner->dispatch(request);
            const auto command = request.is_object() && request.contains("command") && request["command"].is_string()
                ? request["command"].get<std::string>() : std::string{};
            if (owner->commandHandler && (command == "DISCOVER_CAMERAS" || command == "REGISTER_CAMERA" || command == "START_RECORDING" || command == "STOP_RECORDING" || command == "GET_RECORDINGS" || command == "PTZ_MOVE" || command == "PTZ_STOP" || command == "PTZ_CENTER")
                && preliminary.contains("error") && preliminary["error"].value("code", std::string{}) == "NOT_SUPPORTED") {
                std::weak_ptr<Session> weak = self;
                std::weak_ptr<State> weakOwner = owner;
                request["_sessionId"] = self->id_; // Never trust client-supplied session identity.
                owner->commandHandler(request, [weak, weakOwner](Json result) {
                    if (auto state = weakOwner.lock()) net::post(state->context, [weak, result = std::move(result)] {
                        if (auto session = weak.lock()) session->send(result.dump());
                    });
                });
            } else self->send(preliminary.dump());
            if (!self->closed_) self->read();
        });
    }
    void write() {
        auto self = shared_from_this();
        socket_.async_write(net::buffer(outgoing_.front()), [self](Error ec, std::size_t) {
            if (ec) return self->close();
            self->outgoing_.pop_front();
            if (!self->closed_ && !self->outgoing_.empty()) self->write();
        });
    }
    ws::stream<beast::tcp_stream> socket_;
    beast::flat_buffer buffer_;
    http::request_parser<http::string_body> parser_;
    http::request<http::string_body> request_;
    http::response<http::string_body> httpResponse_;
    std::weak_ptr<State> owner_;
    std::uint64_t id_;
    std::deque<std::string> outgoing_;
    bool ready_ = false, closed_ = false;
};
void State::accept() {
    auto self = shared_from_this();
    acceptor.async_accept([self](Error ec, tcp::socket socket) {
        if (!ec && !self->stopping) {
            if (self->sessions.size() < 32) {
                const auto id = ++self->nextSession;
                auto session = std::make_shared<Session>(std::move(socket), self, id);
                self->sessions.emplace(id, session);
                session->start();
            } else {
                Error ignored; socket.close(ignored);
            }
        }
        if (!self->stopping && ec != net::error::operation_aborted) self->accept();
    });
}
void State::broadcast(const std::string& text) {
    // send may remove a slow session; iterate a stable copy.
    const auto copy = sessions;
    for (const auto& entry : copy) entry.second->send(text);
}
void State::tick() {
    timer.expires_after(std::chrono::seconds(1));
    auto self = shared_from_this();
    timer.async_wait([self](Error ec) {
        if (ec || self->stopping) return;
        for (const auto& entry : self->snapshot()) {
            const auto text = Json{{"version", 1}, {"type", "notification"}, {"event", "CAMERA_STATUS"},
                {"cameraId", entry.first}, {"data", {{"camera", cameraJson(entry.second)}}}}.dump();
            if (self->lastNotifications[entry.first] != text) {
                self->lastNotifications[entry.first] = text;
                self->broadcast(text);
            }
        }
        self->tick();
    });
}
void State::shutdown() {
    stopping = true;
    Error ignored;
    acceptor.cancel(ignored); acceptor.close(ignored); timer.cancel();
    const auto copy = sessions;
    for (const auto& entry : copy) entry.second->close();
}
}
struct WebSocketServer::Impl {
    net::io_context context{1};
    std::shared_ptr<State> state = std::make_shared<State>(context);
    std::thread worker;
    std::atomic<bool> running{false};
    unsigned short boundPort = 0;
};
WebSocketServer::WebSocketServer() : impl_(std::make_unique<Impl>()) {}
WebSocketServer::~WebSocketServer() { stop(); }
Result WebSocketServer::start(const ClientServerOptions& options) {
    if (impl_->worker.joinable()) return {false, "WebSocket server already started"};
    try {
        impl_->context.restart();
        impl_->state->stopping = false;
        impl_->state->lastNotifications.clear();
        const auto address = net::ip::make_address(options.bindAddress);
        const tcp::endpoint endpoint(address, options.port);
        auto& acceptor = impl_->state->acceptor;
        acceptor.open(endpoint.protocol());
        acceptor.set_option(net::socket_base::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen(net::socket_base::max_listen_connections);
        impl_->boundPort = acceptor.local_endpoint().port();
        impl_->state->accept(); impl_->state->tick();
        impl_->running.store(true);
        impl_->worker = std::thread([this] {
            try { impl_->context.run(); }
            catch (const std::exception& error) {
                try { log("SERVER", "WS", std::string("Network worker error: ") + error.what()); } catch (...) {}
                impl_->state->shutdown();
            }
            impl_->running.store(false);
        });
        log("SERVER", "WS", "Listening ws://" + options.bindAddress + ':' + std::to_string(impl_->boundPort) + "/ws");
        return {true, {}};
    } catch (const std::exception& error) {
        impl_->state->shutdown();
        impl_->context.poll();
        impl_->running.store(false);
        return {false, error.what()};
    }
}
void WebSocketServer::stop() {
    if (impl_->worker.joinable()) {
        if (impl_->running.load()) net::post(impl_->context, [state = impl_->state] { state->shutdown(); });
        impl_->worker.join();
    }
}
void WebSocketServer::setDisconnectHandler(std::function<void(std::uint64_t)> handler) { impl_->state->disconnectHandler = std::move(handler); }
void WebSocketServer::setCommandHandler(CommandHandler handler) { impl_->state->commandHandler = std::move(handler); }
void WebSocketServer::updateRecording(const std::string& id, bool requested, bool active, const std::string& state, const std::string& error, const std::string& file) {
    std::lock_guard<std::mutex> lock(impl_->state->cameraMutex);
    auto it = impl_->state->cameras.find(id); if (it == impl_->state->cameras.end()) return;
    auto& camera = it->second; camera.recording = active; camera.recordingRequested = requested;
    camera.recordingState = state; camera.recordingError = error; camera.recordingFile = file;
}
void WebSocketServer::updateCamera(const CameraSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(impl_->state->cameraMutex);
    auto copy = snapshot;
    const auto existing = impl_->state->cameras.find(snapshot.id);
    if (existing != impl_->state->cameras.end()) {
        copy.recording = existing->second.recording; copy.recordingRequested = existing->second.recordingRequested;
        copy.recordingState = existing->second.recordingState; copy.recordingError = existing->second.recordingError; copy.recordingFile = existing->second.recordingFile;
    }
    impl_->state->cameras[snapshot.id] = std::move(copy);
}
void WebSocketServer::publish(const std::string& cameraId, const std::string& notification) {
    const auto payload = Json::parse(notification, nullptr, false);
    if (!impl_->running.load() || notification.size() > 65536 || !payload.is_object()) return;
    const auto text = Json{{"version", 1}, {"type", "notification"}, {"event", "MESSAGE"},
        {"cameraId", cameraId}, {"data", payload}}.dump();
    net::post(impl_->context, [state = impl_->state, text] { if (!state->stopping) state->broadcast(text); });
}
unsigned short WebSocketServer::port() const { return impl_->boundPort; }
}
