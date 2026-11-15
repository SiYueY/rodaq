#include "instance_id.hpp"

#include "../uuid.hpp"

#include <utility>

namespace rodaq::pipeline {

Result<InstanceId> new_instance_id() {
    auto created = uuid::generate();
    if (!created.ok()) {
        return std::move(created).status();
    }
    return InstanceId{created.value()};
}

Result<InstanceId> parse_instance_id(std::string_view text) {
    auto parsed = uuid::parse(text);
    if (!parsed.ok()) {
        return std::move(parsed).status();
    }
    return InstanceId{parsed.value()};
}

std::string to_string(InstanceId id) { return uuid::format(id.value); }

bool valid_instance_id(InstanceId id) noexcept { return uuid::nonzero(id.value); }

}  // namespace rodaq::pipeline
