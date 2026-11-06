#include "canonical/canonical_state.hpp"

#include "canonical/model_ops.hpp"

#include <rodaq/plugin/abi_support.hpp>

#include <atomic>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace rodaq {
namespace {

std::uint32_t registry_generation(std::uint64_t activation_generation) noexcept {
    const auto generation = static_cast<std::uint32_t>(activation_generation);
    return generation == 0 ? 1U : generation;
}

Status canonical_status(StatusCode status_code, std::string message) {
    return Status(status_code, "canonical", 0, std::move(message));
}

template <typename T, typename HandleType>
Registry<T, HandleType> make_registry(std::size_t capacity, std::uint32_t generation) {
    auto result = Registry<T, HandleType>::create(capacity, generation);
    if (!result.ok()) {
        throw std::runtime_error(std::string(result.status().message()));
    }
    return std::move(result.value());
}

}  // namespace

CanonicalState::CanonicalState(
    const DAQConfig& config, std::uint64_t activation_generation, std::pmr::memory_resource* arena)
: arena_(arena),
  sources_(make_registry<SourceEntry, SourceHandle>(
      config.resources.max_sources, registry_generation(activation_generation))),
  clocks_(make_registry<ClockEntry, ClockHandle>(
      config.resources.max_clocks, registry_generation(activation_generation))),
  schemas_(make_registry<SchemaEntry, SchemaHandle>(
      config.resources.max_schemas, registry_generation(activation_generation))),
  timestamp_key_sets_(make_registry<TimestampKeySetEntry, TimestampKeySetHandle>(
      config.resources.max_timestamp_key_sets, registry_generation(activation_generation))),
  streams_(make_registry<StreamEntry, StreamHandle>(
      config.resources.max_streams, registry_generation(activation_generation))),
  descriptors_(make_registry<StreamDescriptorEntry, StreamDescriptorHandle>(
      config.resources.max_stream_descriptors, registry_generation(activation_generation))) {
    source_handles_.reserve(config.resources.max_sources);
    clock_handles_.reserve(config.resources.max_clocks);
    schema_handles_.reserve(config.resources.max_schemas);
    timestamp_key_set_handles_.reserve(config.resources.max_timestamp_key_sets);
    stream_handles_.reserve(config.resources.max_streams);
    descriptor_handles_.reserve(config.resources.max_stream_descriptors);
}

Result<SourceHandle> CanonicalState::register_source(
    std::size_t plugin_index, const std::uint8_t* source_id, std::size_t source_id_size) {
    for (SourceHandle existing : source_handles_) {
        const SourceEntry* entry = sources_.find(existing);
        if (entry != nullptr && bytes_equal(entry->source_id, source_id, source_id_size)) {
            if (entry->plugin_index != plugin_index) {
                return canonical_status(
                    StatusCode::Conflict, "Source identity belongs to another Plugin");
            }
            return existing;
        }
    }

    auto availability = std::make_unique<std::atomic<std::uint32_t>>(RODAQ_AVAILABILITY_AVAILABLE);
    auto inserted = sources_.insert(SourceEntry{
        copy_bytes(arena_, source_id, source_id_size),
        plugin_index,
        std::move(availability),
    });
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    source_handles_.push_back(inserted.value());
    return inserted.value();
}

Result<ClockHandle> CanonicalState::register_clock(
    std::size_t plugin_index, const rodaq_clock_registration& request) {
    std::string_view origin;
    static_cast<void>(parse_abi_string(request.origin, &origin));
    const std::size_t native_size = static_cast<std::size_t>(request.native_id.size);
    for (ClockHandle existing : clock_handles_) {
        const ClockEntry* entry = clocks_.find(existing);
        if (entry == nullptr || entry->plugin_index != plugin_index ||
            !bytes_equal(entry->native_id, request.native_id.data, native_size)) {
            continue;
        }
        if (entry->resolution_numerator != request.resolution_numerator ||
            entry->resolution_denominator != request.resolution_denominator ||
            !strings_equal(entry->origin, origin)) {
            return canonical_status(StatusCode::Conflict, "Clock registration facts conflict");
        }
        return existing;
    }

    const std::uint64_t clock_id = allocate_clock_id();
    auto inserted = clocks_.insert(ClockEntry{
        copy_bytes(arena_, request.native_id.data, native_size),
        clock_id,
        request.resolution_numerator,
        request.resolution_denominator,
        copy_string(arena_, origin),
        plugin_index,
    });
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    clock_handles_.push_back(inserted.value());
    return inserted.value();
}

