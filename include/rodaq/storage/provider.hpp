#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/provider.hpp>
#include <rodaq/c/storage.h>
#include <rodaq/storage/validation.hpp>

namespace rodaq::storage {

using plugin::Deadline;

template <typename Implementation>
struct Provider {
    static rodaq_status_code_t prepare(
        rodaq_interface_context_t* context, const PrepareConfig* config, rodaq_deadline_t deadline,
        rodaq_error_t* error) noexcept {
        return control(
            context, deadline, error, [&](auto& implementation, auto budget) -> Result<void> {
                if (!abi::valid_prepare(config)) {
                    return Status(StatusCode::InvalidArgument, "Invalid Storage prepare input");
                }
                return implementation.prepare(*config, budget);
            });
    }

    static rodaq_status_code_t prepare_descriptor(
        rodaq_interface_context_t* context, const StreamDescriptor* descriptor,
        rodaq_deadline_t deadline, rodaq_error_t* error) noexcept {
        return control(
            context, deadline, error, [&](auto& implementation, auto budget) -> Result<void> {
                if (!abi::valid_descriptor(descriptor)) {
                    return Status(StatusCode::InvalidArgument, "Invalid Storage Descriptor");
                }
                return implementation.prepare_descriptor(*descriptor, budget);
            });
    }

    static rodaq_status_code_t open_segment(
        rodaq_interface_context_t* context, const SegmentSpec* segment, rodaq_deadline_t deadline,
        rodaq_error_t* error) noexcept {
        return control(
            context, deadline, error, [&](auto& implementation, auto budget) -> Result<void> {
                if (!abi::valid_segment(segment)) {
                    return Status(StatusCode::InvalidArgument, "Invalid Storage Segment");
                }
                return implementation.open_segment(*segment, budget);
            });
    }

