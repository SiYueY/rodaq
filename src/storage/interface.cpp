#include <rodaq/storage/interface.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rodaq::storage {

Handle::Handle(plugin::OperationHandle<Interface> operation) noexcept
: operation_(std::move(operation)) {}

Result<CapacityModel> Handle::prepare(const PrepareConfig& config, Deadline deadline) {
    if (!abi::valid_prepare(&config)) {
        return Status(StatusCode::InvalidArgument, "Invalid Storage prepare input");
    }
    max_file_count_ = 0;
    max_path_bytes_ = 0;
    CapacityModel model{};
    model.header.struct_size = sizeof(model);
    auto called = call([&](auto* context, const auto& table, auto* error) {
        return table.prepare(context, &config, deadline.native(), &model, error);
    });
    if (!called.ok()) {
        return called.status();
    }
    if (model.header.struct_size != sizeof(model) || !abi::valid_capacity(model)) {
        return invalid_output("Invalid Storage CapacityModel");
    }
    max_file_count_ = config.max_segment_file_count;
    max_path_bytes_ = config.max_relative_path_bytes;
    return model;
}

Result<void> Handle::prepare_descriptor(const StreamDescriptor& descriptor, Deadline deadline) {
    if (!abi::valid_descriptor(&descriptor)) {
        return Status(StatusCode::InvalidArgument, "Invalid Storage Descriptor");
    }
    return call([&](auto* context, const auto& table, auto* error) {
        return table.prepare_descriptor(context, &descriptor, deadline.native(), error);
    });
}

Result<void> Handle::open_segment(const SegmentSpec& segment, Deadline deadline) {
    if (!abi::valid_segment(&segment)) {
        return Status(StatusCode::InvalidArgument, "Invalid Storage Segment");
    }
    return call([&](auto* context, const auto& table, auto* error) {
        return table.open_segment(context, &segment, deadline.native(), error);
    });
}

WriteOutcome Handle::write_batch(Batch batch, Deadline deadline) {
    if (!abi::valid_batch(batch)) {
        return Status(StatusCode::InvalidArgument, "Invalid Storage Batch");
    }
    return operation_.invoke([&](auto* context, const auto& table) -> WriteOutcome {
        BatchResult result{{sizeof(result)}, 0, RODAQ_STORAGE_TAIL_UNKNOWN};
        std::array<std::uint8_t, 512> bytes{};
        rodaq_error_t error{{bytes.data(), bytes.size(), 0}};
        const plugin::abi::BufferSnapshot expected{bytes.data(), bytes.size()};
        try {
            const auto code = table.write_batch(
                context, batch.records, batch.count, deadline.native(), &result, &error);
            auto status = plugin::abi::status_from_abi(code, error, expected);
            if (!plugin::abi::valid_error(code, error, expected)) {
                operation_.report_abi_violation("Storage write error-buffer violation");
            }
            if (result.header.struct_size != sizeof(result) ||
                !abi::valid_outcome(code, result, batch.count)) {
                return {
                    invalid_output("Invalid Storage BatchResult; entire dispatched Batch Unknown"),
                    {{sizeof(BatchResult)}, 0, RODAQ_STORAGE_TAIL_UNKNOWN}};
            }
            // Known non-OK status never erases the valid confirmed-prefix channel.
            return {std::move(status), result};
        } catch (...) {
            operation_.report_abi_violation("Exception crossed Storage write ABI; Batch Unknown");
            return {
                Status(StatusCode::Internal, {}),
                {{sizeof(BatchResult)}, 0, RODAQ_STORAGE_TAIL_UNKNOWN}};
        }
    });
}

Result<SegmentUsage> Handle::segment_usage() const {
    SegmentUsage usage{};
    usage.header.struct_size = sizeof(usage);
    auto called = call([&](auto* context, const auto& table, auto* error) {
        return table.segment_usage(context, &usage, error);
    });
    if (!called.ok()) {
        return called.status();
    }
    if (usage.header.struct_size != sizeof(usage) || !abi::valid_usage(usage)) {
        return invalid_output("Invalid Storage SegmentUsage");
    }
    return usage;
}

