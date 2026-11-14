#pragma once

#include <memory>
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
    CallGuard(CallGuard&&) noexcept;
    CallGuard& operator=(CallGuard&&) = delete;
    ~CallGuard() noexcept;

private:
    friend Result<CallGuard> acquire(const std::shared_ptr<InstanceState>& state);

    explicit CallGuard(std::shared_ptr<InstanceState> state) noexcept;

    std::shared_ptr<InstanceState> state_;
};

Result<CallGuard> acquire(const std::shared_ptr<InstanceState>& state);
Result<rodaq_interface_reference_t> query_native(
    const std::shared_ptr<InstanceState>& state, const rodaq_interface_query_t& query);

}  // namespace invocation

/** Typed function-table snapshot plus managed owner; every actual call reenters admission. */
template <typename Interface>
class OperationHandle {
public:
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
