#pragma once

#include <rodaq/export.hpp>

#include <memory>
#include <string_view>

#include <rodaq/plugin/types.hpp>
#include <rodaq/result.hpp>

namespace rodaq::plugin {

struct LibraryState;
struct ManagerState;
class PluginManager;
class PluginInstance;

class PluginLibrary {
public:
    PluginLibrary() = delete;
    PluginLibrary(const PluginLibrary&) = delete;
    PluginLibrary& operator=(const PluginLibrary&) = delete;
    RODAQ_API PluginLibrary(PluginLibrary&&) noexcept;
    RODAQ_API PluginLibrary& operator=(PluginLibrary&&) noexcept;
    RODAQ_API ~PluginLibrary() noexcept;

    RODAQ_API explicit operator bool() const noexcept;
    [[nodiscard]] RODAQ_API const PluginInfo& info() const noexcept;
    RODAQ_API Result<PluginInstance> create_instance(std::string_view config) const;

private:
    friend class PluginManager;

    PluginLibrary(
        std::shared_ptr<ManagerState> manager, std::shared_ptr<LibraryState> state) noexcept;

    std::shared_ptr<ManagerState> manager_;
    std::shared_ptr<LibraryState> state_;
};

}  // namespace rodaq::plugin
