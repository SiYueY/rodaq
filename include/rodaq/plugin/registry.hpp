#pragma once

#include <memory>
#include <utility>

#include <rodaq/plugin/provider.hpp>

namespace rodaq::plugin {

struct RegistryState;
class PluginLibrary;

/** Move-only builder. Frozen backing is retained by each created Plugin Instance. */
class ServiceRegistry {
public:
    ServiceRegistry();
    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;
    ServiceRegistry(ServiceRegistry&&) noexcept;
    ServiceRegistry& operator=(ServiceRegistry&&) noexcept;
    ~ServiceRegistry() noexcept;

    template <typename Interface, typename Implementation>
    Result<void> add_shared(std::shared_ptr<Implementation> implementation) {
        if (!implementation) {
            return Status(StatusCode::InvalidArgument, "Missing Service provider");
        }
        auto binding = authoring::bind_interface<Interface>(*implementation);
        if (!binding.ok()) {
            return binding.status();
        }
        return add_binding(std::move(binding).value(), std::move(implementation));
    }

    /** Only process-static providers are legal; no ownership is acquired. */
    template <typename Interface, typename Implementation>
    Result<void> add_static(Implementation& implementation) {
        auto binding = authoring::bind_interface<Interface>(implementation);
        if (!binding.ok()) {
            return binding.status();
        }
        return add_binding(std::move(binding).value(), {});
    }

    Result<void> freeze();
    [[nodiscard]] bool frozen() const noexcept;

private:
    friend class PluginLibrary;

    Result<void> add_binding(authoring::InterfaceEntry binding, std::shared_ptr<void> owner);

    std::shared_ptr<RegistryState> state_;
};

}  // namespace rodaq::plugin
