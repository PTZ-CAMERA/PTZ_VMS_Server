#include "webrtc/WebRtcGateway.h"
#include "recording/MuxRecorder.h"
#include "core/Logger.h"
extern "C" {
#include <libavcodec/bsf.h>
#include <libavutil/mathematics.h>
}
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#if VMS_HAS_WEBRTC
#include <rtc/rtc.hpp>
#endif
namespace vms {
using Json=nlohmann::json;
namespace {
Json reply(const Json& r,bool ok,Json data={},const char* code="WEBRTC_ERROR") {
    Json out{{"version",1},{"type","response"},{"requestId",r.value("requestId",std::string{})},{"ok",ok}};
    if(ok)out["data"]=std::move(data);else out["error"]={{"code",code},{"message","WebRTC request could not be completed"}};
    return out;
}
using Packet=std::shared_ptr<AVPacket>;
Packet clone(const AVPacket* input){auto* p=av_packet_clone(input);if(!p)throw std::bad_alloc();return Packet(p,[](AVPacket* q){av_packet_free(&q);});}
}
struct WebRtcGateway::Impl : std::enable_shared_from_this<Impl> {
    StreamConfig config;Notify notify;std::mutex mutex;std::condition_variable wake;std::thread worker;
    bool stopping=false;std::size_t queuedBytes=0;
    enum Kind{Configure,Offline,Media,Command,Disconnect};
    struct Task{Kind kind;std::string camera;RecordingSource source;Packet packet;Json request;std::function<void(Json)> done;std::uint64_t session=0;};
    std::deque<Task> tasks;
#if VMS_HAS_WEBRTC
    struct Source{AVBSFContext* filter=nullptr;~Source(){av_bsf_free(&filter);}};
    struct Peer{
        std::string camera,id;std::uint64_t session=0;
        std::shared_ptr<rtc::PeerConnection> pc;std::shared_ptr<rtc::Track> track;
        std::atomic<bool> replied{false},closed{false};
        std::chrono::steady_clock::time_point created=std::chrono::steady_clock::now();
        std::function<void(Json)> done;Json request;bool needKeyframe=true;std::int64_t firstPts=AV_NOPTS_VALUE;
    };
    std::map<std::string,std::unique_ptr<Source>> sources;
    std::map<std::uint64_t,std::shared_ptr<Peer>> peers;std::uint64_t sequence=0;
    void closePeer(const std::shared_ptr<Peer>& p,const char* reason){
        if(!p->replied.exchange(true)&&p->done)p->done(reply(p->request,false,{},reason));
        if(notify)notify(p->session,p->camera,{{"clientId",p->id},{"state","CLOSED"}});
        p->closed=true;p->pc->resetCallbacks();p->track->resetCallbacks();p->pc->close();
    }
    void remove(std::uint64_t session){auto it=peers.find(session);if(it==peers.end())return;auto p=it->second;peers.erase(it);closePeer(p,"WEBRTC_CLOSED");}
    void offlineCamera(const std::string& camera){
        for(auto it=peers.begin();it!=peers.end();)if(it->second->camera==camera){auto p=it->second;it=peers.erase(it);closePeer(p,"STREAM_NOT_READY");}else ++it;
        sources.erase(camera);
    }
    void configure(Task& t){
        offlineCamera(t.camera);auto s=std::make_unique<Source>();
        if(av_bsf_alloc(av_bsf_get_by_name("h264_mp4toannexb"),&s->filter)<0)throw std::runtime_error("Filter allocation failed");
        if(avcodec_parameters_copy(s->filter->par_in,t.source.parameters.get())<0)throw std::runtime_error("Codec copy failed");
        s->filter->time_base_in=t.source.timeBase;if(av_bsf_init(s->filter)<0)throw std::runtime_error("Filter init failed");sources[t.camera]=std::move(s);
    }
    std::string profile(Source& s){
        const auto* p=s.filter->par_out;const auto* b=p->extradata;
        for(int i=0;b&&i+8<p->extradata_size;++i){
            int start=0;if(b[i]==0&&b[i+1]==0&&b[i+2]==1)start=3;else if(b[i]==0&&b[i+1]==0&&b[i+2]==0&&b[i+3]==1)start=4;
            if(start&&(b[i+start]&31)==7){constexpr char h[]="0123456789abcdef";std::string out;for(int j=1;j<=3;++j){auto v=b[i+start+j];out+=h[v>>4];out+=h[v&15];}return "profile-level-id="+out+";packetization-mode=1;level-asymmetry-allowed=1";}
        }return rtc::DEFAULT_H264_VIDEO_PROFILE;
    }
    void command(Task& t){
        auto cmd=t.request.value("command",std::string{});
        if(!config.webRtcEnabled){t.done(reply(t.request,false,{},"WEBRTC_DISABLED"));return;}
        if(cmd=="WEBRTC_STOP"){remove(t.session);t.done(reply(t.request,true,{{"cameraId",t.camera}}));return;}
        if(cmd=="WEBRTC_ANSWER"){
            auto it=peers.find(t.session);
            if(it==peers.end()||it->second->camera!=t.camera||t.request.value("clientId",std::string{})!=it->second->id){t.done(reply(t.request,false,{},"WEBRTC_SESSION_NOT_FOUND"));return;}
            auto sdp=t.request.value("sdp",std::string{});if(sdp.empty()||sdp.size()>48000)throw std::runtime_error("Invalid SDP");
            it->second->pc->setRemoteDescription(rtc::Description(sdp,"answer"));t.done(reply(t.request,true,{{"cameraId",t.camera},{"clientId",it->second->id},{"phase","ANSWER_ACCEPTED"}}));return;
        }
        if(cmd!="WEBRTC_START"||!t.session||!sources.count(t.camera)){t.done(reply(t.request,false,{},"STREAM_NOT_READY"));return;}
        if(!peers.count(t.session)&&peers.size()>=8){t.done(reply(t.request,false,{},"WEBRTC_BUSY"));return;}
        remove(t.session);auto p=std::make_shared<Peer>();p->camera=t.camera;p->session=t.session;p->id=std::to_string(++sequence);p->done=t.done;p->request=t.request;
        rtc::Configuration opts;opts.disableAutoNegotiation=true;opts.portRangeBegin=config.webRtcPortBegin;opts.portRangeEnd=config.webRtcPortEnd;
        p->pc=std::make_shared<rtc::PeerConnection>(opts);
        const auto ssrc=1000+static_cast<std::uint32_t>(sequence);
        rtc::Description::Video video("video",rtc::Description::Direction::SendOnly);video.addH264Codec(96,profile(*sources.at(t.camera)));video.addSSRC(ssrc,"vms-video",p->camera,"video");p->track=p->pc->addTrack(video);
        auto rtp=std::make_shared<rtc::RtpPacketizationConfig>(ssrc,"vms-video",96,90000);
        auto packetizer=std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence,rtp);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());p->track->setMediaHandler(packetizer);
        std::weak_ptr<Peer> weak=p;std::weak_ptr<Impl> self=shared_from_this();
        // ICE 후보 수집이 완료된 SDP만 반환해 별도 candidate 메시지 순서 경쟁을 줄인다.
        p->pc->onGatheringStateChange([weak](rtc::PeerConnection::GatheringState state){
            auto p=weak.lock();if(!p||state!=rtc::PeerConnection::GatheringState::Complete||p->replied.exchange(true))return;
            auto d=p->pc->localDescription();if(d)p->done(reply(p->request,true,{{"cameraId",p->camera},{"clientId",p->id},{"type","offer"},{"sdp",std::string(*d)}}));else p->done(reply(p->request,false,{},"WEBRTC_NEGOTIATION_FAILED"));
        });
        p->pc->onStateChange([weak,self](rtc::PeerConnection::State state){
            auto p=weak.lock();auto s=self.lock();if(!p||!s)return;
            if(state==rtc::PeerConnection::State::Failed||state==rtc::PeerConnection::State::Closed||state==rtc::PeerConnection::State::Disconnected)p->closed=true;
            if(s->notify)s->notify(p->session,p->camera,{{"clientId",p->id},{"state",state==rtc::PeerConnection::State::Connected ? "CONNECTED" : p->closed ? "CLOSED" : "CONNECTING"}});
        });
        peers[t.session]=p;p->pc->setLocalDescription(rtc::Description::Type::Offer);
    }
    void media(Task& t){
        auto it=sources.find(t.camera);if(it==sources.end())return;auto* f=it->second->filter;if(av_bsf_send_packet(f,t.packet.get())<0)return;
        auto* raw=av_packet_alloc();if(!raw)throw std::bad_alloc();Packet frame(raw,[](AVPacket* q){av_packet_free(&q);});
        while(av_bsf_receive_packet(f,raw)==0){
            const bool key=(raw->flags&AV_PKT_FLAG_KEY)!=0;auto pts=raw->pts!=AV_NOPTS_VALUE ? raw->pts : raw->dts;
            if(pts!=AV_NOPTS_VALUE&&raw->size>0&&raw->size<=4*1024*1024)for(auto& item:peers){auto& p=*item.second;
                if(p.camera!=t.camera||p.closed||!p.track->isOpen()||(p.needKeyframe&&!key))continue;
                if(p.firstPts==AV_NOPTS_VALUE)p.firstPts=pts;
                p.needKeyframe=false;
                if(p.track->bufferedAmount()>4*1024*1024){p.closed=true;continue;}
                auto time=av_rescale_q(pts-p.firstPts,f->time_base_out,AVRational{1,90000});if(time<0)continue;
                // 원본 PTS를 90kHz RTP 시각으로 변환한다. 벽시계/녹화 UTC와 혼동하지 않는다.
                rtc::binary bytes;
                // IDR 앞에 SPS/PPS를 첨부해 늦게 접속한 브라우저의 디코더를 초기화한다.
                if(key&&f->par_out->extradata&&f->par_out->extradata_size>0){auto* extra=reinterpret_cast<const std::byte*>(f->par_out->extradata);bytes.insert(bytes.end(),extra,extra+f->par_out->extradata_size);}
                auto* data=reinterpret_cast<const std::byte*>(raw->data);bytes.insert(bytes.end(),data,data+raw->size);
                try{p.track->sendFrame(std::move(bytes),rtc::FrameInfo(static_cast<std::uint32_t>(time)));}catch(...){p.closed=true;}
            }av_packet_unref(raw);
        }
    }
