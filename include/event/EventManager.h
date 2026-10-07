#pragma once
#include "core/Result.h"
#include "event/DetectionEvent.h"
#include <vector>
namespace vms {
class EventManager {
public:
    virtual ~EventManager() = default;
    virtual Result receive(const DetectionEvent&) = 0; // M4: validate, persist, then publish.
    virtual std::vector<DetectionEvent> getEvents(const std::string& cameraId, UtcTime from, UtcTime to, unsigned limit) const = 0;
};
}
