#include "core/Config.h"
#include "stream/StreamManager.h"
#include "camera/CameraManager.h"
#include "recording/Recorder.h"
#include "event/EventManager.h"
#include "database/DatabaseManager.h"
#include "onvif/OnvifClient.h"
#include "ptz/PtzCommandRouter.h"
#include "client/ClientServer.h"
#include "webrtc/WebRtcGateway.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool condition) { if (!condition) throw std::runtime_error("Test assertion failed"); }
}
int main() {
    const auto path = std::filesystem::temp_directory_path() / "mini-vms-unit.conf";
    try {
        { std::ofstream f(path); f << "# comment\r\ncamera_id=CAM02\r\nrtsp_url=rtsp://localhost:8554/stream?a=b\r\nread_timeout_ms=1000\r\n"; }
        const auto config = vms::loadConfig(path.string());
        require(config.cameraId == "CAM02" && config.readTimeout.count() == 1000);
        require(config.rtspUrl == "rtsp://localhost:8554/stream?a=b");
        for (const auto* text : {"rtsp_url=file:///tmp/x\n", "rtsp_url=rtsp://\n", "rtsp_url=rtsp://localhost\nread_timeout_ms=0\n", "rtsp_url=rtsp://localhost\nread_timeout_ms=1000oops\n", "rtsp_url=rtsp://localhost\nrtsp_transport=invalid\n", "rtsp_url=rtsp://localhost\nrtsp_url=rtsp://other\n", "rtsp_url=rtsp://localhost\nunknown_key=x\n"}) {
            { std::ofstream f(path); f << text; }
            bool rejected = false;
            try { (void)vms::loadConfig(path.string()); } catch (const std::exception&) { rejected = true; }
            require(rejected);
        }
        vms::StreamManager manager;
        // Pi가 받지 않는 Pull 상한은 시작 시 거절하여 반복 SOAP Fault를 방지한다.
        for (const auto* text : {"events_pull_seconds=11\n", "events_message_limit=65\n", "webrtc_gateway_url=rtsp://localhost:8889\n", "webrtc_gateway_url=http://user:password@localhost:8889\n", "webrtc_gateway_url=http://localhost:8889/cam\n"}) {
            { std::ofstream f(path); f << text; }
            bool rejected=false;
            try { (void)vms::loadConfig(path.string()); } catch (const std::exception&) { rejected=true; }
            require(rejected);
        }
        require(manager.status("missing") == vms::CameraStatus::OFFLINE);
        manager.stop("missing");
        manager.stopAll();
        std::filesystem::remove(path);
        std::cout << "Configuration and interface checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove(path);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
