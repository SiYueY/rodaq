#ifndef RODAQ_CANONICAL_MODEL_OPS_HPP_
#define RODAQ_CANONICAL_MODEL_OPS_HPP_

#include "canonical/model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace rodaq {

inline ArenaBytes copy_bytes(
    std::pmr::memory_resource* resource, const std::uint8_t* data, std::size_t size) {
    ArenaBytes result(resource);
    if (size != 0) {
        result.assign(data, data + size);
    }
    return result;
}

inline ArenaString copy_string(std::pmr::memory_resource* resource, std::string_view value) {
    ArenaString result(resource);
    result.assign(value.data(), value.size());
    return result;
}

inline bool bytes_equal(
    const ArenaBytes& stored, const std::uint8_t* data, std::size_t size) noexcept {
    return stored.size() == size && std::equal(stored.begin(), stored.end(), data);
}

inline bool strings_equal(const ArenaString& stored, std::string_view value) noexcept {
    return stored.size() == value.size() && std::equal(stored.begin(), stored.end(), value.begin());
}

}  // namespace rodaq

#endif  // RODAQ_CANONICAL_MODEL_OPS_HPP_
