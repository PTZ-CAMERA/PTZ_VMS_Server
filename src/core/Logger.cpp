#include "core/Logger.h"
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
namespace vms {
void log(const std::string& camera, const std::string& module, const std::string& message) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::cout << '[' << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "]["
              << camera << "][" << module << "] " << message << std::endl;
}
}
