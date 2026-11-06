#include "activation/activation_internal.hpp"
#include "plugin/acquisition_plugin_instance.hpp"
#include <rodaq/plugin/services.hpp>

namespace rodaq {

Result<void> ActivationState::initialize_ingress_clock() {
    static constexpr std::string_view kNativeId = "rodaq/activation/ingress";
    static constexpr std::string_view kOrigin = "monotonic_since_boot";

    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (ingress_clock_.valid()) {
            return activation_status(
                StatusCode::InvalidState, "DAQ ingress Clock is already created");
        }
        auto inserted = clocks_.insert(ClockEntry{
            copy_bytes(
                arena_->resource(), reinterpret_cast<const std::uint8_t*>(kNativeId.data()),
                kNativeId.size()),
            next_clock_id_++,
            1,
            1'000'000'000,
            copy_string(arena_->resource(), kOrigin),
            kRuntimeClockPluginIndex,
        });
        if (!inserted.ok()) {
            return std::move(inserted).status();
        }
        ingress_clock_ = inserted.value();
        clock_handles_.push_back(ingress_clock_);
    }

    auto correlation = ClockCorrelation::establish(
        rodaq_clock_handle{ingress_clock_.value()},
        []() -> std::int64_t {
            auto reading = capture_ingress_reading();
            if (!reading.ok()) {
                throw std::runtime_error(std::string(reading.status().message()));
            }
            return reading.value();
        },
        []() -> std::uint64_t {
            const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
            if (count < 0) {
                throw std::runtime_error("System clock is before Unix epoch");
            }
            return static_cast<std::uint64_t>(count);
        });
    if (!correlation.ok()) {
        return std::move(correlation).status();
    }
    ingress_clock_correlation_.emplace(std::move(correlation.value()));
    return {};
}

Result<void> ActivationState::register_source(
    std::size_t plugin_index, const rodaq_source_registration& request, SourceHandle& handle) {
    if (!rodaq::plugin::is_valid_source_registration(request) ||
        plugin_index >= acquisition_health_.size()) {
        return activation_status(StatusCode::InvalidArgument, "Source registration is invalid");
    }
    const auto* source_id = request.source_id.data;
    const std::size_t source_id_size = static_cast<std::size_t>(request.source_id.size);

    std::lock_guard<std::mutex> lock(control_mutex_);
    const DAQState current_state = state_.load();
    if (current_state != DAQState::Starting && current_state != DAQState::Running) {
        return activation_status(StatusCode::InvalidState, "Source registration is not allowed");
    }
    if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
        RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
        return activation_status(
            StatusCode::InvalidState, "Source registration requires an available Plugin");
    }
    for (SourceHandle existing : source_handles_) {
        const SourceEntry* entry = sources_.find(existing);
        if (entry != nullptr && bytes_equal(entry->source_id, source_id, source_id_size)) {
            if (entry->plugin_index != plugin_index) {
                return activation_status(
                    StatusCode::Conflict, "Source identity belongs to another Plugin");
            }
            handle = existing;
            return {};
        }
    }

    auto availability = std::make_unique<std::atomic<std::uint32_t>>(RODAQ_AVAILABILITY_AVAILABLE);
    auto inserted = sources_.insert(SourceEntry{
        copy_bytes(arena_->resource(), source_id, source_id_size), plugin_index,
        std::move(availability)});
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    source_handles_.push_back(inserted.value());
    handle = inserted.value();
    return {};
}

Result<void> ActivationState::register_clock(
    std::size_t plugin_index, const rodaq_clock_registration& request, ClockHandle& handle) {
    if (!rodaq::plugin::is_valid_clock_registration(request) ||
        plugin_index >= options_.acquisitions.size()) {
        return activation_status(StatusCode::InvalidArgument, "Clock registration is invalid");
    }
    std::string_view origin;
    static_cast<void>(parse_abi_string(request.origin, &origin));
    const std::size_t native_size = static_cast<std::size_t>(request.native_id.size);

    std::lock_guard<std::mutex> lock(control_mutex_);
    const DAQState current_state = state_.load();
    if (current_state != DAQState::Starting && current_state != DAQState::Running) {
        return activation_status(StatusCode::InvalidState, "Clock registration is not allowed");
    }
    if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
        RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
        return activation_status(
            StatusCode::InvalidState, "Clock registration requires an available Plugin");
    }
    for (ClockHandle existing : clock_handles_) {
        const ClockEntry* entry = clocks_.find(existing);
        if (entry == nullptr || entry->plugin_index != plugin_index ||
            !bytes_equal(entry->native_id, request.native_id.data, native_size)) {
            continue;
        }
        if (entry->resolution_numerator != request.resolution_numerator ||
            entry->resolution_denominator != request.resolution_denominator ||
            !strings_equal(entry->origin, origin)) {
            return activation_status(StatusCode::Conflict, "Clock registration facts conflict");
        }
        handle = existing;
        return {};
    }

    const std::uint64_t clock_id = next_clock_id_++;
    auto inserted = clocks_.insert(ClockEntry{
        copy_bytes(arena_->resource(), request.native_id.data, native_size),
        clock_id,
        request.resolution_numerator,
        request.resolution_denominator,
        copy_string(arena_->resource(), origin),
        plugin_index,
    });
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    clock_handles_.push_back(inserted.value());
    handle = inserted.value();
    return {};
}

