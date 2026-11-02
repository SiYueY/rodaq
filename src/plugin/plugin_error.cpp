#include "plugin/plugin_error.hpp"

#include <string>

namespace rodaq::plugin {

Result<void> result_from_status(
    rodaq_status status, const rodaq_error* status_error, std::string_view fallback_domain) {
    return rodaq::result_from_status(status, status_error, fallback_domain);
}

Error invalid_argument_error(std::string message) {
    return Error(ErrorCode::InvalidArgument, "plugin", 0, std::move(message));
}

Error internal_error(std::string message) {
    return Error(ErrorCode::InternalError, "plugin", 0, std::move(message));
}

}  // namespace rodaq::plugin
