#include "plugin/plugin_status.hpp"

#include <string>

namespace rodaq::plugin {

Status invalid_argument_status(std::string message) {
    return Status(StatusCode::InvalidArgument, "plugin", 0, std::move(message));
}

Status internal_status(std::string message) {
    return Status(StatusCode::InternalError, "plugin", 0, std::move(message));
}

}  // namespace rodaq::plugin