#endif
    Impl(StreamConfig c,Notify n):config(std::move(c)),notify(std::move(n)){}
    bool enqueue(Task t){std::lock_guard<std::mutex> lock(mutex);if(stopping)return false;auto size=t.packet ? static_cast<std::size_t>(t.packet->size) : 0;
        if((size&&(queuedBytes+size>8*1024*1024||tasks.size()>256))||(!size&&tasks.size()>512))return false;
        queuedBytes+=size;tasks.push_back(std::move(t));wake.notify_one();return true;
    }
    void run(){for(;;){Task t{};bool have=false;
        {std::unique_lock<std::mutex> lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(100),[&]{return stopping||!tasks.empty();});if(stopping)break;
            if(!tasks.empty()){t=std::move(tasks.front());tasks.pop_front();if(t.packet)queuedBytes-=t.packet->size;have=true;}}
        if(have)try{
#if VMS_HAS_WEBRTC
            switch(t.kind){case Configure:configure(t);break;case Offline:offlineCamera(t.camera);break;case Media:media(t);break;case Command:command(t);break;case Disconnect:remove(t.session);break;}
#else
            if(t.done)t.done(reply(t.request,false,{},"WEBRTC_DISABLED"));
#endif
        }catch(...){if(t.done)t.done(reply(t.request,false,{},"WEBRTC_NEGOTIATION_FAILED"));else log(t.camera,"WEBRTC","Media operation failed");}
#if VMS_HAS_WEBRTC
        for(auto it=peers.begin();it!=peers.end();){auto p=it->second;if(p->closed||(!p->track->isOpen()&&std::chrono::steady_clock::now()-p->created>std::chrono::seconds(15))){it=peers.erase(it);closePeer(p,"WEBRTC_TIMEOUT");}else ++it;}
#endif
        }
