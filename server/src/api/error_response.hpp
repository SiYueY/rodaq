#pragma once

#include "../security/auth.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rodaq/status.hpp>
#include <rodaq/task/types.hpp>

namespace rodaq::server {

constexpr std::uint32_t kApiMajor = 1;
constexpr std::uint32_t kApiMinor = 0;
constexpr std::size_t kMaxErrorDetails = 32;
constexpr std::size_t kMaxDiagnosticBytes = 256;

struct ErrorDetail {
    std::string code;
    std::string path;
    std::string message;
};

struct ApiError {
    int http_status{500};
    std::string code;
    std::string message;
    Retryability retryability{Retryability::Never};
    FailureDomain failure_domain{FailureDomain::Request};
    DataImpact data_impact{DataImpact::None};
    std::vector<ErrorDetail> details;
    std::optional<std::uint32_t> retry_after_seconds;
};

std::string_view to_wire_string(Retryability value) noexcept;

std::string_view to_wire_string(FailureDomain value) noexcept;

std::string_view to_wire_string(DataImpact value) noexcept;

std::string_view to_wire_string(DesiredState value) noexcept;

std::string_view to_wire_string(ExecutionCondition value) noexcept;

std::string_view to_wire_string(OperationKind value) noexcept;

std::string_view to_wire_string(OperationState value) noexcept;

nlohmann::json make_api_version_json(ApiVersion version = {kApiMajor, kApiMinor});

nlohmann::json make_success_envelope(
    const std::string& request_id, nlohmann::json data,
    ApiVersion version = {kApiMajor, kApiMinor});

nlohmann::json make_error_envelope(
    const std::string& request_id, ApiError error, ApiVersion version = {kApiMajor, kApiMinor});

ApiError map_core_status(
    const Status& status, FailureDomain domain, bool is_operation_lookup = false);

ApiError make_request_error(
    int http_status, std::string code, std::string message, std::vector<ErrorDetail> details = {});

std::string sanitize_bounded_message(std::string_view raw);

}  // namespace rodaq::server
