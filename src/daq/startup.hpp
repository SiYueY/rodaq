#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "../persistence/file_descriptor.hpp"
#include "composition.hpp"

namespace rodaq::daq {

enum class DeploymentOpenMode { Initialize, Existing };

struct StartupOptions {
    std::filesystem::path state_root;
    std::filesystem::path volume_root;
    DeploymentOpenMode mode{DeploymentOpenMode::Existing};
    plugin::PluginManagerOptions plugins;
    artifact::VolumeCapacityOptions capacity;
    DeploymentSettings settings;
};

// Pins the state-root namespace and its stable lock through actual Core
// shutdown. Member descriptors close only after all composed owners disappear.
struct DeploymentLease {
    persistence::FileDescriptor directory;
    persistence::FileDescriptor lock;
    std::filesystem::path root;
    std::uint64_t device{0};
    std::uint64_t inode{0};
    std::uint64_t lock_inode{0};

    DeploymentLease(persistence::FileDescriptor directory, persistence::FileDescriptor lock);
    Result<void> verify() const;
};

}  // namespace rodaq::daq
