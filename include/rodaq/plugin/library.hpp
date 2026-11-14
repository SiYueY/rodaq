#pragma once

#include <memory>
#include <string_view>

#include <rodaq/plugin/types.hpp>
#include <rodaq/result.hpp>

namespace rodaq::plugin {

struct LibraryState;
struct ManagerState;
class PluginManager;
class PluginInstance;
class ServiceRegistry;

class PluginLibrary {
public:
    PluginLibrary() = delete;
    PluginLibrary(const PluginLibrary&) = delete;
    PluginLibrary& operator=(const PluginLibrary&) = delete;
    PluginLibrary(PluginLibrary&&) noexcept;
    PluginLibrary& operator=(PluginLibrary&&) noexcept;
    ~PluginLibrary() noexcept;

    explicit operator bool() const noexcept;
    [[nodiscard]] const PluginInfo& info() const noexcept;
    [[nodiscard]] const PluginBinaryInfo& binary() const noexcept;
    Result<PluginInstance> create_instance(
        const ServiceRegistry& services, std::string_view config) const;

private:
    friend class PluginManager;

    PluginLibrary(
        std::shared_ptr<ManagerState> manager, std::shared_ptr<LibraryState> state) noexcept;

    std::shared_ptr<ManagerState> manager_;
    std::shared_ptr<LibraryState> state_;
};

}  // namespace rodaq::plugin
