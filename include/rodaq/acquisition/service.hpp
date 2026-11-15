#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

#include <rodaq/acquisition/c/interface.h>
#include <rodaq/acquisition/validation.hpp>
#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/service_reference.hpp>

namespace rodaq::acquisition {

class ServiceReference;

template <typename Implementation>
struct ServiceProvider {
    static rodaq_status_code_t submit_record(
        rodaq_interface_context_t* context, const RecordAttempt* record,
        SubmitResult* submission) noexcept {
        // Validate capacity before any write, including failure defaults.
        if (!plugin::abi::readable(submission)) {
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
        submission->disposition = RODAQ_SUBMIT_INVALID;
        submission->reserved0 = 0;
        return plugin::authoring::exception_boundary(nullptr, [&]() -> Result<void> {
            if (context == nullptr || !abi::valid_record(record)) {
                return Status(StatusCode::InvalidArgument, "Invalid Acquisition submit input");
            }
            auto result = reinterpret_cast<Implementation*>(context)->submit_record(*record);
            if (!result.ok()) {
                return result.status();
            }
            if (!abi::valid_submission(result.value())) {
                return Status(StatusCode::Internal, "Invalid Acquisition disposition");
            }
            submission->disposition = result.value().disposition;
            return {};
        });
    }

    static rodaq_status_code_t propose_descriptor(
        rodaq_interface_context_t* context, const DescriptorProposal* proposal,
        rodaq_deadline_t deadline, DescriptorToken* token, rodaq_error_t* error) noexcept {
        const bool aligned_token =
            token != nullptr &&
            reinterpret_cast<std::uintptr_t>(token) % alignof(DescriptorToken) == 0;
        if (aligned_token) {
            *token = {};
        }
        return plugin::authoring::exception_boundary(error, [&]() -> Result<void> {
            if (context == nullptr || !aligned_token || !abi::valid_descriptor(proposal)) {
                return Status(StatusCode::InvalidArgument, "Invalid Descriptor proposal input");
            }
            auto budget = plugin::Deadline::from_native(deadline);
            if (!budget.ok()) {
                return budget.status();
            }
            auto result = reinterpret_cast<Implementation*>(context)->propose_descriptor(
                *proposal, budget.value());
            if (!result.ok()) {
                return result.status();
            }
            if (result.value().value == 0 || result.value().generation == 0) {
                return Status(StatusCode::Internal, "Invalid Descriptor token");
            }
            *token = result.value();
            return {};
        });
    }

    static rodaq_status_code_t report_source_event(
        rodaq_interface_context_t* context, const SourceEvent* event) noexcept {
        return plugin::authoring::exception_boundary(nullptr, [&]() -> Result<void> {
            if (context == nullptr || !abi::valid_source_event(event)) {
                return Status(StatusCode::InvalidArgument, "Invalid Source event");
            }
            return reinterpret_cast<Implementation*>(context)->report_source_event(*event);
        });
    }

    static rodaq_status_code_t report_acquisition_event(
        rodaq_interface_context_t* context, const AcquisitionEvent* event) noexcept {
        return plugin::authoring::exception_boundary(nullptr, [&]() -> Result<void> {
            if (context == nullptr || !abi::valid_acquisition_event(event)) {
                return Status(StatusCode::InvalidArgument, "Invalid Acquisition event");
            }
            return reinterpret_cast<Implementation*>(context)->report_acquisition_event(*event);
        });
    }

    static const rodaq_acquisition_service_vtable_t* vtable() noexcept {
        static const rodaq_acquisition_service_vtable_t table{
            {sizeof(table)},
            submit_record,
            propose_descriptor,
            report_source_event,
            report_acquisition_event};
        return &table;
    }
};

struct Service {
    using Vtable = rodaq_acquisition_service_vtable_t;
    using Reference = ServiceReference;
    static constexpr std::string_view kInterfaceId = RODAQ_ACQUISITION_SERVICE_ID;
    static constexpr std::uint16_t kMajor = RODAQ_ACQUISITION_SERVICE_MAJOR;
    static constexpr std::uint16_t kMinor = RODAQ_ACQUISITION_SERVICE_MINOR;

    template <typename Implementation>
    using Provider = ServiceProvider<Implementation>;

