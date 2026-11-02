#ifndef RODAQ_PLUGIN_PLUGIN_LIBRARY_HPP_
#define RODAQ_PLUGIN_PLUGIN_LIBRARY_HPP_

#include "plugin/plugin_types.hpp"

#include <rodaq/plugin/abi/acquisition.h>
#include <rodaq/plugin/abi/plugin.h>
#include <rodaq/plugin/abi/storage.h>
#include <rodaq/result.hpp>

#include <filesystem>
#include <memory>

namespace rodaq::plugin {

class PluginLibraryState;

class PluginLibraryLease;

class PluginLibrary final {
public:
    static Result<PluginLibrary> open(const std::filesystem::path& path);

    PluginLibrary(PluginLibrary&&) noexcept = default;
    PluginLibrary& operator=(PluginLibrary&&) noexcept = default;

    PluginLibrary(const PluginLibrary&) = delete;
    PluginLibrary& operator=(const PluginLibrary&) = delete;

    ~PluginLibrary() = default;

    PluginLibraryLease lease() const noexcept;
    bool valid() const noexcept;
    PluginFamily family() const noexcept;
    const PluginMetadata& metadata() const noexcept;
    const std::filesystem::path& canonical_path() const noexcept;

private:
    explicit PluginLibrary(std::shared_ptr<PluginLibraryState> state) noexcept;

    std::shared_ptr<PluginLibraryState> state_;

    friend class PluginLibraryLease;
};

class PluginLibraryLease final {
public:
    PluginLibraryLease() = default;
    PluginLibraryLease(const PluginLibraryLease&) = default;
    PluginLibraryLease(PluginLibraryLease&&) noexcept = default;
    PluginLibraryLease& operator=(const PluginLibraryLease&) = default;
    PluginLibraryLease& operator=(PluginLibraryLease&&) noexcept = default;
    ~PluginLibraryLease() = default;

    bool valid() const noexcept;
    PluginFamily family() const noexcept;
    const PluginMetadata& metadata() const noexcept;
    const std::filesystem::path& canonical_path() const noexcept;

    const rodaq_plugin_descriptor* raw_descriptor() const noexcept;
    const rodaq_acquisition_plugin* acquisition_protocol() const noexcept;
    const rodaq_storage_plugin* storage_protocol() const noexcept;

    void quarantine() noexcept;
    bool quarantined() const noexcept;

private:
    explicit PluginLibraryLease(std::shared_ptr<PluginLibraryState> state) noexcept;

    std::shared_ptr<PluginLibraryState> state_;

    friend class PluginLibrary;
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_PLUGIN_LIBRARY_HPP_