Result<SchemaHandle> CanonicalState::register_schema(const rodaq_schema_registration& request) {
    std::string_view format;
    std::string_view type_identifier;
    static_cast<void>(parse_abi_string(request.format, &format));
    if (request.type_identifier_present == RODAQ_TRUE) {
        static_cast<void>(parse_abi_string(request.type_identifier, &type_identifier));
    }
    const std::size_t representation_size = static_cast<std::size_t>(request.representation.size);
    for (SchemaHandle existing : schema_handles_) {
        const SchemaEntry* entry = schemas_.find(existing);
        if (entry == nullptr || !strings_equal(entry->format, format) ||
            entry->type_identifier_present != (request.type_identifier_present == RODAQ_TRUE) ||
            !strings_equal(entry->type_identifier, type_identifier) ||
            !bytes_equal(entry->representation, request.representation.data, representation_size)) {
            continue;
        }
        return existing;
    }

    auto inserted = schemas_.insert(SchemaEntry{
        allocate_schema_id(),
        copy_string(arena_, format),
        request.type_identifier_present == RODAQ_TRUE,
        copy_string(arena_, type_identifier),
        copy_bytes(arena_, request.representation.data, representation_size),
    });
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    schema_handles_.push_back(inserted.value());
    return inserted.value();
}

Result<StreamHandle> CanonicalState::register_stream(
    std::size_t plugin_index, const std::uint8_t* native_id, std::size_t native_id_size) {
    for (StreamHandle existing : stream_handles_) {
        const StreamEntry* entry = streams_.find(existing);
        if (entry != nullptr && entry->plugin_index == plugin_index &&
            bytes_equal(entry->native_id, native_id, native_id_size)) {
            return existing;
        }
    }

    auto availability = std::make_unique<std::atomic<std::uint32_t>>(RODAQ_AVAILABILITY_AVAILABLE);
    auto inserted = streams_.insert(StreamEntry{
        allocate_stream_id(),
        plugin_index,
        copy_bytes(arena_, native_id, native_id_size),
        std::move(availability),
    });
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    stream_handles_.push_back(inserted.value());
    return inserted.value();
}

Result<void> CanonicalState::get_source(SourceHandle handle, rodaq_source_info& info) const {
    const SourceEntry* entry = sources_.find(handle);
    if (entry == nullptr) {
        return canonical_status(StatusCode::NotFound, "Source handle was not found");
    }
    info.struct_size = sizeof(info);
    info.handle = rodaq_source_handle{handle.value()};
    info.source_id = rodaq_bytes_view{entry->source_id.data(), entry->source_id.size()};
    return {};
}

Result<void> CanonicalState::get_clock(ClockHandle handle, rodaq_clock_info& info) const {
    const ClockEntry* entry = clocks_.find(handle);
    if (entry == nullptr) {
        return canonical_status(StatusCode::NotFound, "Clock handle was not found");
    }
    info.struct_size = sizeof(info);
    info.handle = rodaq_clock_handle{handle.value()};
    info.clock_id = entry->clock_id;
    info.resolution_numerator = entry->resolution_numerator;
    info.resolution_denominator = entry->resolution_denominator;
    info.origin = rodaq_string_view{entry->origin.data(), entry->origin.size()};
    return {};
}

Result<void> CanonicalState::get_schema(SchemaHandle handle, rodaq_schema_info& info) const {
    const SchemaEntry* entry = schemas_.find(handle);
    if (entry == nullptr) {
        return canonical_status(StatusCode::NotFound, "Schema handle was not found");
    }
    info.struct_size = sizeof(info);
    info.type_identifier_present = entry->type_identifier_present ? RODAQ_TRUE : RODAQ_FALSE;
    info.handle = rodaq_schema_handle{handle.value()};
    info.schema_id = entry->schema_id;
    info.format = rodaq_string_view{entry->format.data(), entry->format.size()};
    info.type_identifier =
        rodaq_string_view{entry->type_identifier.data(), entry->type_identifier.size()};
    info.representation =
        rodaq_bytes_view{entry->representation.data(), entry->representation.size()};
    return {};
}

