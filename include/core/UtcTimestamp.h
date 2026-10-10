#pragma once
#include <cstdint>
#include <optional>
#include <string>
namespace vms {
std::optional<std::int64_t> parseUtcMs(const std::string&);
std::int64_t utcNowMs();
std::string formatUtcMs(std::int64_t milliseconds,int offsetMinutes=0);
}
