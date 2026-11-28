#include "identity.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "../uuid.hpp"

namespace rodaq {

Result<TaskId> parse_task_id(std::string_view text) {
    auto parsed = uuid::parse(text);
    if (!parsed.ok()) {
        return std::move(parsed).status();
    }
    return TaskId{parsed.value()};
}

Result<OperationId> parse_operation_id(std::string_view text) {
    auto parsed = uuid::parse(text);
    if (!parsed.ok()) {
        return std::move(parsed).status();
    }
    return OperationId{parsed.value()};
}

std::string to_string(TaskId task_id) { return uuid::format(task_id.value); }

std::string to_string(OperationId operation_id) { return uuid::format(operation_id.value); }

namespace task {

Result<TaskId> new_task_id() {
    auto created = uuid::generate();
    if (!created.ok()) {
        return std::move(created).status();
    }
    return TaskId{created.value()};
}

Result<OperationId> new_operation_id() {
    auto created = uuid::generate();
    if (!created.ok()) {
        return std::move(created).status();
    }
    return OperationId{created.value()};
}

Result<std::string> new_config_version() {
    auto created = uuid::generate();
    if (!created.ok()) {
        return std::move(created).status();
    }
    return uuid::format(created.value());
}

Result<void> validate_request_identity(const RequestIdentity& request) {
    if (request.request_key.empty() || request.request_key.size() > 128 ||
        !std::all_of(
            request.request_key.begin(), request.request_key.end(),
            [](unsigned char byte) { return byte >= 0x21 && byte <= 0x7e; })) {
        return Status(StatusCode::InvalidArgument, "Invalid request_key");
    }
    return {};
}

}  // namespace task
}  // namespace rodaq
