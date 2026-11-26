#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rodaq/system/system_service.hpp>

namespace rodaq::system {

struct SystemServiceOptions {
    SystemCapabilities capabilities;
    std::chrono::nanoseconds snapshot_max_age{std::chrono::seconds{2}};
};

struct SystemObservation {
    bool core_ready{false};
    SystemRecoveryStatus recovery;
    std::vector<VolumeStatus> volumes;
    SystemBacklogSnapshot backlog;
};

}  // namespace rodaq::system
