#include "event/EventRepository.h"
#include "core/UtcTimestamp.h"
#include <sqlite3.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
namespace vms {
namespace {
using Json = nlohmann::json;
using Statement = std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>;
Statement prepare(sqlite3* db,const std::string& sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db,sql.c_str(),-1,&raw,nullptr) != SQLITE_OK) throw std::runtime_error("Events database statement failed");
    return Statement(raw,&sqlite3_finalize);
}
void execute(sqlite3* db,const char* sql) {
    if (sqlite3_exec(db,sql,nullptr,nullptr,nullptr) != SQLITE_OK) throw std::runtime_error("Events database operation failed");
}
void bindText(sqlite3_stmt* s,int index,const std::string& text) {
    if (sqlite3_bind_text(s,index,text.c_str(),-1,SQLITE_TRANSIENT) != SQLITE_OK) throw std::runtime_error("Events database bind failed");
}
std::string str(sqlite3_stmt* s,int index) { const auto* p = sqlite3_column_text(s,index); return p ? reinterpret_cast<const char*>(p) : ""; }
void done(sqlite3_stmt* s) { if (sqlite3_step(s) != SQLITE_DONE) throw std::runtime_error("Events database write failed"); }
Json row(sqlite3_stmt* s) {
    auto result = Json::parse(str(s,7)); result["id"] = sqlite3_column_int64(s,0);
    result["cameraId"] = str(s,1); result["type"] = str(s,2); result["searchTimeMs"] = sqlite3_column_int64(s,5);
    return result;
}
std::int64_t integer(const Json& j,const char* key,std::int64_t fallback) {
    if (!j.contains(key)) return fallback;
    if (!j[key].is_number_integer()) throw std::runtime_error("Expected integer timestamp/id");
    return j[key].get<std::int64_t>();
}
}
EventRepository::EventRepository(const std::string& path) {
    const auto folder=std::filesystem::u8path(path).parent_path();
    if (!folder.empty()) std::filesystem::create_directories(folder);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(),&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_NOMUTEX,nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        throw std::runtime_error("Cannot open Events database");
    }
    db_ = db;
    try {
        sqlite3_busy_timeout(db,3000); execute(db,"PRAGMA journal_mode=WAL;");
        // IF NOT EXISTS로 recorder와 동일한 recordings schema를 준비하며 기존 데이터를 보존한다.
        execute(db,"CREATE TABLE IF NOT EXISTS recordings(id INTEGER PRIMARY KEY AUTOINCREMENT,camera_id TEXT NOT NULL,start_time INTEGER NOT NULL,end_time INTEGER NOT NULL,file_path TEXT NOT NULL UNIQUE,duration REAL NOT NULL,codec TEXT NOT NULL,width INTEGER NOT NULL,height INTEGER NOT NULL);");
        execute(db,"CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY AUTOINCREMENT,camera_id TEXT NOT NULL,type TEXT NOT NULL,source_time_ms INTEGER,received_time_ms INTEGER NOT NULL,search_time_ms INTEGER NOT NULL,confidence REAL,payload_json TEXT NOT NULL,dedup_key TEXT NOT NULL UNIQUE);");
        execute(db,"CREATE INDEX IF NOT EXISTS event_camera_time ON events(camera_id,search_time_ms,id);");
        execute(db,"CREATE TABLE IF NOT EXISTS event_states(state_key TEXT PRIMARY KEY,value INTEGER NOT NULL,source_ms INTEGER);");
        execute(db,"CREATE TABLE IF NOT EXISTS metadata_samples(id INTEGER PRIMARY KEY AUTOINCREMENT,camera_id TEXT NOT NULL,source_time_ms INTEGER,received_time_ms INTEGER NOT NULL,payload_json TEXT NOT NULL);");
        execute(db,"CREATE INDEX IF NOT EXISTS metadata_camera_time ON metadata_samples(camera_id,received_time_ms);");
        execute(db,"CREATE TABLE IF NOT EXISTS detection_index(sample_id INTEGER PRIMARY KEY,camera_id TEXT NOT NULL,search_time_ms INTEGER NOT NULL,confidence REAL);");
        execute(db,"CREATE INDEX IF NOT EXISTS detection_camera_time ON detection_index(camera_id,search_time_ms,sample_id);");
        execute(db,"CREATE TABLE IF NOT EXISTS recording_time_anchors(recording_id INTEGER PRIMARY KEY,arrival_utc_ms INTEGER NOT NULL,first_pts INTEGER NOT NULL,first_dts INTEGER NOT NULL,time_base_num INTEGER NOT NULL,time_base_den INTEGER NOT NULL);");
    } catch (...) { sqlite3_close(db); db_ = nullptr; throw; }
}
EventRepository::~EventRepository() { if (db_) sqlite3_close(static_cast<sqlite3*>(db_)); }
std::optional<nlohmann::json> EventRepository::transition(const Json& m) {
    if (m.at("topic")=="Runtime/StateEvent") {
        const auto type=m.value("eventType",Json(nullptr));
        if (type!="PERSON_DETECTED" && type!="PERSON_LOST" && type!="TRACKING_STARTED" && type!="TRACKING_STOPPED") return {};
        // Type만 있는 실제 상태 변경이다. 인접 bbox/confidence를 임의로 같은 이벤트에 붙이지 않는다.
        Json e=m; e["type"]=type; e["recordKind"]="state_event";
        const auto source=m.at("sourceTimeMs"); const auto received=m.at("receivedTimeMs").get<std::int64_t>();
        const auto time=source.is_null() ? received : source.get<std::int64_t>();
        auto* db=static_cast<sqlite3*>(db_);
        auto s=prepare(db,"INSERT OR IGNORE INTO events(camera_id,type,source_time_ms,received_time_ms,search_time_ms,confidence,payload_json,dedup_key) VALUES(?,?,?,?,?,NULL,?,?);");
        bindText(s.get(),1,m.at("cameraId").get<std::string>()); bindText(s.get(),2,type.get<std::string>());
        if (source.is_null()) sqlite3_bind_null(s.get(),3); else sqlite3_bind_int64(s.get(),3,source.get<std::int64_t>());
        sqlite3_bind_int64(s.get(),4,received); sqlite3_bind_int64(s.get(),5,time);
        bindText(s.get(),6,e.dump());
        bindText(s.get(),7,Json::array({m.at("cameraId"),m.at("profileToken"),m.at("topic"),source,type,m.at("rawItems")}).dump());
        done(s.get()); if (!sqlite3_changes(db)) return {};
        e["id"]=sqlite3_last_insert_rowid(db); e["searchTimeMs"]=time; return e;
    }
    const bool person = m.at("topic") == "Analytics/PersonDetection", tracking = m.at("topic") == "Tracking/State";
    if ((!person && !tracking) || m.at("available") == false) return {};
    const auto state = m.at(person ? "detected" : "tracking"); if (!state.is_boolean()) return {};
    const auto source = m.at("sourceTimeMs"); const auto received = m.at("receivedTimeMs").get<std::int64_t>();
    const auto key = Json::array({m.at("cameraId"),m.at("topic"),m.at("profileToken")}).dump();
    auto* db = static_cast<sqlite3*>(db_); execute(db,"BEGIN IMMEDIATE;");
    try {
        auto query = prepare(db,"SELECT value,source_ms FROM event_states WHERE state_key=?;"); bindText(query.get(),1,key);
        const int result = sqlite3_step(query.get()); if (result != SQLITE_ROW && result != SQLITE_DONE) throw std::runtime_error("State query failed");
        const bool known = result == SQLITE_ROW;
        const bool previous = known && sqlite3_column_int(query.get(),0);
        if (known && !source.is_null() && sqlite3_column_type(query.get(),1) != SQLITE_NULL
            && source.get<std::int64_t>() < sqlite3_column_int64(query.get(),1)) { execute(db,"COMMIT;"); return {}; }
        // 최초/재구독 Initialized는 상태 기준이다. 등장 이벤트를 임의로 만들지 않는다.
        const bool changed = known && previous != state.get<bool>() && m.at("propertyOperation") == "Changed"
            && m.value("transitionSource",std::string{"property_fallback"})!="runtime";
        auto update = prepare(db,"INSERT INTO event_states(state_key,value,source_ms) VALUES(?,?,?) ON CONFLICT(state_key) DO UPDATE SET value=excluded.value,source_ms=COALESCE(excluded.source_ms,event_states.source_ms);");
        bindText(update.get(),1,key); sqlite3_bind_int(update.get(),2,state.get<bool>());
        if (!source.is_null()) sqlite3_bind_int64(update.get(),3,source.get<std::int64_t>()); else sqlite3_bind_null(update.get(),3);
        done(update.get()); std::optional<Json> event;
        if (changed) {
            Json e = m; e["recordKind"]="state_event";
            e["type"] = person ? (state.get<bool>() ? "PERSON_DETECTED" : "PERSON_LOST") : (state.get<bool>() ? "TRACKING_STARTED" : "TRACKING_STOPPED");
            const auto time = source.is_null() ? received : source.get<std::int64_t>();
            // 원본 시각이 없는 동일 payload는 보수적으로 중복 제거한다. 다른 발생인지 확정할 수 없다.
            const auto dedup = Json::array({key,source,e.at("type"),m.at("rawItems")}).dump();
            auto insert = prepare(db,"INSERT OR IGNORE INTO events(camera_id,type,source_time_ms,received_time_ms,search_time_ms,confidence,payload_json,dedup_key) VALUES(?,?,?,?,?,?,?,?);");
            bindText(insert.get(),1,m.at("cameraId").get<std::string>()); bindText(insert.get(),2,e.at("type").get<std::string>());
            if (!source.is_null()) sqlite3_bind_int64(insert.get(),3,source.get<std::int64_t>()); else sqlite3_bind_null(insert.get(),3);
            sqlite3_bind_int64(insert.get(),4,received); sqlite3_bind_int64(insert.get(),5,time);
            if (m.at("confidence").is_number()) sqlite3_bind_double(insert.get(),6,m.at("confidence").get<double>()); else sqlite3_bind_null(insert.get(),6);
            bindText(insert.get(),7,e.dump()); bindText(insert.get(),8,dedup); done(insert.get());
            if (sqlite3_changes(db)) { e["id"] = sqlite3_last_insert_rowid(db); e["searchTimeMs"] = time; event = e; }
        }
        execute(db,"COMMIT;"); return event;
    } catch (...) { sqlite3_exec(db,"ROLLBACK;",nullptr,nullptr,nullptr); throw; }
}
void EventRepository::sample(const Json& m) {
    auto* db=static_cast<sqlite3*>(db_); execute(db,"BEGIN IMMEDIATE;");
    try {
    auto statement = prepare(db,"INSERT INTO metadata_samples(camera_id,source_time_ms,received_time_ms,payload_json) VALUES(?,?,?,?);");
    bindText(statement.get(),1,m.at("cameraId").get<std::string>());
    if (m.at("sourceTimeMs").is_null()) sqlite3_bind_null(statement.get(),2); else sqlite3_bind_int64(statement.get(),2,m.at("sourceTimeMs").get<std::int64_t>());
    sqlite3_bind_int64(statement.get(),3,m.at("receivedTimeMs").get<std::int64_t>()); bindText(statement.get(),4,m.dump()); done(statement.get());
    if (m.at("topic")=="Analytics/PersonDetection" && m.at("detected")==true && m.at("available")!=false) {
        const auto id=sqlite3_last_insert_rowid(db);
        auto index=prepare(db,"INSERT INTO detection_index VALUES(?,?,?,?);");
        sqlite3_bind_int64(index.get(),1,id); bindText(index.get(),2,m.at("cameraId").get<std::string>());
        // 검색은 제공된 capture UTC를 우선한다. 센서 노출 시각으로 승격하지 않는다.
        const auto capture=m.value("captureTimeMs",Json(nullptr)), analysis=m.value("analysisTimeMs",Json(nullptr));
        const auto time=capture.is_number_integer() ? capture.get<std::int64_t>() : analysis.is_number_integer() ? analysis.get<std::int64_t>()
            : m.at("sourceTimeMs").is_number_integer() ? m.at("sourceTimeMs").get<std::int64_t>() : m.at("receivedTimeMs").get<std::int64_t>();
        sqlite3_bind_int64(index.get(),3,time);
        if (m.at("confidence").is_number()) sqlite3_bind_double(index.get(),4,m.at("confidence").get<double>()); else sqlite3_bind_null(index.get(),4);
        done(index.get());
    }
    execute(db,"COMMIT;");
    } catch (...) { sqlite3_exec(db,"ROLLBACK;",nullptr,nullptr,nullptr); throw; }
}
void EventRepository::prune(unsigned events,unsigned samples) {
    auto* db = static_cast<sqlite3*>(db_);
    for (const auto& item : std::vector<std::pair<std::string,unsigned>>{{"events",events},{"metadata_samples",samples}}) {
        auto s = prepare(db,"DELETE FROM " + item.first + " WHERE received_time_ms<?;");
        sqlite3_bind_int64(s.get(),1,utcNowMs()-static_cast<std::int64_t>(item.second)*86400000); done(s.get());
    }
    execute(db,"DELETE FROM detection_index WHERE sample_id NOT IN (SELECT id FROM metadata_samples);");
}
nlohmann::json EventRepository::search(const Json& request) {
    return searchRecords(request,false);
}
nlohmann::json EventRepository::detections(const Json& request) {
    return searchRecords(request,true);
}
nlohmann::json EventRepository::searchRecords(const Json& request,bool observations) {
    const auto camera = request.value("cameraId",std::string{}); if (camera.empty() || camera.size()>128) throw std::runtime_error("cameraId required");
    const auto from = integer(request,"fromMs",0), to = integer(request,"toMs",std::numeric_limits<std::int64_t>::max());
    const auto limit = integer(request,"limit",50); if (from>=to || limit<1 || limit>100) throw std::runtime_error("Invalid event query range/limit");
    std::vector<std::string> types;
    if (request.contains("types")) {
        if (!request["types"].is_array() || request["types"].size()>4) throw std::runtime_error("Invalid event types");
        for (const auto& t : request["types"]) {
            if (!t.is_string() || (observations ? t!="DETECTION_SAMPLE" : (t!="PERSON_DETECTED" && t!="PERSON_LOST" && t!="TRACKING_STARTED" && t!="TRACKING_STOPPED"))) throw std::runtime_error("Unknown record type");
            types.push_back(t.get<std::string>());
        }
    }
    const bool confidence = request.contains("minConfidence");
    double minimum = 0; if (confidence) {
        if (!request["minConfidence"].is_number()) throw std::runtime_error("Invalid minConfidence");
        minimum=request["minConfidence"].get<double>();
        if (!(minimum>=0 && minimum<=1)) throw std::runtime_error("Invalid minConfidence");
    }
    std::int64_t cursorTime=std::numeric_limits<std::int64_t>::max(), cursorId=std::numeric_limits<std::int64_t>::max();
    if (request.contains("cursor") && !request["cursor"].is_null()) {
        if (!request["cursor"].is_object()) throw std::runtime_error("Invalid cursor");
        cursorTime=integer(request["cursor"],"timeMs",-1); cursorId=integer(request["cursor"],"id",-1);
        if (cursorTime<0 || cursorId<1) throw std::runtime_error("Invalid cursor");
    }
    std::string sql="SELECT id,camera_id,type,source_time_ms,received_time_ms,search_time_ms,confidence,payload_json FROM events WHERE camera_id=? AND search_time_ms>=? AND search_time_ms<? AND (search_time_ms<? OR (search_time_ms=? AND id<?))";
    if (observations) sql="SELECT d.sample_id,d.camera_id,'DETECTION_SAMPLE',m.source_time_ms,m.received_time_ms,d.search_time_ms,d.confidence,m.payload_json FROM detection_index d JOIN metadata_samples m ON m.id=d.sample_id WHERE d.camera_id=? AND d.search_time_ms>=? AND d.search_time_ms<? AND (d.search_time_ms<? OR (d.search_time_ms=? AND d.sample_id<?))";
    if (confidence) sql+=" AND confidence>=?";
    if (!types.empty()) { sql+=" AND type IN ("; for (std::size_t i=0;i<types.size();++i) sql+=(i ? ",?" : "?"); sql+=")"; }
    sql+=observations ? " ORDER BY d.search_time_ms DESC,d.sample_id DESC LIMIT ?;" : " ORDER BY search_time_ms DESC,id DESC LIMIT ?;";
    auto s=prepare(static_cast<sqlite3*>(db_),sql); bindText(s.get(),1,camera);
    sqlite3_bind_int64(s.get(),2,from); sqlite3_bind_int64(s.get(),3,to); sqlite3_bind_int64(s.get(),4,cursorTime);
    sqlite3_bind_int64(s.get(),5,cursorTime); sqlite3_bind_int64(s.get(),6,cursorId); int index=7;
    if (confidence) sqlite3_bind_double(s.get(),index++,minimum);
    for (const auto& t : types) bindText(s.get(),index++,t);
    sqlite3_bind_int64(s.get(),index,limit+1);
    auto list=Json::array(); int code;
    while ((code=sqlite3_step(s.get()))==SQLITE_ROW) {
        auto record=row(s.get());
        if (observations) { record["sampleId"]=record["id"]; record["recordKind"]="detection_sample"; }
        list.push_back(std::move(record));
    }
    if (code!=SQLITE_DONE) throw std::runtime_error("Event query failed");
    Json cursor=nullptr; bool more=list.size()>static_cast<std::size_t>(limit);
    if (more) list.erase(list.end()-1);
    // WS 최대 64KiB보다 작게 잘라서 큰 raw metadata 검색도 연결을 끊지 않는다.
    while (list.size()>1 && list.dump().size()>48000) { list.erase(list.end()-1); more=true; }
    if (more && !list.empty()) cursor={{"timeMs",list.back()["searchTimeMs"]},{"id",list.back()["id"]}};
    return {{observations ? "detections" : "events",list},{"nextCursor",cursor}};
}
nlohmann::json EventRepository::playback(const Json& request) {
    const bool sample=request.contains("sampleId");
    if (sample && request.contains("eventId")) throw std::runtime_error("Use eventId or sampleId, not both");
    const auto eventId=integer(request,sample ? "sampleId" : "eventId",0); if (eventId<1) throw std::runtime_error("eventId/sampleId required");
    auto* db=static_cast<sqlite3*>(db_);
    auto event=prepare(db,sample ? "SELECT d.sample_id,d.camera_id,'DETECTION_SAMPLE',m.source_time_ms,m.received_time_ms,d.search_time_ms,d.confidence,m.payload_json FROM detection_index d JOIN metadata_samples m ON m.id=d.sample_id WHERE d.sample_id=?;"
        : "SELECT id,camera_id,type,source_time_ms,received_time_ms,search_time_ms,confidence,payload_json FROM events WHERE id=?;");
    sqlite3_bind_int64(event.get(),1,eventId); const int code=sqlite3_step(event.get());
    if (code==SQLITE_DONE) return {{sample ? "sampleId" : "eventId",eventId},{"playable",false},{"reason",sample ? "DETECTION_NOT_FOUND" : "EVENT_NOT_FOUND"}};
    if (code!=SQLITE_ROW) throw std::runtime_error("Event lookup failed");
    auto e=row(event.get()); const auto camera=e["cameraId"].get<std::string>();
    Json result={{"eventId",eventId},{"cameraId",camera},{"playable",false},{"offsetMs",nullptr},{"recording",nullptr},{"timeMapping","receive_estimated"}};
    if (sample) { result.erase("eventId"); result["sampleId"]=eventId; }
    result["captureTimeMs"]=e.value("captureTimeMs",Json(nullptr));
    result["analysisTimeMs"]=e.value("analysisTimeMs",Json(nullptr));
    if (request.contains("allowEstimated") && !request["allowEstimated"].is_boolean()) throw std::runtime_error("allowEstimated must be boolean");
    if (!request.value("allowEstimated",false)) { result["reason"]="PRECISE_TIME_MAPPING_UNAVAILABLE"; return result; }
    // 원본 이벤트 시각과 수신 시각을 혼동하지 않는다. 현재는 수신 시각 기반 추정 탐색만 제공한다.
    const auto time=e["receivedTimeMs"].get<std::int64_t>();
    result["lookupTimeMs"]=time; result["sourceTimeMs"]=e["sourceTimeMs"];
    auto r=prepare(db,"SELECT id,start_time,end_time,file_path,duration,codec,width,height FROM recordings WHERE camera_id=? AND start_time<=? AND end_time>? ORDER BY start_time DESC LIMIT 1;");
    bindText(r.get(),1,camera); sqlite3_bind_int64(r.get(),2,time); sqlite3_bind_int64(r.get(),3,time);
    const int recording=sqlite3_step(r.get());
    if (recording==SQLITE_DONE) { result["reason"]="NO_RECORDING_AT_TIME"; return result; }
    if (recording!=SQLITE_ROW) throw std::runtime_error("Recording lookup failed");
    const auto file=str(r.get(),3); std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(file),error) || (file.size()>=5 && file.substr(file.size()-5)==".part")) {
        result["reason"]="RECORDING_FILE_UNAVAILABLE"; return result;
    }
    const auto recordingId=sqlite3_column_int64(r.get(),0), start=sqlite3_column_int64(r.get(),1);
    result["recording"]={{"id",recordingId},{"cameraId",camera},{"startTimeMs",start},{"endTimeMs",sqlite3_column_int64(r.get(),2)},
        {"filePath",file},{"duration",sqlite3_column_double(r.get(),4)},{"codec",str(r.get(),5)},{"width",sqlite3_column_int(r.get(),6)},{"height",sqlite3_column_int(r.get(),7)}};
    result["offsetMs"]=time-start; result["anchorKind"]="legacy_receive";
    auto anchor=prepare(db,"SELECT arrival_utc_ms,first_pts,first_dts,time_base_num,time_base_den FROM recording_time_anchors WHERE recording_id=?;");
    sqlite3_bind_int64(anchor.get(),1,recordingId);
    if (sqlite3_step(anchor.get())==SQLITE_ROW) {
        result["anchorKind"]="receive_pts";
        result["timeAnchor"]={{"arrivalUtcMs",sqlite3_column_int64(anchor.get(),0)},{"firstPts",sqlite3_column_int64(anchor.get(),1)},
            {"firstDts",sqlite3_column_int64(anchor.get(),2)},{"timeBaseNum",sqlite3_column_int(anchor.get(),3)},{"timeBaseDen",sqlite3_column_int(anchor.get(),4)}};
        const auto num=sqlite3_column_int(anchor.get(),3), den=sqlite3_column_int(anchor.get(),4);
        if (den>0) result["offsetMs"]=time-start+static_cast<std::int64_t>((static_cast<double>(sqlite3_column_int64(anchor.get(),1))-static_cast<double>(sqlite3_column_int64(anchor.get(),2)))*num/den*1000);
    }
    const auto offset=result["offsetMs"].get<std::int64_t>();
    if (offset<0 || offset>=sqlite3_column_double(r.get(),4)*1000) { result["reason"]="OFFSET_OUT_OF_RANGE"; return result; }
    result["playable"]=true; result["reason"]="ESTIMATED_RECEIVE_TIME"; return result;
}
}
