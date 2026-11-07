#include "activation/activation_state.hpp"
#include "plugin/abi_conversion.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <string_view>

namespace rodaq {
namespace {

using Clock = std::chrono::steady_clock;

rodaq_status_code assign_status(const Status& error, rodaq_status* out_status) noexcept {
    assign_abi_status(error, out_status);
    return abi_status_from_status_code(error.code());
}

rodaq_status_code assign_message(
    StatusCode status_code, std::string_view message, rodaq_status* out_status) noexcept {
    assign_abi_status(abi_status_from_status_code(status_code), "plugin", 0, message, out_status);
    return abi_status_from_status_code(status_code);
}

ActivationState* state_from(void* context) noexcept {
    return static_cast<ActivationState*>(context);
}

LogLevel log_level_from_abi(rodaq_log_level level) noexcept {
    switch (level) {
        case RODAQ_ABI_LOG_TRACE:
            return LogLevel::Trace;
        case RODAQ_ABI_LOG_DEBUG:
            return LogLevel::Debug;
        case RODAQ_ABI_LOG_INFO:
            return LogLevel::Info;
        case RODAQ_ABI_LOG_WARN:
            return LogLevel::Warn;
        case RODAQ_ABI_LOG_ERROR:
            return LogLevel::Error;
        case RODAQ_ABI_LOG_CRITICAL:
            return LogLevel::Critical;
        case RODAQ_ABI_LOG_OFF:
        default:
            return LogLevel::Off;
    }
}

void common_log(
    void* context, rodaq_log_level level, rodaq_string_view category,
    rodaq_string_view message) noexcept {
    if (context == nullptr) {
        return;
    }
    std::string_view parsed_category;
    std::string_view parsed_message;
    if (parse_abi_string(category, &parsed_category) &&
        parse_abi_string(message, &parsed_message)) {
        state_from(context)->log(log_level_from_abi(level), parsed_category, parsed_message);
    }
}

std::uint64_t common_monotonic_time_ns(void*) noexcept {
    const auto now = Clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

rodaq_status_code common_lifecycle_snapshot(
    void* context, rodaq_runtime_lifecycle_snapshot* out_snapshot,
    rodaq_status* out_status) noexcept {
    if (context == nullptr || out_snapshot == nullptr) {
        return assign_message(
            StatusCode::InvalidArgument, "lifecycle snapshot input is invalid", out_status);
    }
    try {
        *out_snapshot = {};
        const auto result = state_from(context)->lifecycle_snapshot();
        if (!result.ok()) {
            return assign_status(result.status(), out_status);
        }
        const DAQLifecycleSnapshot& snapshot = result.value();
        std::uint32_t lifecycle = RODAQ_RUNTIME_CONFIGURED;
        switch (snapshot.state) {
            case DAQState::Constructed:
            case DAQState::Configured:
                lifecycle = RODAQ_RUNTIME_CONFIGURED;
                break;
            case DAQState::Starting:
                lifecycle = RODAQ_RUNTIME_STARTING;
                break;
            case DAQState::Running:
                lifecycle = RODAQ_RUNTIME_RUNNING;
                break;
            case DAQState::Stopping:
                lifecycle = RODAQ_RUNTIME_STOPPING;
                break;
            case DAQState::Stopped:
                lifecycle = RODAQ_RUNTIME_STOPPED;
                break;
            case DAQState::Failed:
                lifecycle = RODAQ_RUNTIME_FAILED;
                break;
        }
        *out_snapshot = rodaq_runtime_lifecycle_snapshot{
            lifecycle,
            snapshot.normal_acquisition_allowed ? RODAQ_TRUE : RODAQ_FALSE,
            snapshot.generation,
        };
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        return assign_message(StatusCode::InternalError, error.what(), out_status);
    } catch (...) {
        return assign_message(StatusCode::InternalError, "lifecycle snapshot failed", out_status);
    }
}

void common_diagnostic(
    void* context, rodaq_diagnostic_severity severity, rodaq_string_view code,
    rodaq_string_view message) noexcept {
    if (context == nullptr) {
        return;
    }
    std::string_view parsed_code;
    std::string_view parsed_message;
    if (parse_abi_string(code, &parsed_code) && parse_abi_string(message, &parsed_message)) {
        state_from(context)->diagnostic(severity, parsed_code, parsed_message);
    }
}

}  // namespace

const rodaq_runtime_common ActivationState::kCommonVtable = {
    RODAQ_RUNTIME_COMMON_PROTOCOL_VERSION,
    RODAQ_RUNTIME_COMMON_MIN_STRUCT_SIZE,
    &common_log,
    &common_monotonic_time_ns,
    &common_lifecycle_snapshot,
    &common_diagnostic,
};

}  // namespace rodaq
