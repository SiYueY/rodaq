#pragma once

#include <rodaq/acquisition/c/interface.h>
#include <rodaq/acquisition/validation.hpp>
#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/provider.hpp>

namespace rodaq::acquisition {

using plugin::Deadline;

template <typename Implementation>
struct Provider {
    static rodaq_status_code_t prepare(
        rodaq_interface_context_t* context, const PrepareConfig* config, rodaq_deadline_t deadline,
        rodaq_error_t* error) noexcept {
        return control(
            context, deadline, error, [&](auto& implementation, auto budget) -> Result<void> {
                if (!abi::valid_prepare(config)) {
                    return Status(StatusCode::InvalidArgument, "Invalid Acquisition prepare input");
                }
                return implementation.prepare(*config, budget);
            });
    }

    static rodaq_status_code_t start(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline,
        rodaq_error_t* error) noexcept {
        return control(context, deadline, error, [](auto& implementation, auto budget) {
            return implementation.start(budget);
        });
    }

    static rodaq_status_code_t quiesce(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline,
        rodaq_error_t* error) noexcept {
        return control(context, deadline, error, [](auto& implementation, auto budget) {
            return implementation.quiesce(budget);
        });
    }

    static const rodaq_acquisition_interface_vtable_t* vtable() noexcept {
        static const rodaq_acquisition_interface_vtable_t table{
            {sizeof(table)}, prepare, start, quiesce};
        return &table;
    }

private:
    template <typename Function>
    static rodaq_status_code_t control(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error,
        Function&& function) noexcept {
        return plugin::authoring::exception_boundary(error, [&]() -> Result<void> {
            if (context == nullptr) {
                return Status(StatusCode::InvalidArgument, "Missing Acquisition context");
            }
            auto budget = Deadline::from_native(deadline);
            if (!budget.ok()) {
                return budget.status();
            }
            return function(*reinterpret_cast<Implementation*>(context), budget.value());
        });
    }
};

}  // namespace rodaq::acquisition