    static constexpr std::size_t minimum_required_prefix_size() noexcept {
        return offsetof(Vtable, report_acquisition_event) +
               sizeof(Vtable::report_acquisition_event);
    }

    static bool valid_vtable(const Vtable& table, std::uint16_t) noexcept {
        return table.submit_record != nullptr && table.propose_descriptor != nullptr &&
               table.report_source_event != nullptr && table.report_acquisition_event != nullptr;
    }
};

/** Borrowed capability. Every callback enters the Foundation managed-call domain. */
class ServiceReference {
public:
    explicit ServiceReference(plugin::ServiceReference<Service> reference)
    : reference_(std::move(reference)) {}

    Result<SubmitResult> submit_record(const RecordAttempt& record) const {
        if (!abi::valid_record(&record)) {
            return Status(StatusCode::InvalidArgument, "Invalid Acquisition submit input");
        }
        return call([&](auto* context, const auto& table) -> Result<SubmitResult> {
            SubmitResult result{{sizeof(result)}, RODAQ_SUBMIT_INVALID, 0};
            const auto code = table.submit_record(context, &record, &result);
            // SuccessOnly: never inspect business output after failure.
            if (code != RODAQ_STATUS_OK) {
                return status_only(code);
            }
            if (result.header.struct_size != sizeof(result) || !abi::valid_submission(result)) {
                return Status(StatusCode::Internal, "Invalid Acquisition submit output");
            }
            return result;
        });
    }

    Result<DescriptorToken> propose_descriptor(
        const DescriptorProposal& proposal, plugin::Deadline deadline) const {
        if (!abi::valid_descriptor(&proposal)) {
            return Status(StatusCode::InvalidArgument, "Invalid Descriptor proposal input");
        }
        return call([&](auto* context, const auto& table) -> Result<DescriptorToken> {
            DescriptorToken token{};
            std::array<std::uint8_t, 512> bytes{};
            rodaq_error_t error{{bytes.data(), bytes.size(), 0}};
            const plugin::abi::BufferSnapshot expected{bytes.data(), bytes.size()};
            const auto code =
                table.propose_descriptor(context, &proposal, deadline.native(), &token, &error);
            auto status = plugin::abi::status_from_abi(code, error, expected);
            if (!status.ok()) {
                return status;
            }
            if (token.value == 0 || token.generation == 0) {
                return Status(StatusCode::Internal, "Invalid Descriptor token");
            }
            return token;
        });
    }

    Result<void> report_source_event(const SourceEvent& event) const {
        if (!abi::valid_source_event(&event)) {
            return Status(StatusCode::InvalidArgument, "Invalid Source event");
        }
        return call([&](auto* context, const auto& table) -> Result<void> {
            auto status = status_only(table.report_source_event(context, &event));
            return status.ok() ? Result<void>{} : Result<void>{std::move(status)};
        });
    }

    Result<void> report_acquisition_event(const AcquisitionEvent& event) const {
        if (!abi::valid_acquisition_event(&event)) {
            return Status(StatusCode::InvalidArgument, "Invalid Acquisition event");
        }
        return call([&](auto* context, const auto& table) -> Result<void> {
            auto status = status_only(table.report_acquisition_event(context, &event));
            return status.ok() ? Result<void>{} : Result<void>{std::move(status)};
        });
    }

private:
    static Status status_only(rodaq_status_code_t code) {
        if (!plugin::abi::known_status(code)) {
            return Status(StatusCode::Internal, "Unknown Acquisition callback status");
        }
        return code == RODAQ_STATUS_OK ? Status{} : Status(static_cast<StatusCode>(code), {});
    }

    template <typename Function>
    auto call(Function&& function) const
        -> std::invoke_result_t<Function, rodaq_interface_context_t*, const Service::Vtable&> {
        using Return =
            std::invoke_result_t<Function, rodaq_interface_context_t*, const Service::Vtable&>;
        try {
            return reference_.invoke(std::forward<Function>(function));
        } catch (...) {
            return Return(
                Status(StatusCode::Internal, "Exception crossed Acquisition Service ABI"));
        }
    }

    plugin::ServiceReference<Service> reference_;
};

}  // namespace rodaq::acquisition
