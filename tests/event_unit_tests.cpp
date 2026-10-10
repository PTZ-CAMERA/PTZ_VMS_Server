#include "core/UtcTimestamp.h"
#include "event/EventManager.h"
#include "event/EventRepository.h"
#include "database/RecordingRepository.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace { void requireAt(bool c,int line) { if (!c) throw std::runtime_error("Event assertion failed at line " + std::to_string(line)); } }
#define check(c) requireAt((c),__LINE__)
int main() {
    const auto root=std::filesystem::temp_directory_path()/("vms-event-unit-"+std::to_string(vms::utcNowMs()));
    try {
        using Json=nlohmann::json;
        check(vms::parseUtcMs("1970-01-01T00:00:00Z")==0);
        check(vms::parseUtcMs("2026-10-09T12:00:00+09:00")==vms::parseUtcMs("2026-10-09T03:00:00Z"));
        check(!vms::parseUtcMs("2026-02-29T00:00:00Z")); check(!vms::parseUtcMs("2026-10-09 03:00:00"));
        const std::string xml=R"(<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:e="http://www.onvif.org/ver10/events/wsdl" xmlns:n="http://docs.oasis-open.org/wsn/b-2" xmlns:v="http://www.onvif.org/ver10/schema"><s:Body><e:PullMessagesResponse><n:NotificationMessage><n:Topic>x:Analytics/PersonDetection</n:Topic><n:Message><v:Message UtcTime="2026-10-09T03:00:00Z" PropertyOperation="Initialized"><v:Source><v:SimpleItem Name="ProfileToken" Value="main"/></v:Source><v:Data><v:SimpleItem Name="Detected" Value="false"/></v:Data></v:Message></n:Message></n:NotificationMessage></e:PullMessagesResponse></s:Body></s:Envelope>)";
        const auto parsed=vms::parseEventMessages(xml); check(parsed.size()==1);
        auto m=vms::normalizeMetadata("CAM01","main",parsed[0],Json::object(),vms::utcNowMs());
        check(m["confidence"].is_null() && m["bboxX"].is_null() && m["detected"]==false);
        auto malformed=parsed[0]; malformed.data["score"]="nan"; malformed.data["x"]="0";
        auto mapped=vms::normalizeMetadata("CAM01","main",malformed,{{"confidence","score"},{"bboxX","x"}},vms::utcNowMs());
        check(mapped["confidence"].is_null() && mapped["bboxX"]==0);
        auto pi=parsed[0]; pi.data={{"Detected","true"},{"Confidence","0.92"},{"BoxX","250"},{"BoxY","120"},
            {"BoxWidth","140"},{"BoxHeight","260"},{"ImageWidth","1280"},{"ImageHeight","720"},{"PanAngle","102"},{"TiltAngle","88"},
            {"FrameId","9007199254740993"},{"StreamEpoch","9007199254740995"},{"FramePtsNs","9007199254740997"},
            {"CaptureUnixMs","1791514799900"},{"AnalysisUnixMs","1791514800000"},{"CaptureTimeSource","pts-estimated"}};
        const auto real=vms::normalizeMetadata("CAM01","main",pi,Json::object(),vms::utcNowMs());
        check(real["confidence"]==0.92 && real["bboxX"]==250 && real["panCommandAngle"]==102);
        check(real["frameId"]=="9007199254740993" && real["streamEpoch"]=="9007199254740995" && real["framePtsNs"]=="9007199254740997");
        check(real["captureTimeMs"]==1791514799900LL && real["analysisTimeMs"]==1791514800000LL);
        pi.data["CaptureUnixMs"]="0"; pi.data["FramePtsNs"]="-1";
        const auto unknown=vms::normalizeMetadata("CAM01","main",pi,Json::object(),vms::utcNowMs());
        check(unknown["captureTimeMs"].is_null() && unknown["framePtsNs"].is_null());
        std::filesystem::create_directories(root);
        const auto path=(root/"vms.db").string(); vms::EventRepository repo(path);
        check(!repo.transition(m)); m["propertyOperation"]="Changed"; m["detected"]=true;
        m["sourceTimeMs"]=m["sourceTimeMs"].get<std::int64_t>()+1;
        const auto event=repo.transition(m); check(event.has_value()); check(!repo.transition(m));
        auto stale=m; stale["detected"]=false; stale["sourceTimeMs"]=m["sourceTimeMs"].get<std::int64_t>()-2;
        check(!repo.transition(stale));
        auto result=repo.search({{"cameraId","CAM01"}}); check(result["events"].size()==1);
        check(repo.search({{"cameraId","CAM01"},{"minConfidence",0.1}})["events"].empty());
        const auto id=event->at("id"); check(!repo.playback({{"eventId",id}})["playable"].get<bool>());
        check(repo.playback({{"eventId",id},{"allowEstimated",true}})["reason"]=="NO_RECORDING_AT_TIME");
        const auto file=root/"segment.mkv"; { std::ofstream f(file); f<<"fixture"; }
        vms::RecordingRepository recordings(path); vms::RecordingEntry entry;
        entry.cameraId="CAM01"; entry.filePath=file.string(); entry.startMs=m["receivedTimeMs"].get<std::int64_t>()-1000;
        entry.endMs=entry.startMs+3000; entry.duration=3; entry.codec="H264";
        entry.hasTimeAnchor=true; entry.firstPts=2; entry.firstDts=0; entry.timeBaseNum=1; entry.timeBaseDen=1000;
        recordings.insert(entry);
        const auto playback=repo.playback({{"eventId",id},{"allowEstimated",true}});
        check(playback["playable"]==true && playback["offsetMs"]==1002 && playback["timeMapping"]=="receive_estimated");
        std::filesystem::remove(file); check(repo.playback({{"eventId",id},{"allowEstimated",true}})["reason"]=="RECORDING_FILE_UNAVAILABLE");
        m["cameraId"]="CAM02"; check(!repo.transition(m)); // 다른 카메라의 baseline은 독립적이다.
        m["cameraId"]="CAM01"; m["sourceTimeMs"]=m["sourceTimeMs"].get<std::int64_t>()+1; m["detected"]=false;
        check(repo.transition(m).has_value());
        m["sourceTimeMs"]=m["sourceTimeMs"].get<std::int64_t>()+1; m["detected"]=true; m["confidence"]=0.9;
        check(repo.transition(m).has_value());
        auto page=repo.search({{"cameraId","CAM01"},{"limit",1}});
        auto next=repo.search({{"cameraId","CAM01"},{"limit",1},{"cursor",page["nextCursor"]}});
        check(page["events"][0]["id"]!=next["events"][0]["id"]);
        check(repo.search({{"cameraId","CAM01"},{"types",Json::array({"PERSON_LOST"})}})["events"].size()==1);
        check(repo.search({{"cameraId","CAM01"},{"minConfidence",0.8}})["events"].size()==1);
        vms::EventRepository reopened(path); check(!reopened.transition(m));
        repo.sample(m); repo.prune(30,7); check(repo.search({{"cameraId","CAM01"}})["events"].size()==3);
        repo.sample(real);
        const auto detections=repo.detections({{"cameraId","CAM01"},{"minConfidence",0.91}});
        check(detections["detections"].size()==1 && detections["detections"][0]["frameId"]=="9007199254740993");
        check(!repo.playback({{"sampleId",detections["detections"][0]["sampleId"]},{"allowEstimated",true}})["playable"].get<bool>());
        // Runtime가 선언되면 property는 baseline만 갱신한다. 같은 변경을 두 번 저장하지 않는다.
        auto runtime=m; runtime["cameraId"]="CAM03"; runtime["transitionSource"]="runtime";
        runtime["detected"]=false; runtime["propertyOperation"]="Initialized";
        check(!repo.transition(runtime)); runtime["detected"]=true; runtime["propertyOperation"]="Changed";
        runtime["sourceTimeMs"]=runtime["sourceTimeMs"].get<std::int64_t>()+1; check(!repo.transition(runtime));
        runtime["topic"]="Runtime/StateEvent"; runtime["propertyOperation"]=""; runtime["eventType"]="PERSON_DETECTED";
        runtime["rawItems"]={{"Type","PERSON_DETECTED"}}; runtime["confidence"]=nullptr;
        check(repo.transition(runtime).has_value()); check(!repo.transition(runtime));
        runtime["eventType"]="PERSON_LOST"; runtime["rawItems"]={{"Type","PERSON_LOST"}};
        // 동일 ms라도 서로 다른 Type은 별개 이벤트다.
        check(repo.transition(runtime).has_value());
        check(repo.search({{"cameraId","CAM03"}})["events"].size()==2);
        check(repo.search({{"cameraId","CAM03"},{"minConfidence",0.1}})["events"].empty());
        std::cout<<"Events UTC/parser/null/transition/duplicates/confidence/playback anchors passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
    // 연결 destructors가 먼저 종료된 후 fixture 폴더를 정리한다.
    std::filesystem::remove_all(root); return 0;
}
