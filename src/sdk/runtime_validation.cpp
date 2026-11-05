#include <rodaq/plugin/runtime_services.hpp>

namespace rodaq {

Result<void> runtime_result(rodaq_status status, const rodaq_error& error) {
    if (status == RODAQ_STATUS_OK) {
        return {};
    }
    return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error());
}

}  // namespace rodaq

namespace rodaq::plugin {

bool is_valid_source_registration(const rodaq_source_registration& request) noexcept {
    const std::uint8_t* source_id = nullptr;
    std::size_t source_id_size = 0;
    return request.struct_size >= RODAQ_SOURCE_REGISTRATION_MIN_STRUCT_SIZE &&
           request.reserved == 0 &&
           parse_abi_bytes(request.source_id, &source_id, &source_id_size) && source_id_size != 0;
}

bool is_valid_clock_registration(const rodaq_clock_registration& request) noexcept {
    const std::uint8_t* native_id = nullptr;
    std::size_t native_id_size = 0;
    std::string_view origin;
    return request.struct_size >= RODAQ_CLOCK_REGISTRATION_MIN_STRUCT_SIZE &&
           request.reserved == 0 &&
           parse_abi_bytes(request.native_id, &native_id, &native_id_size) && native_id_size != 0 &&
           request.resolution_numerator != 0 && request.resolution_denominator != 0 &&
           parse_abi_string(request.origin, &origin);
}

bool is_valid_schema_registration(const rodaq_schema_registration& request) noexcept {
    if (request.struct_size < RODAQ_SCHEMA_REGISTRATION_MIN_STRUCT_SIZE ||
        !is_valid_abi_bool(request.type_identifier_present) || request.reserved != 0) {
        return false;
    }
    std::string_view format;
    const std::uint8_t* representation = nullptr;
    std::size_t representation_size = 0;
    if (!parse_abi_string(request.format, &format) ||
        !parse_abi_bytes(request.representation, &representation, &representation_size)) {
        return false;
    }
    if (request.type_identifier_present == RODAQ_TRUE) {
        std::string_view type_identifier;
        return parse_abi_string(request.type_identifier, &type_identifier);
    }
    return true;
}

bool is_valid_stream_registration(const rodaq_stream_registration& request) noexcept {
    const std::uint8_t* native_id = nullptr;
    std::size_t native_id_size = 0;
    return request.struct_size >= RODAQ_STREAM_REGISTRATION_MIN_STRUCT_SIZE &&
           request.reserved == 0 &&
           parse_abi_bytes(request.native_id, &native_id, &native_id_size) && native_id_size != 0;
}

bool is_valid_stream_descriptor_registration(
    const rodaq_stream_descriptor_registration& request) noexcept {
    if (request.struct_size < RODAQ_STREAM_DESCRIPTOR_REGISTRATION_MIN_STRUCT_SIZE ||
        request.timestamp_key_count > kMaximumTransportTimestampKeys ||
        !is_valid_abi_bool(request.schema_present) || request.reserved != 0 ||
        request.stream.value == 0 || request.reserved2 != 0 ||
        (request.timestamp_key_count != 0 && request.timestamp_keys == nullptr)) {
        return false;
    }
    std::string_view name;
    std::string_view payload_encoding;
    if (!parse_abi_string(request.name, &name) ||
        !parse_abi_string(request.payload_encoding, &payload_encoding)) {
        return false;
    }
    if (request.schema_present == RODAQ_TRUE && request.schema.value == 0) {
        return false;
    }
    for (std::uint32_t index = 0; index < request.timestamp_key_count; ++index) {
        std::string_view role;
        if (!parse_abi_string(request.timestamp_keys[index].role, &role) ||
            request.timestamp_keys[index].clock.value == 0) {
            return false;
        }
    }
    return true;
}

bool is_valid_payload_input(const rodaq_payload_input& payload) noexcept {
    if (payload.reserved != 0 ||
        (payload.mode != RODAQ_PAYLOAD_COPY && payload.mode != RODAQ_PAYLOAD_ADOPT &&
         payload.mode != RODAQ_PAYLOAD_RETAIN)) {
        return false;
    }
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    if (!parse_abi_bytes(rodaq_bytes_view{payload.data, payload.size}, &data, &size)) {
        return false;
    }
    if (payload.mode == RODAQ_PAYLOAD_COPY) {
        return payload.owner_context == nullptr && payload.release == nullptr;
    }
    return payload.release != nullptr;
}

bool is_valid_record_submission(const rodaq_record_submission& submission) noexcept {
    if (submission.struct_size < RODAQ_RECORD_SUBMISSION_MIN_STRUCT_SIZE || submission.flags != 0 ||
        submission.reserved != 0 || submission.stream.value == 0 ||
        submission.descriptor.value == 0 || !is_valid_abi_bool(submission.source_present) ||
        !is_valid_abi_bool(submission.source_sequence_present)) {
        return false;
    }
    if (!submission.source_present && submission.source_sequence_present) {
        return false;
    }
    if (submission.source_present && submission.source.value == 0) {
        return false;
    }
    return is_valid_timestamp_values(
               submission.timestamp_values, submission.timestamp_value_count,
               kMaximumTransportTimestampKeys) &&
           is_valid_payload_input(submission.payload);
}

}  // namespace rodaq::plugin