Result<void> ActivationState::register_schema(
    std::size_t plugin_index, const rodaq_schema_registration& request, SchemaHandle& handle) {
    if (!rodaq::plugin::is_valid_schema_registration(request) ||
        plugin_index >= acquisition_health_.size()) {
        return activation_status(StatusCode::InvalidArgument, "Schema registration is invalid");
    }
    std::string_view format;
    std::string_view type_identifier;
    parse_abi_string(request.format, &format);
    if (request.type_identifier_present == RODAQ_TRUE) {
        parse_abi_string(request.type_identifier, &type_identifier);
    }
    const std::size_t representation_size = static_cast<std::size_t>(request.representation.size);

    std::lock_guard<std::mutex> lock(control_mutex_);
    const DAQState current_state = state_.load();
    if (current_state != DAQState::Starting && current_state != DAQState::Running) {
        return activation_status(StatusCode::InvalidState, "Schema registration is not allowed");
    }
    if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
        RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
        return activation_status(
            StatusCode::InvalidState, "Schema registration requires an available Plugin");
    }
    for (SchemaHandle existing : schema_handles_) {
        const SchemaEntry* entry = schemas_.find(existing);
        if (entry == nullptr || !strings_equal(entry->format, format) ||
            entry->type_identifier_present != (request.type_identifier_present == RODAQ_TRUE) ||
            !strings_equal(entry->type_identifier, type_identifier) ||
            !bytes_equal(entry->representation, request.representation.data, representation_size)) {
            continue;
        }
        handle = existing;
        return {};
    }

    const std::uint64_t schema_id = next_schema_id_++;
    auto inserted = schemas_.insert(SchemaEntry{
        schema_id,
        copy_string(arena_->resource(), format),
        request.type_identifier_present == RODAQ_TRUE,
        copy_string(arena_->resource(), type_identifier),
        copy_bytes(arena_->resource(), request.representation.data, representation_size),
    });
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    schema_handles_.push_back(inserted.value());
    handle = inserted.value();
    return {};
}

Result<void> ActivationState::register_stream(
    std::size_t plugin_index, const rodaq_stream_registration& request, StreamHandle& handle) {
    if (!rodaq::plugin::is_valid_stream_registration(request) ||
        plugin_index >= options_.acquisitions.size()) {
        return activation_status(StatusCode::InvalidArgument, "Stream registration is invalid");
    }
    const std::size_t native_size = static_cast<std::size_t>(request.native_id.size);

    std::lock_guard<std::mutex> lock(control_mutex_);
    const DAQState current_state = state_.load();
    if (current_state != DAQState::Starting && current_state != DAQState::Running) {
        return activation_status(StatusCode::InvalidState, "Stream registration is not allowed");
    }
    if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
        RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
        return activation_status(
            StatusCode::InvalidState, "Stream registration requires an available Plugin");
    }
    for (StreamHandle existing : stream_handles_) {
        const StreamEntry* entry = streams_.find(existing);
        if (entry != nullptr && entry->plugin_index == plugin_index &&
            bytes_equal(entry->native_id, request.native_id.data, native_size)) {
            handle = existing;
            return {};
        }
    }

    auto availability = std::make_unique<std::atomic<std::uint32_t>>(RODAQ_AVAILABILITY_AVAILABLE);
    auto current_binding = std::make_unique<std::atomic<std::uint64_t>>(0);
    auto admission_mutex = std::make_unique<std::mutex>();
    auto binding_transition_mutex = std::make_unique<std::mutex>();
    auto binding_transition_condition = std::make_unique<std::condition_variable>();
    auto inserted = streams_.insert(StreamEntry{
        next_stream_id_++,
        plugin_index,
        copy_bytes(arena_->resource(), request.native_id.data, native_size),
        std::move(availability),
        std::move(current_binding),
        std::move(admission_mutex),
        std::move(binding_transition_mutex),
        std::move(binding_transition_condition),
        false,
    });
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    stream_handles_.push_back(inserted.value());
    handle = inserted.value();
    return {};
}

