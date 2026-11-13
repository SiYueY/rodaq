#pragma once

#include <rodaq/plugin/interface.hpp>
#include <rodaq/result.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace rodaq::plugin {

class ServiceRegistryState;
class PluginLibrary;

class ServiceRegistry final {
public:
    ServiceRegistry();

    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;
    ServiceRegistry(ServiceRegistry&&) noexcept;
    ServiceRegistry& operator=(ServiceRegistry&&) noexcept;
    ~ServiceRegistry();

    template <typename ServiceInterface, typename Implementation>
    Result<void> add_shared(std::shared_ptr<Implementation> implementation) {
        if (implementation == nullptr) {
            return Status{StatusCode::InvalidArgument, "service implementation is null"};
        }
        if (ServiceInterface::kInterfaceId.size() > UINT32_MAX ||
            !interface_support::valid_utf8(ServiceInterface::kInterfaceId)) {
            return Status{StatusCode::InvalidArgument, "service interface id is invalid"};
        }

        using Provider = typename ServiceInterface::template Provider<Implementation>;
        const auto* vtable = Provider::vtable();
        if (!ServiceInterface::required_slots_valid(vtable)) {
            return Status{StatusCode::InvalidArgument, "service provider vtable is incomplete"};
        }
        return add_entry(
            ServiceInterface::kInterfaceId, ServiceInterface::kMajor, ServiceInterface::kMinor,
            implementation.get(), vtable,
            static_cast<std::uint32_t>(sizeof(typename ServiceInterface::Vtable)),
            std::shared_ptr<const void>{implementation});
    }

    template <typename ServiceInterface, typename Implementation>
    Result<void> add_static(Implementation& implementation) {
        if (ServiceInterface::kInterfaceId.size() > UINT32_MAX ||
            !interface_support::valid_utf8(ServiceInterface::kInterfaceId)) {
            return Status{StatusCode::InvalidArgument, "service interface id is invalid"};
        }
        using Provider = typename ServiceInterface::template Provider<Implementation>;
        const auto* vtable = Provider::vtable();
        if (!ServiceInterface::required_slots_valid(vtable)) {
            return Status{StatusCode::InvalidArgument, "service provider vtable is incomplete"};
        }
        return add_entry(
            ServiceInterface::kInterfaceId, ServiceInterface::kMajor, ServiceInterface::kMinor,
            &implementation, vtable,
            static_cast<std::uint32_t>(sizeof(typename ServiceInterface::Vtable)), {});
    }

    Result<void> freeze();
    [[nodiscard]] bool frozen() const noexcept;

private:
    friend class PluginLibrary;

    [[nodiscard]] std::shared_ptr<const ServiceRegistryState> backing() const;

    Result<void> add_entry(
        std::string_view interface_id, std::uint16_t major, std::uint16_t minor, void* context,
        const void* vtable, std::uint32_t vtable_size, std::shared_ptr<const void> owner);

    std::shared_ptr<ServiceRegistryState> state_;
};

}  // namespace rodaq::plugin
