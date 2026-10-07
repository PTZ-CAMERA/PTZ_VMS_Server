#include "stream/StreamManager.h"
#include <stdexcept>
namespace vms {
StreamManager::~StreamManager() { stopAll(); }
void StreamManager::start(const StreamConfig& config, StreamCallbacks callbacks) {
    if (sessions_.count(config.cameraId)) throw std::runtime_error("Stream already registered");
    auto session = std::make_unique<RtspSession>(config, std::move(callbacks));
    auto inserted = sessions_.emplace(config.cameraId, std::move(session));
    try { inserted.first->second->start(); }
    catch (...) { sessions_.erase(inserted.first); throw; }
}
void StreamManager::stop(const std::string& cameraId) {
    const auto it = sessions_.find(cameraId);
    if (it == sessions_.end()) return;
    it->second->requestStop();
    it->second->join();
    sessions_.erase(it);
}
void StreamManager::stopAll() {
    for (auto& entry : sessions_) entry.second->requestStop();
    for (auto& entry : sessions_) entry.second->join();
    sessions_.clear();
}
CameraStatus StreamManager::status(const std::string& cameraId) const {
    const auto it = sessions_.find(cameraId);
    return it == sessions_.end() ? CameraStatus::OFFLINE : it->second->status();
}
}
