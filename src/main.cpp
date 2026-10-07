#include "core/Config.h"
#include "core/Logger.h"
#include "stream/StreamManager.h"
#include "stream/RtspRelayServer.h"
#include "client/WebSocketServer.h"
#include "camera/CameraService.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}
#include <csignal>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
volatile std::sig_atomic_t interrupted = 0;
void signalHandler(int) { interrupted = 1; }
struct Network {
    Network() { if (avformat_network_init() < 0) throw std::runtime_error("FFmpeg network initialization failed"); }
    ~Network() { avformat_network_deinit(); }
};
}
int main(int argc, char** argv) {
    try {
        std::string path = "config/vms.conf";
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "Usage: vms-server [--config path]\n";
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--config") path = argv[2];
        else if (argc != 1) throw std::runtime_error("Usage: vms-server [--config path]");
        const auto config = vms::loadConfig(path);
        // FFmpeg diagnostics can expose URL credentials. Use our camera-ID based logs.
        av_log_set_level(AV_LOG_QUIET);
        Network network;
        if (std::signal(SIGINT, signalHandler) == SIG_ERR || std::signal(SIGTERM, signalHandler) == SIG_ERR)
            throw std::runtime_error("Cannot install termination handlers");
#ifdef SIGBREAK
        if (std::signal(SIGBREAK, signalHandler) == SIG_ERR)
            throw std::runtime_error("Cannot install console break handler");
#endif
        vms::RtspRelayServer relay;
        const auto relayResult = relay.start({config.relayBind, config.relayPublicHost, config.relayPort});
        if (!relayResult.ok) throw std::runtime_error("RTSP relay startup failed: " + relayResult.error);
        vms::WebSocketServer clientServer;
        vms::CameraService cameras(config, clientServer, relay);
        clientServer.setCommandHandler([&](const nlohmann::json& request, std::function<void(nlohmann::json)> done) {
            cameras.request(request, std::move(done));
        });
        clientServer.setDisconnectHandler([&](std::uint64_t session) { cameras.disconnect(session); });
        const auto apiResult = clientServer.start({config.clientBind, config.clientPort});
        if (!apiResult.ok) throw std::runtime_error("WebSocket startup failed: " + apiResult.error);
        vms::log(config.cameraId, "SERVER", "Mini VMS starting; Ctrl+C to stop");
        cameras.start();
        while (!interrupted) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        vms::log(config.cameraId, "SERVER", "Shutdown requested");
        clientServer.stop();
        cameras.stop();
        relay.stop();
        vms::log(config.cameraId, "SERVER", "Shutdown complete");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[SERVER] " << error.what() << '\n';
        return 1;
    }
}
