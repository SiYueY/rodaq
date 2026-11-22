#pragma once

#include <string_view>

#include <rodaq/result.hpp>

namespace rodaq::mcap_storage {

// V1 has one qualified option set. Missing optional fields select that set;
// accepting another libmcap option would require a separately qualified profile.
Result<void> validate_config(std::string_view input);

}  // namespace rodaq::mcap_storage
