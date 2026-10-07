#include "database/RecordingRepository.h"
#include <sqlite3.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
namespace vms {
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement prepare(::sqlite3* db, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) throw std::runtime_error("Recording database statement failed");
    return Statement(raw, &sqlite3_finalize);
}
void sql(::sqlite3* db, const char* query) {
    if (sqlite3_exec(db, query, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error("Recording database operation failed");
}
void text(sqlite3_stmt* s, int index, const std::string& value) {
    if (sqlite3_bind_text(s, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) throw std::runtime_error("Recording database bind failed");
}
std::string text(sqlite3_stmt* s, int index) {
    const auto* p = sqlite3_column_text(s, index); return p ? reinterpret_cast<const char*>(p) : "";
}
}
RecordingRepository::RecordingRepository(const std::string& path) {
    const auto folder = std::filesystem::u8path(path).parent_path(); if (!folder.empty()) std::filesystem::create_directories(folder);
    ::sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        throw std::runtime_error("Cannot open recording database");
    }
    db_ = db;
    try {
        sqlite3_busy_timeout(db, 3000);
        sql(db, "PRAGMA journal_mode=WAL;");
        sql(db, "CREATE TABLE IF NOT EXISTS recordings(id INTEGER PRIMARY KEY AUTOINCREMENT,camera_id TEXT NOT NULL,start_time INTEGER NOT NULL,end_time INTEGER NOT NULL,file_path TEXT NOT NULL UNIQUE,duration REAL NOT NULL,codec TEXT NOT NULL,width INTEGER NOT NULL,height INTEGER NOT NULL);");
        sql(db, "CREATE INDEX IF NOT EXISTS recording_camera_time ON recordings(camera_id,start_time,end_time);");
    } catch (...) { sqlite3_close(db); db_ = nullptr; throw; }
}
RecordingRepository::~RecordingRepository() { if (db_) sqlite3_close(static_cast<::sqlite3*>(db_)); }
void RecordingRepository::insert(const RecordingEntry& e) {
    auto statement = prepare(static_cast<::sqlite3*>(db_), "INSERT INTO recordings(camera_id,start_time,end_time,file_path,duration,codec,width,height) VALUES(?,?,?,?,?,?,?,?);");
    text(statement.get(), 1, e.cameraId); sqlite3_bind_int64(statement.get(), 2, e.startMs); sqlite3_bind_int64(statement.get(), 3, e.endMs);
    text(statement.get(), 4, e.filePath); sqlite3_bind_double(statement.get(), 5, e.duration); text(statement.get(), 6, e.codec);
    sqlite3_bind_int(statement.get(), 7, e.width); sqlite3_bind_int(statement.get(), 8, e.height);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) throw std::runtime_error("Recording metadata could not be saved");
}
std::vector<RecordingEntry> RecordingRepository::list(const std::string& id, std::int64_t from, std::int64_t to, unsigned limit) {
    auto statement = prepare(static_cast<::sqlite3*>(db_), "SELECT id,camera_id,start_time,end_time,file_path,duration,codec,width,height FROM recordings WHERE camera_id=? AND end_time>? AND start_time<? ORDER BY start_time DESC,id DESC LIMIT ?;");
    text(statement.get(), 1, id); sqlite3_bind_int64(statement.get(), 2, from); sqlite3_bind_int64(statement.get(), 3, to); sqlite3_bind_int(statement.get(), 4, static_cast<int>(limit));
    std::vector<RecordingEntry> entries; int result;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        RecordingEntry e; e.id = sqlite3_column_int64(statement.get(), 0); e.cameraId = text(statement.get(), 1);
        e.startMs = sqlite3_column_int64(statement.get(), 2); e.endMs = sqlite3_column_int64(statement.get(), 3); e.filePath = text(statement.get(), 4);
        e.duration = sqlite3_column_double(statement.get(), 5); e.codec = text(statement.get(), 6); e.width = sqlite3_column_int(statement.get(), 7); e.height = sqlite3_column_int(statement.get(), 8);
        entries.push_back(std::move(e));
    }
    if (result != SQLITE_DONE) throw std::runtime_error("Recording query failed");
    return entries;
}
}
