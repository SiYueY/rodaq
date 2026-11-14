#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include <rodaq/plugin/close_future.hpp>
#include <rodaq/plugin/operation.hpp>
#include <rodaq/plugin/types.hpp>

namespace rodaq::plugin {

class PluginLibrary;

class PluginInstance {
public:
    PluginInstance() = delete;
    PluginInstance(const PluginInstance&) = delete;
    PluginInstance& operator=(const PluginInstance&) = delete;
    PluginInstance(PluginInstance&&) noexcept;
    PluginInstance& operator=(PluginInstance&&) = delete;
    ~PluginInstance() noexcept;

    explicit operator bool() const noexcept;

    template <typename Interface>
    Result<typename Interface::Handle> query() {
        if (!abi::valid_identifier(Interface::kInterfaceId)) {
            return Status(StatusCode::InvalidArgument, "Invalid Interface ID");
        }
        auto guard = invocation::acquire(state_);
        if (!guard.ok()) {
            return guard.status();
        }
        const rodaq_interface_query_t request{
            {Interface::kInterfaceId.data(),
             static_cast<std::uint32_t>(Interface::kInterfaceId.size())},
            Interface::kMajor,
            0,
            Interface::kMinor};
        auto reference = invocation::query_native(state_, request);
        if (!reference.ok()) {
            return reference.status();
        }
        typename Interface::Vtable table{};
        if (!abi::copy_vtable<Interface>(reference.value(), table)) {
            return Status(StatusCode::Internal, "Invalid Operation vtable");
        }
        // Complete the Handle while CallGuard still pins all provider metadata.
        return typename Interface::Handle(
            OperationHandle<Interface>(state_, reference.value().context, table));
    }

    Result<void> close(Deadline deadline);
    CloseFuture async_close() noexcept;
    [[nodiscard]] PluginInstancePhase phase() const noexcept;
    [[nodiscard]] PluginInstanceId id() const noexcept;

private:
    friend class PluginLibrary;

    explicit PluginInstance(std::shared_ptr<InstanceState> state) noexcept;

    std::shared_ptr<InstanceState> state_;
};

}  // namespace rodaq::plugin
