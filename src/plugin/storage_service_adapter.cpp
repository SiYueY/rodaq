#include "activation/activation_state.hpp"
#include "activation/activation_error.hpp"
#include "plugin/abi_conversion.hpp"
#include "plugin/service_context.hpp"

#include <exception>
#include <utility>

namespace rodaq {
namespace {

ActivationState* state_from(void* context) noexcept {
    return static_cast<ActivationState*>(context);
}

const plugin::StorageServiceContext* context_from(void* context) noexcept {
    return static_cast<const plugin::StorageServiceContext*>(context);
}

void storage_payload_consumed(void* context, rodaq_storage_operation operation) noexcept {
    if (context == nullptr) {
        return;
    }
    try {
        auto* state = state_from(context_from(context)->state);
        if (state->pipeline() != nullptr) {
            state->pipeline()->payload_consumed(operation);
        }
    } catch (...) {
    }
}

void storage_persistence_completed(
    void* context, rodaq_storage_operation operation, rodaq_storage_persistence_state state_value,
    const rodaq_status* error) noexcept {
    if (context == nullptr) {
        return;
    }
    try {
        auto* state = state_from(context_from(context)->state);
        if (error != nullptr &&
            (!is_valid_abi_status(*error) || error->status_code == RODAQ_STATUS_OK)) {
            state->request_failure(activation_status(
                StatusCode::InvalidArgument,
                "Storage persistence callback supplied a malformed diagnostic"));
            return;
        }
        if (state->pipeline() != nullptr) {
            state->pipeline()->persistence_completed(operation, state_value, error);
        }
    } catch (...) {
    }
}

void storage_report_failure(void* context, const rodaq_status* error) noexcept {
    if (context == nullptr) {
        return;
    }
    try {
        auto* state = state_from(context_from(context)->state);
        if (error != nullptr &&
            (!is_valid_abi_status(*error) || error->status_code == RODAQ_STATUS_OK)) {
            state->request_failure(activation_status(
                StatusCode::InvalidArgument,
                "Storage failure callback supplied a malformed diagnostic"));
            return;
        }
        Status failure =
            error == nullptr
                ? activation_status(StatusCode::StorageError, "Storage reported failure")
                : status_from_abi_diagnostic(*error);
        state->request_failure(std::move(failure));
    } catch (...) {
    }
}

}  // namespace

const rodaq_runtime_storage ActivationState::kStorageVtable = {
    RODAQ_RUNTIME_STORAGE_PROTOCOL_VERSION,
    RODAQ_RUNTIME_STORAGE_MIN_STRUCT_SIZE,
    &storage_payload_consumed,
    &storage_persistence_completed,
    &storage_report_failure,
};

}  // namespace rodaq
