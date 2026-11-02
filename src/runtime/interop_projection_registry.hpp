#ifndef RODAQ_RUNTIME_INTEROP_PROJECTION_REGISTRY_HPP_
#define RODAQ_RUNTIME_INTEROP_PROJECTION_REGISTRY_HPP_

#include <rodaq/plugin/abi/runtime.h>
#include <rodaq/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rodaq::runtime {

struct ProjectionLimits {
    std::size_t max_projection_id_bytes;
    std::size_t max_projection_count_per_descriptor;
    std::size_t max_projection_bytes_per_descriptor;
    std::size_t max_projection_bytes_per_activation;
};

struct ProjectionView {
    std::string_view projection_id;
    const std::uint8_t* payload;
    std::size_t payload_size;
};

class InteropProjectionRegistry final {
public:
    explicit InteropProjectionRegistry(ProjectionLimits limits);

    InteropProjectionRegistry(const InteropProjectionRegistry&) = delete;
    InteropProjectionRegistry& operator=(const InteropProjectionRegistry&) = delete;
    InteropProjectionRegistry(InteropProjectionRegistry&&) = delete;
    InteropProjectionRegistry& operator=(InteropProjectionRegistry&&) = delete;
    ~InteropProjectionRegistry() = default;

    Result<void> Register(
        rodaq_stream_descriptor_handle descriptor, std::string_view projection_id,
        const std::uint8_t* payload, std::size_t payload_size);

    Result<ProjectionView> Get(
        rodaq_stream_descriptor_handle descriptor, std::string_view projection_id) const;

    rodaq_interface AbiInterface() noexcept;

private:
    struct Entry {
        rodaq_stream_descriptor_handle descriptor;
        std::string projection_id;
        std::vector<std::uint8_t> payload;
    };

    static rodaq_status register_projection(
        void* context, const rodaq_projection_registration* request,
        rodaq_error* out_error) noexcept;

    static rodaq_status get_projection(
        void* context, rodaq_stream_descriptor_handle descriptor, rodaq_string_view projection_id,
        rodaq_projection_view* out_view, rodaq_error* out_error) noexcept;

    static const rodaq_runtime_projection_v1 kProjectionVtable;

    const ProjectionLimits limits_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::size_t total_payload_bytes_ = 0;
};

}  // namespace rodaq::runtime

#endif  // RODAQ_RUNTIME_INTEROP_PROJECTION_REGISTRY_HPP_