    static rodaq_status_code_t write_batch(
        rodaq_interface_context_t* context, const Record* records, std::uint32_t count,
        rodaq_deadline_t deadline, BatchResult* result, rodaq_error_t* error) noexcept {
        constexpr auto kPrefix =
            offsetof(BatchResult, tail_state) + sizeof(BatchResult::tail_state);
        if (!plugin::abi::readable(result, kPrefix)) {
            plugin::abi::write_error(error, {});
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
        result->confirmed_prefix = 0;
        result->tail_state = RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN;
        return control(
            context, deadline, error, [&](auto& implementation, auto budget) -> Result<void> {
                if (!abi::valid_batch({records, count})) {
                    return Status(StatusCode::InvalidArgument, "Invalid Storage Batch");
                }
                // After actual entry, an exception cannot prove any unwritten suffix.
                result->tail_state = RODAQ_STORAGE_TAIL_UNKNOWN;
                auto outcome = implementation.write_batch(Batch{records, count}, budget);
                const auto code = static_cast<rodaq_status_code_t>(outcome.status.code());
                if (!abi::valid_outcome(code, outcome.batch_result, count)) {
                    return Status(StatusCode::Internal, "Invalid Storage BatchResult");
                }
                result->confirmed_prefix = outcome.batch_result.confirmed_prefix;
                result->tail_state = outcome.batch_result.tail_state;
                return outcome.status.ok() ? Result<void>{}
                                           : Result<void>{std::move(outcome.status)};
            });
    }

    static rodaq_status_code_t segment_usage(
        rodaq_interface_context_t* context, SegmentUsage* usage, rodaq_error_t* error) noexcept {
        constexpr auto kPrefix =
            offsetof(SegmentUsage, reserved0) + sizeof(SegmentUsage::reserved0);
        if (!plugin::abi::readable(usage, kPrefix)) {
            plugin::abi::write_error(error, {});
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
        write_value(usage, SegmentUsage{});
        return plugin::authoring::exception_boundary(error, [&]() -> Result<void> {
            if (context == nullptr) {
                return Status(StatusCode::InvalidArgument, "Missing Storage context");
            }
            auto result = reinterpret_cast<Implementation*>(context)->segment_usage();
            if (!result.ok()) {
                return result.status();
            }
            if (!abi::valid_usage(result.value())) {
                return Status(StatusCode::Internal, "Invalid Storage SegmentUsage");
            }
            write_value(usage, result.value());
            return {};
        });
    }

    static rodaq_status_code_t seal_segment(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline,
        rodaq_segment_seal_result_t* output, rodaq_error_t* error) noexcept {
        constexpr auto kPrefix = offsetof(rodaq_segment_seal_result_t, total_physical_bytes) +
                                 sizeof(rodaq_segment_seal_result_t::total_physical_bytes);
        if (!plugin::abi::readable(output, kPrefix) || output->files == nullptr ||
            output->file_capacity == 0 ||
            reinterpret_cast<std::uintptr_t>(output->files) % alignof(rodaq_sealed_file_t) != 0) {
            plugin::abi::write_error(error, {});
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
        // Validate every nested buffer before even writing default-invalid output.
        for (std::uint32_t i = 0; i < output->file_capacity; ++i) {
            const auto& path = output->files[i].relative_path;
            if (path.data == nullptr || path.capacity == 0) {
                plugin::abi::write_error(error, {});
                return RODAQ_STATUS_INVALID_ARGUMENT;
            }
        }
        output->file_count = 0;
        output->total_physical_bytes = 0;
        for (std::uint32_t i = 0; i < output->file_capacity; ++i) {
            auto& file = output->files[i];
            file.physical_size = 0;
            file.relative_path.size = 0;
            std::memset(file.relative_path.data, 0, file.relative_path.capacity);
        }
        return control(
            context, deadline, error, [&](auto& implementation, auto budget) -> Result<void> {
                auto result = implementation.seal_segment(budget);
                if (!result.ok()) {
                    return result.status();
                }
                if (result.value().files.size() > output->file_capacity) {
                    return Status(StatusCode::ResourceExhausted, "Sealed file capacity exceeded");
                }
                if (!abi::valid_sealed_files(result.value())) {
                    return Status(StatusCode::Internal, "Invalid sealed file set");
                }
                for (std::size_t i = 0; i < result.value().files.size(); ++i) {
                    if (result.value().files[i].relative_path.size() >=
                        output->files[i].relative_path.capacity) {
                        return Status(
                            StatusCode::ResourceExhausted, "Sealed path capacity exceeded");
                    }
                }
                // All fallible checks precede publication; no partial SuccessOnly result.
                for (std::size_t i = 0; i < result.value().files.size(); ++i) {
                    auto& target = output->files[i];
                    const auto& file = result.value().files[i];
                    std::memcpy(
                        target.relative_path.data, file.relative_path.data(),
                        file.relative_path.size());
                    target.relative_path.size =
                        static_cast<std::uint32_t>(file.relative_path.size());
                    target.physical_size = file.physical_size;
                }
                output->file_count = static_cast<std::uint32_t>(result.value().files.size());
                output->total_physical_bytes = result.value().total_physical_bytes;
                return {};
            });
    }

    static rodaq_status_code_t quiesce(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline,
        rodaq_error_t* error) noexcept {
        return control(context, deadline, error, [](auto& implementation, auto budget) {
            return implementation.quiesce(budget);
        });
    }

    static const rodaq_storage_interface_vtable_t* vtable() noexcept {
        static const rodaq_storage_interface_vtable_t table{
            {sizeof(table)}, prepare,       prepare_descriptor, open_segment,
            write_batch,     segment_usage, seal_segment,       quiesce};
        return &table;
    }

private:
    template <typename Value>
    static void write_value(Value* output, const Value& value) noexcept {
        const auto writable = std::min<std::size_t>(output->header.struct_size, sizeof(Value));
        // Preserve the caller's header and never touch a provider-only optional tail.
        std::memcpy(
            reinterpret_cast<std::uint8_t*>(output) + sizeof(rodaq_struct_header_t),
            reinterpret_cast<const std::uint8_t*>(&value) + sizeof(rodaq_struct_header_t),
            writable - sizeof(rodaq_struct_header_t));
    }

    template <typename Function>
    static rodaq_status_code_t control(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error,
        Function&& function) noexcept {
        return plugin::authoring::exception_boundary(error, [&]() -> Result<void> {
            if (context == nullptr) {
                return Status(StatusCode::InvalidArgument, "Missing Storage context");
            }
            auto budget = Deadline::from_native(deadline);
            if (!budget.ok()) {
                return budget.status();
            }
            return function(*reinterpret_cast<Implementation*>(context), budget.value());
        });
    }
};

}  // namespace rodaq::storage