Result<SegmentSealResult> Handle::seal_segment(Deadline deadline) {
    if (max_file_count_ == 0) {
        return Status(StatusCode::InvalidState, "Prepare this Writer Handle before seal");
    }
    try {
        SegmentSealResult result;
        result.files.resize(max_file_count_);
        std::vector<rodaq_sealed_file_t> files(max_file_count_);
        for (std::uint32_t i = 0; i < max_file_count_; ++i) {
            // Reserve all owning result storage before the one native seal.
            auto& path = result.files[i].relative_path;
            path.resize(max_path_bytes_, '\0');
            files[i].relative_path = {path.data(), max_path_bytes_ + 1, 0};
        }
        rodaq_segment_seal_result_t output{{sizeof(output)}, files.data(), max_file_count_, 0, 0};
        auto called = call([&](auto* context, const auto& table, auto* error) {
            return table.seal_segment(context, deadline.native(), &output, error);
        });
        // Identity checks run on every status and before following any path.
        bool identities = output.header.struct_size == sizeof(output) &&
                          output.files == files.data() && output.file_capacity == max_file_count_;
        for (std::uint32_t i = 0; i < max_file_count_; ++i) {
            identities = identities &&
                         files[i].relative_path.data == result.files[i].relative_path.data() &&
                         files[i].relative_path.capacity == max_path_bytes_ + 1;
        }
        if (!identities) {
            operation_.report_abi_violation("Storage seal changed caller-owned buffer identity");
        }
        if (!called.ok()) {
            return called.status();
        }
        if (!identities || output.file_count == 0 || output.file_count > max_file_count_) {
            return invalid_output("Invalid Storage seal output descriptor");
        }
        for (std::uint32_t i = 0; i < max_file_count_; ++i) {
            const auto& native = files[i];
            auto& file = result.files[i];
            const auto size = native.relative_path.size;
            if (i >= output.file_count) {
                if (size != 0 || native.physical_size != 0 ||
                    !std::all_of(
                        file.relative_path.begin(), file.relative_path.end(),
                        [](auto byte) { return byte == 0; }) ||
                    file.relative_path.data()[max_path_bytes_] != '\0') {
                    return invalid_output("Storage seal modified unused file backing");
                }
                continue;
            }
            if (size > max_path_bytes_ || file.relative_path.data()[size] != '\0' ||
                !abi::valid_relative_path({file.relative_path.data(), size})) {
                return invalid_output("Invalid Storage sealed relative path");
            }
            file.relative_path.resize(size);
            file.physical_size = native.physical_size;
        }
        result.files.resize(output.file_count);
        result.total_physical_bytes = output.total_physical_bytes;
        if (!abi::valid_sealed_files(result)) {
            return invalid_output("Invalid Storage sealed file set or checked total");
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::length_error&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> Handle::quiesce(Deadline deadline) {
    return call([&](auto* context, const auto& table, auto* error) {
        return table.quiesce(context, deadline.native(), error);
    });
}

Status Handle::invalid_output(std::string_view message) const {
    operation_.report_abi_violation(message);
    return Status(StatusCode::Internal, std::string(message));
}

template <typename Function>
Result<void> Handle::call(Function&& function) const {
    return operation_.invoke([&](auto* context, const auto& table) -> Result<void> {
        std::array<std::uint8_t, 512> bytes{};
        rodaq_error_t error{{bytes.data(), bytes.size(), 0}};
        const plugin::abi::BufferSnapshot expected{bytes.data(), bytes.size()};
        try {
            const auto code = function(context, table, &error);
            if (!plugin::abi::known_status(code) ||
                !plugin::abi::valid_error(code, error, expected)) {
                operation_.report_abi_violation("Storage status/error ABI violation");
            }
            auto status = plugin::abi::status_from_abi(code, error, expected);
            return status.ok() ? Result<void>{} : Result<void>{std::move(status)};
        } catch (...) {
            operation_.report_abi_violation("Exception crossed Storage operation ABI");
            return Status(StatusCode::Internal, {});
        }
    });
}

}  // namespace rodaq::storage
