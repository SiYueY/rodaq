#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

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

/** One Writer lane owns a prepared Handle. Cached limits size caller-owned seal
 * backing only; the backend and Core retain all business lifecycle authorities.
 */
class Handle {
public:
    explicit Handle(plugin::OperationHandle<Interface> operation) noexcept;

    Handle(const Handle&) = delete;

    Handle& operator=(const Handle&) = delete;

    Handle(Handle&&) noexcept = default;

    Handle& operator=(Handle&&) noexcept = default;

    Result<CapacityModel> prepare(const PrepareConfig& config, Deadline deadline);

    Result<void> prepare_descriptor(const StreamDescriptor& descriptor, Deadline deadline);

    Result<void> open_segment(const SegmentSpec& segment, Deadline deadline);

    WriteOutcome write_batch(Batch batch, Deadline deadline);

    Result<SegmentUsage> segment_usage() const;

    Result<SegmentSealResult> seal_segment(Deadline deadline);

    Result<void> quiesce(Deadline deadline);

private:
    Status invalid_output(std::string_view message) const;

    template <typename Function>
    Result<void> call(Function&& function) const;

    plugin::OperationHandle<Interface> operation_;
    std::uint32_t max_file_count_{0};
    std::uint32_t max_path_bytes_{0};
};

}  // namespace rodaq::storage
