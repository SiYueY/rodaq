#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rodaq/plugin/operation.hpp>
#include <rodaq/storage/provider.hpp>

namespace rodaq::storage {

class Handle;

struct Interface {
    using Vtable = rodaq_storage_interface_vtable_t;
    using Handle = storage::Handle;
    static constexpr std::string_view kInterfaceId = RODAQ_STORAGE_INTERFACE_ID;
    static constexpr std::uint16_t kMajor = RODAQ_STORAGE_INTERFACE_MAJOR;
    static constexpr std::uint16_t kMinor = RODAQ_STORAGE_INTERFACE_MINOR;

    template <typename Implementation>
    using Provider = storage::Provider<Implementation>;

    static constexpr std::size_t minimum_required_prefix_size() noexcept {
        return offsetof(Vtable, quiesce) + sizeof(Vtable::quiesce);
    }

    static bool valid_vtable(const Vtable& table, std::uint16_t) noexcept {
        return table.prepare != nullptr && table.prepare_descriptor != nullptr &&
               table.open_segment != nullptr && table.write_batch != nullptr &&
               table.segment_usage != nullptr && table.seal_segment != nullptr &&
               table.quiesce != nullptr;
    }
};

/**
 * Header-only managed wrapper (keeps domain ABI symbols out of librodaq_plugin.so).
 * One Writer lane owns a prepared Handle. Cached limits size caller-owned seal
 * backing only; the backend and Core retain all business lifecycle authorities.
 */
class Handle {
public:
    explicit Handle(plugin::OperationHandle<Interface> operation) noexcept
    : operation_(std::move(operation)) {}

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&&) noexcept = default;
    Handle& operator=(Handle&&) noexcept = default;

    Result<void> prepare(const PrepareConfig& config, Deadline deadline) {
        if (!abi::valid_prepare(&config)) {
            return Status(StatusCode::InvalidArgument, "Invalid Storage prepare input");
        }
        max_file_count_ = 0;
        max_path_bytes_ = 0;
        auto called = call([&](auto* context, const auto& table, auto* error) {
            return table.prepare(context, &config, deadline.native(), error);
        });
        if (!called.ok()) {
            return called.status();
        }
        max_file_count_ = config.max_segment_file_count;
        max_path_bytes_ = config.max_relative_path_bytes;
        return {};
    }

    Result<void> prepare_descriptor(const StreamDescriptor& descriptor, Deadline deadline) {
        if (!abi::valid_descriptor(&descriptor)) {
            return Status(StatusCode::InvalidArgument, "Invalid Storage Descriptor");
        }
        return call([&](auto* context, const auto& table, auto* error) {
            return table.prepare_descriptor(context, &descriptor, deadline.native(), error);
        });
    }

    Result<void> open_segment(const SegmentSpec& segment, Deadline deadline) {
        if (!abi::valid_segment(&segment)) {
            return Status(StatusCode::InvalidArgument, "Invalid Storage Segment");
        }
        return call([&](auto* context, const auto& table, auto* error) {
            return table.open_segment(context, &segment, deadline.native(), error);
        });
    }

    WriteOutcome write_batch(Batch batch, Deadline deadline) {
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
                        invalid_output(
                            "Invalid Storage BatchResult; entire dispatched Batch Unknown"),
                        {{sizeof(BatchResult)}, 0, RODAQ_STORAGE_TAIL_UNKNOWN}};
                }
                // Known non-OK status never erases the valid confirmed-prefix channel.
                return {std::move(status), result};
            } catch (...) {
                operation_.report_abi_violation(
                    "Exception crossed Storage write ABI; Batch Unknown");
                return {
                    Status(StatusCode::Internal, {}),
                    {{sizeof(BatchResult)}, 0, RODAQ_STORAGE_TAIL_UNKNOWN}};
            }
        });
    }

    Result<SegmentUsage> segment_usage() const {
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

    Result<SegmentSealResult> seal_segment(Deadline deadline) {
        if (max_file_count_ == 0) {
            return Status(StatusCode::InvalidState, "Prepare this Writer Handle before seal");
        }
        try {
            SegmentSealResult result;
            result.files.resize(max_file_count_);
            std::vector<rodaq_sealed_file_t> files(max_file_count_);
            for (std::uint32_t i = 0; i < max_file_count_; ++i) {
                auto& path = result.files[i].relative_path;
                path.resize(max_path_bytes_, '\0');
                files[i].relative_path = {path.data(), max_path_bytes_ + 1, 0};
            }
            rodaq_segment_seal_result_t output{
                {sizeof(output)}, files.data(), max_file_count_, 0, 0};
            auto called = call([&](auto* context, const auto& table, auto* error) {
                return table.seal_segment(context, deadline.native(), &output, error);
            });
            bool identities = output.header.struct_size == sizeof(output) &&
                              output.files == files.data() &&
                              output.file_capacity == max_file_count_;
            for (std::uint32_t i = 0; i < max_file_count_; ++i) {
                identities = identities &&
                             files[i].relative_path.data == result.files[i].relative_path.data() &&
                             files[i].relative_path.capacity == max_path_bytes_ + 1;
            }
            if (!identities) {
                operation_.report_abi_violation(
                    "Storage seal changed caller-owned buffer identity");
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

    Result<void> quiesce(Deadline deadline) {
        return call([&](auto* context, const auto& table, auto* error) {
            return table.quiesce(context, deadline.native(), error);
        });
    }

private:
    Status invalid_output(std::string_view message) const {
        operation_.report_abi_violation(message);
        return Status(StatusCode::Internal, std::string(message));
    }

    template <typename Function>
    Result<void> call(Function&& function) const {
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

    plugin::OperationHandle<Interface> operation_;
    std::uint32_t max_file_count_{0};
    std::uint32_t max_path_bytes_{0};
};

}  // namespace rodaq::storage
