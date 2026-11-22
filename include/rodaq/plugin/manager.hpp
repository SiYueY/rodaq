#pragma once

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
    std::size_t max_loaded_plugins{0};
    std::size_t max_instances{0};
    /** Maximum Plugin JSON bytes; must fit uint32_t. Zero permits only absent config. */
    std::size_t max_config_size{0};
    DeadlineDuration shutdown_timeout{};
};

class PluginManager {
public:
    static Result<PluginManager> create(PluginManagerOptions options);

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    PluginManager(PluginManager&&) noexcept;
    PluginManager& operator=(PluginManager&&) noexcept;
    ~PluginManager() noexcept;

    Result<std::vector<PluginInfo>> plugins() const;
    Result<PluginLibrary> load(std::string_view plugin_id);
    Result<PluginLibrary> load(std::string_view plugin_id, std::string_view plugin_version);
    [[nodiscard]] PluginManagerPhase phase() const noexcept;
    [[nodiscard]] bool restart_required() const noexcept;
    // Only discharges create failures already returned to the caller. A true
    // observation is not quiescence proof for ordinary Open instances.
    [[nodiscard]] bool initialization_cleanup_settled() const noexcept;
    Result<void> shutdown(Deadline deadline);

private:
    explicit PluginManager(std::shared_ptr<ManagerState> state) noexcept;

    std::shared_ptr<ManagerState> state_;
};

}  // namespace rodaq::plugin