Result<void> ActivationState::get_source(SourceHandle handle, rodaq_source_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const SourceEntry* entry = sources_.find(handle);
    if (entry == nullptr) {
        return activation_status(StatusCode::NotFound, "Source handle was not found");
    }
    view.struct_size = sizeof(view);
    view.handle = rodaq_source_handle{handle.value()};
    view.source_id = rodaq_bytes_view{entry->source_id.data(), entry->source_id.size()};
    return {};
}

Result<void> ActivationState::get_clock(ClockHandle handle, rodaq_clock_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const ClockEntry* entry = clocks_.find(handle);
    if (entry == nullptr) {
        return activation_status(StatusCode::NotFound, "Clock handle was not found");
    }
    view.struct_size = sizeof(view);
    view.handle = rodaq_clock_handle{handle.value()};
    view.clock_id = entry->clock_id;
    view.resolution_numerator = entry->resolution_numerator;
    view.resolution_denominator = entry->resolution_denominator;
    view.origin = rodaq_string_view{entry->origin.data(), entry->origin.size()};
    return {};
}

Result<void> ActivationState::get_schema(SchemaHandle handle, rodaq_schema_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const SchemaEntry* entry = schemas_.find(handle);
    if (entry == nullptr) {
        return activation_status(StatusCode::NotFound, "Schema handle was not found");
    }
    view.struct_size = sizeof(view);
    view.type_identifier_present = entry->type_identifier_present ? RODAQ_TRUE : RODAQ_FALSE;
    view.handle = rodaq_schema_handle{handle.value()};
    view.schema_id = entry->schema_id;
    view.format = rodaq_string_view{entry->format.data(), entry->format.size()};
    view.type_identifier =
        rodaq_string_view{entry->type_identifier.data(), entry->type_identifier.size()};
    view.representation =
        rodaq_bytes_view{entry->representation.data(), entry->representation.size()};
    return {};
}

Result<void> ActivationState::get_timestamp_key_set(
    std::uint64_t id, rodaq_timestamp_key_set_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    for (TimestampKeySetHandle handle : timestamp_key_set_handles_) {
        const TimestampKeySetEntry* entry = timestamp_key_sets_.find(handle);
        if (entry == nullptr || entry->timestamp_key_set_id != id) {
            continue;
        }
        view.struct_size = sizeof(view);
        view.timestamp_key_set_id = id;
        view.keys = entry->abi_keys.data();
        view.key_count = static_cast<std::uint32_t>(entry->abi_keys.size());
        return {};
    }
    return activation_status(StatusCode::NotFound, "TimestampKeySet was not found");
}

Result<void> ActivationState::get_stream(StreamHandle handle, rodaq_stream_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const StreamEntry* entry = streams_.find(handle);
    if (entry == nullptr) {
        return activation_status(StatusCode::NotFound, "Stream handle was not found");
    }
    view.struct_size = sizeof(view);
    view.handle = rodaq_stream_handle{handle.value()};
    view.stream_id = entry->stream_id;
    return {};
}

Result<void> ActivationState::get_stream_descriptor(
    StreamDescriptorHandle handle, rodaq_stream_descriptor_view& view) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    const StreamDescriptorEntry* entry = descriptors_.find(handle);
    if (entry == nullptr) {
        return activation_status(StatusCode::NotFound, "StreamDescriptor handle was not found");
    }
    view.struct_size = sizeof(view);
    view.schema_present = entry->schema_present ? RODAQ_TRUE : RODAQ_FALSE;
    view.handle = rodaq_stream_descriptor_handle{handle.value()};
    view.stream = rodaq_stream_handle{entry->stream.value()};
    view.schema = rodaq_schema_handle{entry->schema.value()};
    view.stream_descriptor_id = entry->stream_descriptor_id;
    const StreamEntry* stream = streams_.find(entry->stream);
    view.stream_id = stream == nullptr ? 0 : stream->stream_id;
    view.name = rodaq_string_view{entry->name.data(), entry->name.size()};
    view.schema_id = entry->schema_id;
    view.payload_encoding =
        rodaq_string_view{entry->payload_encoding.data(), entry->payload_encoding.size()};
    view.timestamp_key_set_id = entry->timestamp_key_set_id;
    view.transport_timestamp_keys = entry->transport_timestamp_keys.data();
    view.transport_timestamp_key_count =
        static_cast<std::uint32_t>(entry->transport_timestamp_keys.size());
    return {};
}

}  // namespace rodaq
