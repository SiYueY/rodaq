#pragma once

#include "artifact.hpp"
#include "manifest.hpp"

#include <rodaq/plugin/c/abi.h>
#include <rodaq/plugin/manager.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rodaq::plugin {

enum class PluginLibraryPhase { Loaded, Faulted };

class PluginLibraryState final {
public:
    PluginLibraryState(
        void* native_handle, Manifest manifest, ArtifactDigest artifact,
        const rodaq_plugin_entry_t* entry);

    PluginLibraryState(const PluginLibraryState&) = delete;
    PluginLibraryState& operator=(const PluginLibraryState&) = delete;
    ~PluginLibraryState();

    [[nodiscard]] void* native_handle() const noexcept { return native_handle_; }
    [[nodiscard]] const PluginInfo& info() const noexcept { return info_; }
    [[nodiscard]] const PluginArtifactInfo& artifact_info() const noexcept {
        return artifact_info_;
    }
    [[nodiscard]] const Manifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] const ArtifactDigest& artifact() const noexcept { return artifact_; }
    [[nodiscard]] const rodaq_plugin_entry_t* entry() const noexcept { return entry_; }
    [[nodiscard]] PluginLibraryPhase phase() const noexcept {
        return phase_.load(std::memory_order_acquire);
    }
    void mark_faulted() noexcept { phase_.store(PluginLibraryPhase::Faulted); }

private:
    void* native_handle_;
    Manifest manifest_;
    ArtifactDigest artifact_;
    const rodaq_plugin_entry_t* entry_;
    PluginInfo info_;
    PluginArtifactInfo artifact_info_;
    std::atomic<PluginLibraryPhase> phase_{PluginLibraryPhase::Loaded};
};

}  // namespace rodaq::plugin
