#pragma once
#include "recording/MuxRecorder.h"
#include <functional>
#include <nlohmann/json.hpp>
namespace vms {
struct RecordingStatus {
    bool requested = false, active = false;
    std::string state = "STOPPED", error, filePath;
};
class RecordingManager {
public:
    using StatusCallback = std::function<void(const std::string&, const RecordingStatus&)>;
    RecordingManager(RecordingOptions, std::string databasePath, StatusCallback,bool autoEnabled=false,unsigned postSeconds=10);
    ~RecordingManager();
    void start();
    void stop();
    void configure(const std::string&, const AVFormatContext*, int index);
    void offline(const std::string&);
    void pushPacket(const std::string&, const AVPacket*);
    void request(const nlohmann::json&, std::function<void(nlohmann::json)>);
    nlohmann::json snapshot(const std::string& cameraId) const;
    void detection(const std::string&,const nlohmann::json& detected,bool available);
    void tracking(const std::string&,bool enabled);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
