#pragma once
#include "core/Config.h"
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
struct AVPacket;
struct AVFormatContext;
namespace vms {
class WebRtcGateway {
public:
    using Json=nlohmann::json;
    using Notify=std::function<void(std::uint64_t,const std::string&,const Json&)>;
    WebRtcGateway(StreamConfig,Notify);
    ~WebRtcGateway();
    static bool available();
    void start();
    void stop();
    void configure(const std::string&,const AVFormatContext*,int);
    void offline(const std::string&);
    void pushPacket(const std::string&,const AVPacket*);
    void request(const Json&,std::function<void(Json)>);
    void disconnect(std::uint64_t session);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
