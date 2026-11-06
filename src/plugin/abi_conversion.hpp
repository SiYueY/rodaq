#ifndef RODAQ_PLUGIN_ABI_CONVERSION_HPP_
#define RODAQ_PLUGIN_ABI_CONVERSION_HPP_

#include <rodaq/plugin/abi_support.hpp>

#include <algorithm>
#include <cstring>

namespace rodaq {

template <typename Struct>
Result<void> write_output_prefix(const Struct& output, Struct* destination) {
    if (destination == nullptr) {
        return Status(StatusCode::InvalidArgument, "plugin", 0, "ABI output pointer is null");
    }
    const std::uint32_t capacity = destination->struct_size;
    const std::uint32_t initialized = initialized_prefix_size<Struct>(capacity);
    std::memset(destination, 0, initialized);
    std::memcpy(destination, &output, initialized);
    destination->struct_size = initialized;
    return {};
}

inline Status status_from_abi_diagnostic(const rodaq_status& error) {
    return status_from_abi(error.status_code, &error, "plugin");
}

}  // namespace rodaq

#endif  // RODAQ_PLUGIN_ABI_CONVERSION_HPP_
