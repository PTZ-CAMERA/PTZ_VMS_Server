#include "core/UtcTimestamp.h"
#include <chrono>
#include <regex>
#include <ctime>
#include <iomanip>
#include <sstream>
namespace vms {
std::int64_t utcNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string formatUtcMs(std::int64_t ms,int offset) {
    const auto seconds=static_cast<std::time_t>(ms/1000+offset*60); std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm,&seconds);
#else
    gmtime_r(&seconds,&tm);
#endif
    std::ostringstream out; out << std::put_time(&tm,"%Y-%m-%dT%H:%M:%S");
    if (!offset) out << 'Z';
    else out << (offset<0 ? '-' : '+') << std::setfill('0') << std::setw(2) << std::abs(offset)/60 << ':' << std::setw(2) << std::abs(offset)%60;
    return out.str();
}
std::optional<std::int64_t> parseUtcMs(const std::string& text) {
    // 시간대 없는 문자열을 PC 로컬 시간으로 추측하지 않는다. UTC 또는 명시적 offset만 허용한다.
    static const std::regex format(R"(^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,9}))?(Z|[+-]\d{2}:\d{2})$)");
    std::smatch m; if (!std::regex_match(text, m, format)) return {};
    int y = std::stoi(m[1]), month = std::stoi(m[2]), day = std::stoi(m[3]);
    const int hour = std::stoi(m[4]), minute = std::stoi(m[5]), second = std::stoi(m[6]);
    const bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    const int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1970 || month < 1 || month > 12 || day < 1 || day > lengths[month-1] + (month == 2 && leap)
        || hour > 23 || minute > 59 || second > 59) return {};
    // Gregorian civil date를 epoch day로 변환하여 Windows/Linux의 timegm 차이를 피한다.
    y -= month <= 2; const int era = y / 400, year = y - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const std::int64_t days = era * 146097LL + year * 365 + year / 4 - year / 100 + doy - 719468;
    std::string fraction = m[7]; fraction += "000"; const int ms = std::stoi(fraction.substr(0,3));
    const std::string zone = m[8]; int offset = 0;
    if (zone != "Z") {
        const int h = std::stoi(zone.substr(1,2)), min = std::stoi(zone.substr(4,2));
        if (h > 23 || min > 59) return {};
        offset = (h * 60 + min) * (zone[0] == '-' ? -1 : 1);
    }
    return ((days * 24 + hour) * 3600 + minute * 60 + second - offset * 60) * 1000 + ms;
}
}
