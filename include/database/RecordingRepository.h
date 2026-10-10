#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace vms {
struct RecordingEntry {
    std::int64_t id = 0, startMs = 0, endMs = 0;
    std::string cameraId, filePath, codec;
    double duration = 0;
    int width = 0, height = 0;
    bool hasTimeAnchor = false;
    std::int64_t firstPts = 0, firstDts = 0;
    int timeBaseNum = 0, timeBaseDen = 1;
};
struct sqlite3; // implementation keeps SQLite types private
class RecordingRepository {
public:
    explicit RecordingRepository(const std::string& path);
    ~RecordingRepository();
    RecordingRepository(const RecordingRepository&) = delete;
    RecordingRepository& operator=(const RecordingRepository&) = delete;
    void insert(const RecordingEntry&);
    std::vector<RecordingEntry> list(const std::string& cameraId, std::int64_t fromMs, std::int64_t toMs, unsigned limit);
private:
    void* db_ = nullptr; // Dedicated recorder worker owns connection and statements.
};
}
