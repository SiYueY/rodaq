#include <rodaq/plugin/storage_plugin.hpp>

namespace rodaq::plugin {

bool is_valid_storage_open_request(const rodaq_storage_open_request& request) noexcept {
    if (request.struct_size < RODAQ_STORAGE_OPEN_REQUEST_MIN_STRUCT_SIZE || request.flags != 0) {
        return false;
    }
    std::string_view session_id;
    std::string_view target_uri;
    return parse_abi_string(request.session_id, &session_id) &&
           parse_abi_string(request.target_uri, &target_uri);
}

bool is_valid_storage_submission(const rodaq_storage_submission& submission) noexcept {
    if (submission.struct_size < RODAQ_STORAGE_SUBMISSION_MIN_STRUCT_SIZE ||
        submission.flags != 0 || submission.reserved != 0 || submission.operation.value == 0 ||
        submission.stream.value == 0 || submission.descriptor.value == 0 ||
        !rodaq::is_valid_abi_bool(submission.source_present) ||
        !rodaq::is_valid_abi_bool(submission.source_sequence_present)) {
        return false;
    }
    if (!submission.source_present && submission.source_sequence_present) {
        return false;
    }
    if (submission.source_present && submission.source.value == 0) {
        return false;
    }
    if (!rodaq::is_valid_timestamp_values(
            submission.timestamp_values, submission.timestamp_value_count,
            kMaximumCanonicalTimestampSlots)) {
        return false;
    }
    const std::uint8_t* payload_data = nullptr;
    std::size_t payload_size = 0;
    return parse_abi_bytes(submission.payload, &payload_data, &payload_size);
}

bool is_valid_storage_submit_result(const rodaq_storage_submit_result& result) noexcept {
    if (result.struct_size < RODAQ_STORAGE_SUBMIT_RESULT_MIN_STRUCT_SIZE || result.reserved != 0 ||
        (result.payload_state != RODAQ_STORAGE_PAYLOAD_PENDING &&
         result.payload_state != RODAQ_STORAGE_PAYLOAD_CONSUMED)) {
        return false;
    }
    const bool pending = result.persistence_state == RODAQ_STORAGE_PERSISTENCE_PENDING;
    const bool succeeded = result.persistence_state == RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
    const bool failed = result.persistence_state == RODAQ_STORAGE_PERSISTENCE_FAILED;
    const bool unknown = result.persistence_state == RODAQ_STORAGE_PERSISTENCE_UNKNOWN;
    if (!pending && !succeeded && !failed && !unknown) {
        return false;
    }
    if (!rodaq::is_valid_abi_status(result.persistence_error)) {
        return false;
    }
    if (pending || succeeded) {
        return result.persistence_error.status_code == RODAQ_STATUS_OK &&
               result.persistence_error.flags == 0 && result.persistence_error.error_code == 0 &&
               result.persistence_error.domain_size == 0 &&
               result.persistence_error.message_size == 0;
    }
    return result.persistence_error.status_code != RODAQ_STATUS_OK &&
           result.persistence_error.message_size != 0;
}

void write_storage_capabilities(
    rodaq_storage_capabilities* output, std::uint32_t capacity,
    const rodaq_storage_capabilities& value) noexcept {
    const std::uint32_t initialized = clear_output_prefix(output, capacity);
    output->struct_size = initialized;
    output->flags = 0;
    output->full_fidelity = value.full_fidelity;
    output->reserved = 0;
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_storage_capabilities, capacity, maximum_payload_bytes)) {
        output->maximum_payload_bytes = value.maximum_payload_bytes;
    }
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_storage_capabilities, capacity, maximum_inflight_records)) {
        output->maximum_inflight_records = value.maximum_inflight_records;
    }
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_storage_capabilities, capacity, maximum_inflight_bytes)) {
        output->maximum_inflight_bytes = value.maximum_inflight_bytes;
    }
    if (RODAQ_HAS_FIELD_CAPACITY(
            rodaq_storage_capabilities, capacity, handoff_progress_timeout_ns)) {
        output->handoff_progress_timeout_ns = value.handoff_progress_timeout_ns;
    }
    if (RODAQ_HAS_FIELD_CAPACITY(
            rodaq_storage_capabilities, capacity, descriptor_progress_timeout_ns)) {
        output->descriptor_progress_timeout_ns = value.descriptor_progress_timeout_ns;
    }
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_storage_capabilities, capacity, flush_progress_timeout_ns)) {
        output->flush_progress_timeout_ns = value.flush_progress_timeout_ns;
    }
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_storage_capabilities, capacity, close_progress_timeout_ns)) {
        output->close_progress_timeout_ns = value.close_progress_timeout_ns;
    }
}

void write_storage_submit_result(
    rodaq_storage_submit_result* output, std::uint32_t capacity,
    const rodaq_storage_submit_result& value) noexcept {
    const std::uint32_t initialized = clear_output_prefix(output, capacity);
    output->struct_size = initialized;
    output->reserved = 0;
    output->payload_state = value.payload_state;
    output->persistence_state = value.persistence_state;
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_storage_submit_result, capacity, persistence_error)) {
        output->persistence_error = value.persistence_error;
    }
}

void write_descriptor_readiness_result(
    rodaq_descriptor_readiness_result* output, std::uint32_t capacity,
    const rodaq_descriptor_readiness_result& value) noexcept {
    const std::uint32_t initialized = clear_output_prefix(output, capacity);
    output->struct_size = initialized;
    output->state = value.state;
    if (RODAQ_HAS_FIELD_CAPACITY(rodaq_descriptor_readiness_result, capacity, failure)) {
        output->failure = value.failure;
    }
}

}  // namespace rodaq::plugin