Result<void> CanonicalState::get_timestamp_key_set(
    std::uint64_t id, rodaq_timestamp_key_set_info& info) const {
    for (TimestampKeySetHandle handle : timestamp_key_set_handles_) {
        const TimestampKeySetEntry* entry = timestamp_key_sets_.find(handle);
        if (entry == nullptr || entry->timestamp_key_set_id != id) {
            continue;
        }
        info.struct_size = sizeof(info);
        info.timestamp_key_set_id = id;
        info.keys = entry->abi_keys.data();
        info.key_count = static_cast<std::uint32_t>(entry->abi_keys.size());
        return {};
    }
    return canonical_status(StatusCode::NotFound, "TimestampKeySet was not found");
}

Result<void> CanonicalState::get_stream(StreamHandle handle, rodaq_stream_info& info) const {
    const StreamEntry* entry = streams_.find(handle);
    if (entry == nullptr) {
        return canonical_status(StatusCode::NotFound, "Stream handle was not found");
    }
    info.struct_size = sizeof(info);
    info.handle = rodaq_stream_handle{handle.value()};
    info.stream_id = entry->stream_id;
    return {};
}

Result<void> CanonicalState::get_stream_descriptor(
    StreamDescriptorHandle handle, rodaq_stream_descriptor_info& info) const {
    const StreamDescriptorEntry* entry = descriptors_.find(handle);
    if (entry == nullptr) {
        return canonical_status(StatusCode::NotFound, "StreamDescriptor handle was not found");
    }
    info.struct_size = sizeof(info);
    info.schema_present = entry->schema_present ? RODAQ_TRUE : RODAQ_FALSE;
    info.handle = rodaq_stream_descriptor_handle{handle.value()};
    info.stream = rodaq_stream_handle{entry->stream.value()};
    info.schema = rodaq_schema_handle{entry->schema.value()};
    info.stream_descriptor_id = entry->stream_descriptor_id;
    const StreamEntry* stream = streams_.find(entry->stream);
    info.stream_id = stream == nullptr ? 0 : stream->stream_id;
    info.name = rodaq_string_view{entry->name.data(), entry->name.size()};
    info.schema_id = entry->schema_id;
    info.payload_encoding =
        rodaq_string_view{entry->payload_encoding.data(), entry->payload_encoding.size()};
    info.timestamp_key_set_id = entry->timestamp_key_set_id;
    info.transport_timestamp_keys = entry->transport_timestamp_keys.data();
    info.transport_timestamp_key_count =
        static_cast<std::uint32_t>(entry->transport_timestamp_keys.size());
    return {};
}

Result<void> CanonicalState::report_source_availability(
    std::size_t plugin_index, SourceHandle source, rodaq_availability_state availability) {
    SourceEntry* entry = sources_.find(source);
    if (entry == nullptr) {
        return canonical_status(StatusCode::NotFound, "Source handle is unknown");
    }
    if (entry->plugin_index != plugin_index) {
        return canonical_status(StatusCode::InvalidState, "Source belongs to another Plugin");
    }
    if (availability != RODAQ_AVAILABILITY_AVAILABLE &&
        availability != RODAQ_AVAILABILITY_UNAVAILABLE) {
        return canonical_status(StatusCode::InvalidArgument, "Source availability is invalid");
    }
    entry->availability->store(availability);
    return {};
}

Result<void> CanonicalState::report_stream_availability(
    std::size_t plugin_index, StreamHandle stream, rodaq_availability_state availability) {
    StreamEntry* entry = streams_.find(stream);
    if (entry == nullptr) {
        return canonical_status(StatusCode::NotFound, "Stream handle is unknown");
    }
    if (entry->plugin_index != plugin_index) {
        return canonical_status(StatusCode::InvalidState, "Stream belongs to another Plugin");
    }
    if (availability != RODAQ_AVAILABILITY_AVAILABLE &&
        availability != RODAQ_AVAILABILITY_UNAVAILABLE) {
        return canonical_status(StatusCode::InvalidArgument, "Stream availability is invalid");
    }
    entry->availability->store(availability);
    return {};
}

}  // namespace rodaq
