#pragma once

#include <rodaq/export.hpp>

#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include <rodaq/plugin/abi.hpp>
#include <rodaq/result.hpp>

namespace rodaq::plugin {

struct InstanceState;
class PluginInstance;

namespace invocation {

/** SDK bridge only. Owned by one synchronous invocation on its calling thread. */
class CallGuard {
public:
    CallGuard(const CallGuard&) = delete;
    CallGuard& operator=(const CallGuard&) = delete;
    RODAQ_API CallGuard(CallGuard&&) noexcept;
    CallGuard& operator=(CallGuard&&) = delete;
    RODAQ_API ~CallGuard() noexcept;

private:
    friend RODAQ_API Result<CallGuard> acquire(const std::shared_ptr<InstanceState>& state);

    explicit CallGuard(std::shared_ptr<InstanceState> state) noexcept;

    std::shared_ptr<InstanceState> state_;
};

RODAQ_API Result<CallGuard> acquire(const std::shared_ptr<InstanceState>& state);
RODAQ_API Result<rodaq_interface_reference_t> query_native(
    const std::shared_ptr<InstanceState>& state, const rodaq_interface_query_t& query);

RODAQ_API void report_abi_violation(
    const std::shared_ptr<InstanceState>& state, std::string_view message) noexcept;

}  // namespace invocation

/** Typed function-table snapshot plus managed owner; every actual call reenters admission. */
template <typename Interface>
class OperationHandle {
public:
    void report_abi_violation(std::string_view message) const noexcept {
        invocation::report_abi_violation(state_, message);
    }

    template <typename Function>
    auto invoke(Function&& function) const {
        using Return = std::invoke_result_t<
            Function, rodaq_interface_context_t*, const typename Interface::Vtable&>;
        auto guard = invocation::acquire(state_);
        if (!guard.ok()) {
            return Return(guard.status());
        }
        // Interface wrappers own output validation and policy-specific exception
        // settlement (especially Storage ValidOutcome); no generic result erasure.
        return function(context_, table_);
    }

private:
    friend class PluginInstance;

    OperationHandle(
        std::shared_ptr<InstanceState> state, rodaq_interface_context_t* context,
        typename Interface::Vtable table) noexcept
    : state_(std::move(state)), context_(context), table_(table) {}

    std::shared_ptr<InstanceState> state_;
    rodaq_interface_context_t* context_;
    typename Interface::Vtable table_;
};

}  // namespace rodaq::plugin
