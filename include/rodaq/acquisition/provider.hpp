#pragma once

#include <array>
#include <cstdint>

#include <rodaq/acquisition/c/interface.h>
#include <rodaq/acquisition/validation.hpp>
#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/provider.hpp>

namespace rodaq::acquisition {

using plugin::Deadline;

/** Typed borrowed wrapper over Core-owned Acquisition callbacks (valid until quiesce completes). */
class Callbacks {
public:
    Callbacks() = default;

    explicit Callbacks(CallbacksTable table) noexcept : table_(table) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return table_.prepare_descriptor != nullptr && table_.submit_record != nullptr &&
               table_.report_event != nullptr;
    }

    Result<rodaq_descriptor_id_t> prepare_descriptor(
        rodaq_stream_id_t stream_id, const Descriptor& descriptor) const {
        if (!*this || stream_id == 0 || !abi::valid_descriptor(&descriptor)) {
            return Status(StatusCode::InvalidArgument, "Invalid Acquisition descriptor");
        }
        rodaq_descriptor_id_t descriptor_id = 0;
        std::array<std::uint8_t, 512> bytes{};
        rodaq_error_t error{{bytes.data(), bytes.size(), 0}};
        const plugin::abi::BufferSnapshot expected{bytes.data(), bytes.size()};
        const auto code = table_.prepare_descriptor(
            table_.context, stream_id, &descriptor, &descriptor_id, &error);
        auto status = plugin::abi::status_from_abi(code, error, expected);
        if (!status.ok()) {
            return status;
        }
        if (descriptor_id == 0) {
            return Status(StatusCode::Internal, "Core returned zero descriptor_id");
        }
        return descriptor_id;
    }

    RecordDisposition submit_record(const RecordAttempt& attempt) const noexcept {
        if (!*this || !abi::valid_record(&attempt)) {
            return RODAQ_SUBMIT_INVALID_RECORD;
        }
        const auto disposition = table_.submit_record(table_.context, &attempt);
        return abi::valid_disposition(disposition)
                   ? disposition
                   : static_cast<RecordDisposition>(RODAQ_SUBMIT_INVALID);
    }

    void report_event(const AcquisitionEvent& event) const noexcept {
        if (!*this || !abi::valid_acquisition_event(&event)) {
            return;
        }
        table_.report_event(table_.context, &event);
    }

private:
    CallbacksTable table_{};
};

template <typename Implementation>
struct Provider {
    static rodaq_status_code_t prepare(
        rodaq_interface_context_t* context, const PrepareConfig* config,
        const CallbacksTable* callbacks, rodaq_deadline_t deadline, rodaq_error_t* error) noexcept {
        return control(
            context, deadline, error, [&](auto& implementation, auto budget) -> Result<void> {
                if (!abi::valid_prepare(config) || !abi::valid_callbacks(callbacks)) {
                    return Status(StatusCode::InvalidArgument, "Invalid Acquisition prepare input");
                }
                return implementation.prepare(*config, Callbacks(*callbacks), budget);
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
