#ifndef RODAQ_PROJECTION_REGISTRY_HPP_
#define RODAQ_PROJECTION_REGISTRY_HPP_

#include <rodaq/plugin/abi/runtime.h>
#include <rodaq/result.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rodaq {

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

class ProjectionRegistry final {
public:
    using RegistrationSink = std::function<void(rodaq_stream_descriptor_handle, std::string_view)>;

    explicit ProjectionRegistry(ProjectionLimits limits);

    ProjectionRegistry(const ProjectionRegistry&) = delete;
    ProjectionRegistry& operator=(const ProjectionRegistry&) = delete;
    ProjectionRegistry(ProjectionRegistry&&) = delete;
    ProjectionRegistry& operator=(ProjectionRegistry&&) = delete;
    ~ProjectionRegistry() = default;

    void set_registration_sink(RegistrationSink sink);

    Result<void> register_entry(
        rodaq_stream_descriptor_handle descriptor, std::string_view projection_id,
        const std::uint8_t* payload, std::size_t payload_size);

    Result<ProjectionView> get(
        rodaq_stream_descriptor_handle descriptor, std::string_view projection_id) const;

    rodaq_interface abi_interface() noexcept;

private:
    struct Entry {
        rodaq_stream_descriptor_handle descriptor;
        std::string projection_id;
        std::vector<std::uint8_t> payload;
    };

    static rodaq_status_code register_projection(
        void* context, const rodaq_projection_registration* request,
        rodaq_status* out_status) noexcept;

    static rodaq_status_code get_projection(
        void* context, rodaq_stream_descriptor_handle descriptor, rodaq_string_view projection_id,
        rodaq_projection_info* out_info, rodaq_status* out_status) noexcept;

    static const rodaq_runtime_projection kProjectionVtable;

private:
    const ProjectionLimits limits_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Entry>> entries_;
    RegistrationSink registration_sink_;
    std::size_t total_payload_bytes_ = 0;
};

}  // namespace rodaq

#endif  // RODAQ_PROJECTION_REGISTRY_HPP_
