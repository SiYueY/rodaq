#include "qos.hpp"

#include <iterator>
#include <new>

#include <fmt/format.h>
#include <rodaq/canonical/validation.hpp>

namespace rodaq::mcap_storage {

Result<std::string> encode_qos(const rodaq_ros2_qos_profile_t* profiles, std::uint32_t count) {
    if (count > kMaxQosProfiles || ((count == 0) != (profiles == nullptr))) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (count == 0) {
        return std::string("[]\n");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!canonical::valid_qos(profiles[i])) {
            return Status(StatusCode::InvalidArgument, {});
        }
    }
    try {
        std::string output;
        output.reserve(count * kMaxEncodedQosBytes);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& qos = profiles[i];
            fmt::format_to(
                std::back_inserter(output), kQosFormat, qos.history, qos.depth, qos.reliability,
                qos.durability, qos.deadline.sec, qos.deadline.nsec, qos.lifespan.sec,
                qos.lifespan.nsec, qos.liveliness, qos.liveliness_lease_duration.sec,
                qos.liveliness_lease_duration.nsec, qos.avoid_ros_namespace_conventions != 0);
        }
        return output;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::mcap_storage
