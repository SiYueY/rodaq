#include "activation/activation_error.hpp"
#include "activation/activation_state.hpp"
#include "canonical/model_ops.hpp"
#include "clock/ingress_time.hpp"
#include "plugin/acquisition_plugin_instance.hpp"

#include <rodaq/plugin/services.hpp>

namespace rodaq {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kDAQOwnerIndex = std::numeric_limits<std::size_t>::max();

Result<void> ActivationState::initialize_ingress_clock() {
    static constexpr std::string_view kNativeId = "rodaq/activation/ingress";
    static constexpr std::string_view kOrigin = "monotonic_since_boot";

    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (ingress_clock_.valid()) {
            return activation_status(
                StatusCode::InvalidState, "DAQ ingress Clock is already created");
        }
        auto inserted = canonical_.clocks().insert(ClockEntry{
            copy_bytes(
                arena_->resource(), reinterpret_cast<const std::uint8_t*>(kNativeId.data()),
                kNativeId.size()),
            canonical_.allocate_clock_id(),
            1,
            1'000'000'000,
            copy_string(arena_->resource(), kOrigin),
            kDAQOwnerIndex,
        });
        if (!inserted.ok()) {
            return std::move(inserted).status();
        }
        ingress_clock_ = inserted.value();
        canonical_.clock_handles().push_back(ingress_clock_);
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

    std::lock_guard<std::mutex> lock(control_mutex_);
    const ActivationPhase current_state = phase_.load();
    if (current_state != ActivationPhase::Starting && current_state != ActivationPhase::Running) {
        return activation_status(StatusCode::InvalidState, "Source registration is not allowed");
    }
    if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
        RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
        return activation_status(
            StatusCode::InvalidState, "Source registration requires an available Plugin");
    }
    auto registered = canonical_.register_source(
        plugin_index, request.source_id.data, static_cast<std::size_t>(request.source_id.size));
    if (!registered.ok()) {
        return std::move(registered).status();
    }
    handle = registered.value();
    return {};
}

Result<void> ActivationState::register_clock(
    std::size_t plugin_index, const rodaq_clock_registration& request, ClockHandle& handle) {
    if (!rodaq::plugin::is_valid_clock_registration(request) ||
        plugin_index >= config_.acquisitions.size()) {
        return activation_status(StatusCode::InvalidArgument, "Clock registration is invalid");
    }

    std::lock_guard<std::mutex> lock(control_mutex_);
    const ActivationPhase current_state = phase_.load();
    if (current_state != ActivationPhase::Starting && current_state != ActivationPhase::Running) {
        return activation_status(StatusCode::InvalidState, "Clock registration is not allowed");
    }
    if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
        RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
        return activation_status(
            StatusCode::InvalidState, "Clock registration requires an available Plugin");
    }
    auto registered = canonical_.register_clock(plugin_index, request);
    if (!registered.ok()) {
        return std::move(registered).status();
    }
    handle = registered.value();
    return {};
}

Result<void> ActivationState::register_schema(
    std::size_t plugin_index, const rodaq_schema_registration& request, SchemaHandle& handle) {
    if (!rodaq::plugin::is_valid_schema_registration(request) ||
        plugin_index >= acquisition_health_.size()) {
        return activation_status(StatusCode::InvalidArgument, "Schema registration is invalid");
    }

    std::lock_guard<std::mutex> lock(control_mutex_);
    const ActivationPhase current_state = phase_.load();
    if (current_state != ActivationPhase::Starting && current_state != ActivationPhase::Running) {
        return activation_status(StatusCode::InvalidState, "Schema registration is not allowed");
    }
    if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
        RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
        return activation_status(
            StatusCode::InvalidState, "Schema registration requires an available Plugin");
    }
    auto registered = canonical_.register_schema(request);
    if (!registered.ok()) {
        return std::move(registered).status();
    }
    handle = registered.value();
    return {};
}

Result<void> ActivationState::register_stream(
    std::size_t plugin_index, const rodaq_stream_registration& request, StreamHandle& handle) {
    if (!rodaq::plugin::is_valid_stream_registration(request) ||
        plugin_index >= config_.acquisitions.size()) {
        return activation_status(StatusCode::InvalidArgument, "Stream registration is invalid");
    }

    std::lock_guard<std::mutex> lock(control_mutex_);
    const ActivationPhase current_state = phase_.load();
    if (current_state != ActivationPhase::Starting && current_state != ActivationPhase::Running) {
        return activation_status(StatusCode::InvalidState, "Stream registration is not allowed");
    }
    if (acquisition_health_[plugin_index]->load(std::memory_order_acquire) !=
        RODAQ_ACQUISITION_PLUGIN_AVAILABLE) {
        return activation_status(
            StatusCode::InvalidState, "Stream registration requires an available Plugin");
    }
    auto registered = canonical_.register_stream(
        plugin_index, request.native_id.data, static_cast<std::size_t>(request.native_id.size));
    if (!registered.ok()) {
        return std::move(registered).status();
    }
    handle = registered.value();
    if (!bindings_.is_stream_registered(handle)) {
        bindings_.register_stream(handle);
    }
    if (!admission_.is_stream_registered(handle)) {
        admission_.register_stream(handle);
    }
    return {};
}

Result<void> ActivationState::get_source(SourceHandle handle, rodaq_source_info& info) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return canonical_.get_source(handle, info);
}

Result<void> ActivationState::get_clock(ClockHandle handle, rodaq_clock_info& info) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return canonical_.get_clock(handle, info);
}

Result<void> ActivationState::get_schema(SchemaHandle handle, rodaq_schema_info& info) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return canonical_.get_schema(handle, info);
}

Result<void> ActivationState::get_timestamp_key_set(
    std::uint64_t id, rodaq_timestamp_key_set_info& info) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return canonical_.get_timestamp_key_set(id, info);
}

Result<void> ActivationState::get_stream(StreamHandle handle, rodaq_stream_info& info) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return canonical_.get_stream(handle, info);
}

Result<void> ActivationState::get_stream_descriptor(
    StreamDescriptorHandle handle, rodaq_stream_descriptor_info& info) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return canonical_.get_stream_descriptor(handle, info);
}

}  // namespace rodaq
