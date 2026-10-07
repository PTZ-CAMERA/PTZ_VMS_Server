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
