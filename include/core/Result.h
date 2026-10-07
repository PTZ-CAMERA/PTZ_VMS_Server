#pragma once
#include <string>
namespace vms {
// Future adapters report failures; an unimplemented adapter must never report success.
struct Result { bool ok = false; std::string error; };
}
