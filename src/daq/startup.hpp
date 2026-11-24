#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "composition.hpp"
#include "../persistence/file_descriptor.hpp"

namespace rodaq::daq {

// Exclusive boot work before Catalog/capacity publication. Task/Reset authority
// is loaded for each actual publication boundary; unresolved deletes run first.
Result<void> recover_pending_commits(const CompositionOptions& options);

enum class DeploymentOpenMode { Initialize, Existing };

struct StartupOptions {
    // Explicit, existing absolute directories. Initialize requires both roots
    // empty; Existing requires the durable bootstrap marker and all namespaces.
    std::filesystem::path state_root;
    std::filesystem::path volume_root;
    DeploymentOpenMode mode{DeploymentOpenMode::Existing};
    plugin::PluginManagerOptions plugins;
    artifact::VolumeCapacityOptions capacity;
    std::uint32_t ledger_records{4096};
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
