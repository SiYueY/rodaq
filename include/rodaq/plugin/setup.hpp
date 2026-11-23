#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include <rodaq/plugin/service_reference.hpp>

namespace rodaq::plugin {

namespace authoring {
template <typename Plugin>
struct Instance;
}

class PluginSetup {
public:
    PluginSetup(const PluginSetup&) = delete;
    PluginSetup& operator=(const PluginSetup&) = delete;
    PluginSetup(PluginSetup&&) = delete;
    PluginSetup& operator=(PluginSetup&&) = delete;

    [[nodiscard]] std::string_view config() const noexcept { return config_; }

    template <typename Interface, typename Implementation>
    Result<void> provide(Implementation& implementation) {
        return authoring::add_interface<Interface>(entries_, implementation);
    }

    template <typename Interface>
    Result<typename Interface::Reference> service(std::uint16_t minimum_minor = 0) {
        const auto id = Interface::kInterfaceId;
        if (!abi::valid_identifier(id) || minimum_minor > Interface::kMinor) {
            return Status(StatusCode::InvalidArgument, "Invalid Service query");
        }
        const rodaq_interface_query_t query{
            {id.data(), static_cast<std::uint32_t>(id.size())},
            Interface::kMajor,
            minimum_minor,
            Interface::kMinor};
        std::array<std::uint8_t, 512> buffer{};
        rodaq_error_t error{{buffer.data(), buffer.size(), 0}};
        rodaq_interface_reference_t reference{};
        const abi::BufferSnapshot expected{buffer.data(), buffer.size()};
        const auto code = services_.vtable->query(services_.context, &query, &reference, &error);
        auto status = abi::status_from_abi(code, error, expected);
        if (!status.ok()) {
            return status;
        }
        typename Interface::Vtable table{};
        if (reference.negotiated_version.minor < minimum_minor ||
            !abi::copy_vtable<Interface>(reference, table)) {
            return Status(StatusCode::Internal, "Invalid Service vtable");
        }
        return
            typename Interface::Reference(ServiceReference<Interface>(services_, reference, table));
    }

private:
    template <typename Plugin>
    friend struct authoring::Instance;

    PluginSetup(
        std::string_view config, rodaq_service_registry_reference_t services,
        std::vector<authoring::InterfaceEntry>& entries) noexcept
    : config_(config), services_(services), entries_(entries) {}

    std::string_view config_;
    rodaq_service_registry_reference_t services_;
    std::vector<authoring::InterfaceEntry>& entries_;
};

}  // namespace rodaq::plugin
