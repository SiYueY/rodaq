#pragma once

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/interface.hpp>
#include <rodaq/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rodaq::plugin {

namespace authoring {
class PluginSetupFactory;
}

class PluginBase {
public:
    virtual ~PluginBase() noexcept = default;

    PluginBase(const PluginBase&) = delete;
    PluginBase& operator=(const PluginBase&) = delete;

    [[nodiscard]] virtual Result<void> configure(PluginSetup& setup) = 0;
    [[nodiscard]] virtual Result<void> shutdown(Deadline deadline) = 0;

protected:
    PluginBase() = default;
};

class PluginSetup final {
public:
    PluginSetup(const PluginSetup&) = delete;
    PluginSetup& operator=(const PluginSetup&) = delete;
    PluginSetup(PluginSetup&&) = delete;
    PluginSetup& operator=(PluginSetup&&) = delete;

    [[nodiscard]] std::string_view config() const noexcept { return config_; }

    template <typename ServiceInterface>
    [[nodiscard]] Result<typename ServiceInterface::Reference> service() {
        if (services_ == nullptr || services_->context == nullptr || services_->vtable == nullptr ||
            services_->vtable->query == nullptr) {
            return Status{StatusCode::InvalidState, "service registry is unavailable"};
        }

        rodaq_interface_query_t query{};
        query.interface_id = rodaq_string_t{
            ServiceInterface::kInterfaceId.data(),
            static_cast<std::uint32_t>(ServiceInterface::kInterfaceId.size())};
        query.major = ServiceInterface::kMajor;
        query.min_minor = ServiceInterface::kMinMinor;
        query.max_minor = ServiceInterface::kMinor;

        rodaq_interface_reference_t reference{};
        std::array<std::uint8_t, interface_support::kErrorMessageCapacity> error_storage{};
        rodaq_error_t error{};
        error.message.data = error_storage.data();
        error.message.capacity = static_cast<std::uint32_t>(error_storage.size());
        const auto code = services_->vtable->query(services_->context, &query, &reference, &error);
        auto result = interface_support::result_from_abi_call(code, &error);
        if (!result.ok()) {
            return result.status();
        }
        auto validated = interface_support::validate_typed_reference<ServiceInterface>(reference);
        if (!validated.ok()) {
            return validated.status();
        }
        return typename ServiceInterface::Reference{reference, *services_};
    }

    template <typename OperationInterface, typename Implementation>
    [[nodiscard]] Result<void> provide(Implementation& implementation) {
        if (builder_ == nullptr || add_operation_ == nullptr) {
            return Status{StatusCode::InvalidState, "operation builder is unavailable"};
        }

        if (OperationInterface::kInterfaceId.size() > UINT32_MAX ||
            !interface_support::valid_utf8(OperationInterface::kInterfaceId)) {
            return Status{StatusCode::InvalidArgument, "operation interface id is invalid"};
        }
        using Provider = typename OperationInterface::template Provider<Implementation>;
        const auto* vtable = Provider::vtable();
        if (!OperationInterface::required_slots_valid(vtable)) {
            return Status{StatusCode::InvalidArgument, "operation provider vtable is incomplete"};
        }
        return add_operation_(
            builder_, OperationInterface::kInterfaceId, OperationInterface::kMajor,
            OperationInterface::kMinor, &implementation, vtable,
            static_cast<std::uint32_t>(sizeof(typename OperationInterface::Vtable)));
    }

private:
    friend class authoring::PluginSetupFactory;

    using AddOperationFunction = Result<void> (*)(
        void*, std::string_view, std::uint16_t, std::uint16_t, void*, const void*, std::uint32_t);

    PluginSetup(
        std::string_view config, const rodaq_service_registry_reference_t* services, void* builder,
        AddOperationFunction add_operation) noexcept
    : config_(config), services_(services), builder_(builder), add_operation_(add_operation) {}

    std::string_view config_;
    const rodaq_service_registry_reference_t* services_;
    void* builder_;
    AddOperationFunction add_operation_;
};

}  // namespace rodaq::plugin
