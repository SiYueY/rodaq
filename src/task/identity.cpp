#include "identity.hpp"

#include "config.hpp"
#include "../persistence/json.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <new>
#include <limits>
#include <string>
#include <string_view>
#include <sys/random.h>
#include <utility>

#include <openssl/evp.h>

namespace rodaq {
namespace {

using Identity = std::array<std::uint8_t, 16>;

bool nonzero(const Identity& value) noexcept {
    return std::any_of(value.begin(), value.end(), [](std::uint8_t byte) { return byte != 0; });
}

Result<Identity> parse_identity(std::string_view text) {
    if (text.size() != 36) {
        return Status(StatusCode::InvalidArgument, {});
    }
    Identity value{};
    std::size_t output = 0;
    int high = -1;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (text[index] != '-') {
                return Status(StatusCode::InvalidArgument, {});
            }
            continue;
        }
        const char character = text[index];
        const int nibble = character >= '0' && character <= '9'   ? character - '0'
                           : character >= 'a' && character <= 'f' ? character - 'a' + 10
                                                                  : -1;
        if (nibble < 0) {
            return Status(StatusCode::InvalidArgument, {});
        }
        if (high < 0) {
            high = nibble;
        } else {
            value[output++] = static_cast<std::uint8_t>(high * 16 + nibble);
            high = -1;
        }
    }
    if (!nonzero(value)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return value;
}

std::string format_identity(const Identity& value) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            text += '-';
        }
        text += kHex[value[index] >> 4u];
        text += kHex[value[index] & 0xfu];
    }
    return text;
}

Result<Identity> new_identity() {
    Identity identity{};
    std::size_t filled = 0;
    while (filled < identity.size()) {
        const auto count = getrandom(identity.data() + filled, identity.size() - filled, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return Status(StatusCode::Unavailable, {});
        }
        filled += static_cast<std::size_t>(count);
    }
    identity[6] = static_cast<std::uint8_t>((identity[6] & 0x0fu) | 0x40u);
    identity[8] = static_cast<std::uint8_t>((identity[8] & 0x3fu) | 0x80u);
    return identity;
}

persistence::Json precondition_document(const ControlPrecondition& precondition) {
    if (!precondition.has_expected_control_generation) {
        return nullptr;
    }
    return std::to_string(precondition.expected_control_generation);
}

}  // namespace

Result<TaskId> parse_task_id(std::string_view text) {
    auto parsed = parse_identity(text);
    if (!parsed.ok()) {
        return std::move(parsed).status();
    }
    return TaskId{parsed.value()};
}

Result<OperationId> parse_operation_id(std::string_view text) {
    auto parsed = parse_identity(text);
    if (!parsed.ok()) {
        return std::move(parsed).status();
    }
    return OperationId{parsed.value()};
}

std::string to_string(TaskId task_id) { return format_identity(task_id.value); }

std::string to_string(OperationId operation_id) { return format_identity(operation_id.value); }

namespace task {

Result<TaskId> new_task_id() {
    auto created = new_identity();
    if (!created.ok()) {
        return std::move(created).status();
    }
    return TaskId{created.value()};
}

Result<OperationId> new_operation_id() {
    auto created = new_identity();
    if (!created.ok()) {
        return std::move(created).status();
    }
    return OperationId{created.value()};
}

Result<void> validate_request_identity(const RequestIdentity& request) {
    if (request.request_key.empty() || request.request_key.size() > 128 ||
        !std::all_of(
            request.request_key.begin(), request.request_key.end(),
            [](unsigned char byte) { return byte >= 0x21 && byte <= 0x7e; })) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return {};
}

Result<std::string> request_key_hash(const RequestIdentity& request) {
    auto valid = validate_request_identity(request);
    if (!valid.ok()) {
        return std::move(valid).status();
    }
    try {
        // Unlike request digest, request-key hash is over exact key bytes, not JCS.
        constexpr char kDomain[] = "rodaq.request-key.v1";
        std::string input(kDomain, sizeof(kDomain));
        input += request.request_key;
        std::array<unsigned char, 32> bytes{};
        unsigned int size = 0;
        if (EVP_Digest(input.data(), input.size(), bytes.data(), &size, EVP_sha256(), nullptr) !=
                1 ||
            size != bytes.size()) {
            return Status(StatusCode::Internal, {});
        }
        constexpr std::string_view kHex = "0123456789abcdef";
        std::string output = "sha256:";
        output.reserve(71);
        for (const auto byte : bytes) {
            output += kHex[byte >> 4u];
            output += kHex[byte & 0xfu];
        }
        return output;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::string> create_request_digest(
    const TaskConfig& config, const TaskConfigLimits& limits) {
    auto document = config_document(config, limits);
    if (!document.ok()) {
        return std::move(document).status();
    }
    const auto bounds = config_json_limits(limits);
    return persistence::semantic_digest(document.value(), "rodaq.task-create-request.v1", bounds);
}

Result<std::string> configure_request_digest(
    TaskId task_id, const TaskConfig& config, const ControlPrecondition& precondition,
    const TaskConfigLimits& limits) {
    if (!nonzero(task_id.value)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto document = config_document(config, limits);
    if (!document.ok()) {
        return std::move(document).status();
    }
    try {
        const persistence::Json request{
            {"task_id", to_string(task_id)},
            {"config", std::move(document).value()},
            {"expected_control_generation", precondition_document(precondition)}};
        auto bounds = config_json_limits(limits);
        // The fixed request envelope has a separate bounded overhead. A legal
        // config must not lose its last bytes, nodes or depth to that envelope.
        if (bounds.max_bytes > std::numeric_limits<std::size_t>::max() - 256 ||
            bounds.max_nodes > std::numeric_limits<std::size_t>::max() - 4 ||
            bounds.max_depth > 256) {
            return Status(StatusCode::InvalidArgument, {});
        }
        bounds.max_bytes += 256;
        bounds.max_nodes += 4;
        bounds.max_depth += 1;
        bounds.max_string_bytes = std::max<std::size_t>(bounds.max_string_bytes, 36);
        bounds.max_object_members = std::max<std::size_t>(bounds.max_object_members, 3);
        return persistence::semantic_digest(request, "rodaq.task-configure-request.v1", bounds);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::string> lifecycle_request_digest(
    TaskId task_id, OperationKind kind, const ControlPrecondition& precondition) {
    if (!nonzero(task_id.value)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const char* operation = nullptr;
    switch (kind) {
        case OperationKind::Start:
            operation = "start";
            break;
        case OperationKind::Pause:
            operation = "pause";
            break;
        case OperationKind::Resume:
            operation = "resume";
            break;
        case OperationKind::Stop:
            operation = "stop";
            break;
        case OperationKind::Reset:
            operation = "reset";
            break;
        case OperationKind::Retire:
            operation = "retire";
            break;
        case OperationKind::Configure:
            return Status(StatusCode::InvalidArgument, {});
    }
    if (operation == nullptr) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        const persistence::Json request{
            {"task_id", to_string(task_id)},
            {"operation_kind", operation},
            {"parameters", persistence::Json::object()},
            {"expected_control_generation", precondition_document(precondition)}};
        return persistence::semantic_digest(request, "rodaq.task-lifecycle-request.v1");
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace task
}  // namespace rodaq
