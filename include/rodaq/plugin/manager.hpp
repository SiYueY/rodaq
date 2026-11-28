#pragma once

#include <rodaq/export.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/types.hpp>

namespace rodaq::plugin {

struct ManagerState;
class PluginLibrary;

struct PluginManagerOptions {
    std::vector<std::filesystem::path> search_paths;
    std::size_t max_loaded_plugins{32};
    std::size_t max_instances{64};
    /** Maximum Plugin JSON bytes; must fit uint32_t. Zero permits only absent config. */
    std::size_t max_config_size{64U * 1024U};
    DeadlineDuration shutdown_timeout{std::chrono::seconds{5}};
};

class PluginManager {
public:
    static RODAQ_API Result<PluginManager> create(PluginManagerOptions options);

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    RODAQ_API PluginManager(PluginManager&&) noexcept;
    RODAQ_API PluginManager& operator=(PluginManager&&) noexcept;
    RODAQ_API ~PluginManager() noexcept;

    RODAQ_API Result<std::vector<PluginInfo>> plugins() const;
    RODAQ_API Result<PluginLibrary> load(std::string_view plugin_id);
    RODAQ_API Result<PluginLibrary> load(
        std::string_view plugin_id, std::string_view expected_version);
    [[nodiscard]] RODAQ_API PluginManagerPhase phase() const noexcept;
    [[nodiscard]] RODAQ_API bool restart_required() const noexcept;
    RODAQ_API Result<void> shutdown(Deadline deadline);

private:
    explicit PluginManager(std::shared_ptr<ManagerState> state) noexcept;

    std::shared_ptr<ManagerState> state_;
};

}  // namespace rodaq::plugin