#if VMS_HAS_WEBRTC
        while(!peers.empty())remove(peers.begin()->first);
        sources.clear();
#endif
        std::deque<Task> left;{std::lock_guard<std::mutex> lock(mutex);left.swap(tasks);queuedBytes=0;}for(auto& t:left)if(t.done)t.done(reply(t.request,false,{},"SHUTTING_DOWN"));
    }
};
bool WebRtcGateway::available(){return VMS_HAS_WEBRTC;}
WebRtcGateway::WebRtcGateway(StreamConfig c,Notify n):impl_(std::make_shared<Impl>(std::move(c),std::move(n))){}
WebRtcGateway::~WebRtcGateway(){stop();}
void WebRtcGateway::start(){
#if VMS_HAS_WEBRTC
    rtc::SetThreadPoolSize(2);rtc::InitLogger(rtc::LogLevel::None);
#endif
    impl_->worker=std::thread([p=impl_]{p->run();});
}
void WebRtcGateway::stop(){{std::lock_guard<std::mutex> lock(impl_->mutex);impl_->stopping=true;}impl_->wake.notify_all();if(impl_->worker.joinable())impl_->worker.join();}
void WebRtcGateway::configure(const std::string& id,const AVFormatContext* in,int index){
    if(!available() || !impl_->config.webRtcEnabled)return;
    try{Impl::Task t{};t.kind=Impl::Configure;t.camera=id;t.source=copyRecordingSource(in,index);impl_->enqueue(std::move(t));}
    catch(...){offline(id);log(id,"WEBRTC","Stream configuration failed");}
}
void WebRtcGateway::offline(const std::string& id){Impl::Task t{};t.kind=Impl::Offline;t.camera=id;impl_->enqueue(std::move(t));}
void WebRtcGateway::pushPacket(const std::string& id,const AVPacket* p){if(!available()||!p||p->size<=0||p->size>4*1024*1024||!impl_->config.webRtcEnabled)return;Impl::Task t{};t.kind=Impl::Media;t.camera=id;t.packet=clone(p);impl_->enqueue(std::move(t));}
void WebRtcGateway::request(const Json& r,std::function<void(Json)> done){
    if(!r.contains("cameraId") || !r["cameraId"].is_string() || r["cameraId"].get_ref<const std::string&>().empty() || r["cameraId"].get_ref<const std::string&>().size()>128){done(reply(r,false,{},"INVALID_REQUEST"));return;}
    Impl::Task t{};t.kind=Impl::Command;t.camera=r["cameraId"].get<std::string>();t.session=r.value("_sessionId",std::uint64_t{});t.request=r;t.done=done;
    if(!impl_->enqueue(std::move(t)))done(reply(r,false,{},"WEBRTC_BUSY"));
}
void WebRtcGateway::disconnect(std::uint64_t s){Impl::Task t{};t.kind=Impl::Disconnect;t.session=s;impl_->enqueue(std::move(t));}
}
