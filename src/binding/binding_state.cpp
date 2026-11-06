#include "binding/binding_state.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace rodaq {
namespace {

std::uint32_t registry_generation(std::uint64_t activation_generation) noexcept {
    const auto generation = static_cast<std::uint32_t>(activation_generation);
    return generation == 0 ? 1U : generation;
}

std::size_t stream_index(StreamHandle stream, std::size_t capacity) {
    const std::uint64_t encoded = stream.value() & 0xffffffffULL;
    if (encoded == 0 || encoded > capacity) {
        throw std::invalid_argument("Binding stream handle is outside the activation capacity");
    }
    return static_cast<std::size_t>(encoded - 1U);
}

Status binding_status(StatusCode status_code, std::string message) {
    return Status(status_code, "binding", 0, std::move(message));
}

}  // namespace

BindingState::BindingState(const DAQConfig& config, std::uint64_t activation_generation)
: bindings_([&] {
      auto result = Registry<BindingEntry, BindingHandle>::create(
          config.resources.max_bindings, registry_generation(activation_generation));
      if (!result.ok()) {
          throw std::runtime_error(std::string(result.status().message()));
      }
      return std::move(result.value());
  }()),
  streams_(config.resources.max_streams) {
    binding_handles_.reserve(config.resources.max_bindings);
}

void BindingState::register_stream(StreamHandle stream) {
    auto& state = streams_[stream_index(stream, streams_.size())];
    if (state != nullptr) {
        throw std::invalid_argument("Binding stream was already registered");
    }
    state = std::make_unique<StreamBindingState>();
}

bool BindingState::is_stream_registered(StreamHandle stream) const noexcept {
    try {
        const std::size_t index = stream_index(stream, streams_.size());
        return streams_[index] != nullptr;
    } catch (...) {
        return false;
    }
}

StreamBindingState& BindingState::stream_state(StreamHandle stream) {
    auto& state = streams_[stream_index(stream, streams_.size())];
    if (state == nullptr) {
        throw std::invalid_argument("Binding stream is not registered");
    }
    return *state;
}

Result<BindingHandle> BindingState::create_binding(
    std::size_t plugin_index, StreamHandle stream, StreamDescriptorHandle descriptor,
    BindingLifecycle lifecycle_value) {
    if (!is_stream_registered(stream)) {
        return binding_status(StatusCode::InvalidArgument, "Binding Stream is not registered");
    }
    auto inserted = bindings_.insert(BindingEntry{
        stream,
        descriptor,
        plugin_index,
        std::make_unique<std::atomic<std::uint32_t>>(static_cast<std::uint32_t>(lifecycle_value)),
        std::make_unique<std::atomic<std::uint64_t>>(0),
    });
    if (!inserted.ok()) {
        return std::move(inserted).status();
    }
    binding_handles_.push_back(inserted.value());
    return inserted.value();
}

Result<BindingLifecycle> BindingState::lifecycle(BindingHandle binding) const {
    const BindingEntry* entry = bindings_.find(binding);
    if (entry == nullptr) {
        return binding_status(StatusCode::NotFound, "Binding handle was not found");
    }
    const auto value = entry->lifecycle->load(std::memory_order_acquire);
    if (value < static_cast<std::uint32_t>(BindingLifecycle::Prepared) ||
        value > static_cast<std::uint32_t>(BindingLifecycle::Retired)) {
        return binding_status(StatusCode::InvalidState, "Binding lifecycle value is invalid");
    }
    return static_cast<BindingLifecycle>(value);
}

Result<void> BindingState::transition(
    BindingHandle binding, BindingLifecycle expected, BindingLifecycle desired) {
    BindingEntry* entry = bindings_.find(binding);
    if (entry == nullptr) {
        return binding_status(StatusCode::NotFound, "Binding handle was not found");
    }
    std::uint32_t current = static_cast<std::uint32_t>(expected);
    const bool valid =
        (expected == BindingLifecycle::Prepared &&
         (desired == BindingLifecycle::Active || desired == BindingLifecycle::Retired)) ||
        (expected == BindingLifecycle::Active && desired == BindingLifecycle::Draining) ||
        (expected == BindingLifecycle::Draining && desired == BindingLifecycle::Retired);
    if (!valid) {
        return binding_status(
            StatusCode::InvalidArgument, "Binding lifecycle transition is invalid");
    }
    if (!entry->lifecycle->compare_exchange_strong(
            current, static_cast<std::uint32_t>(desired), std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return binding_status(StatusCode::Conflict, "Binding lifecycle transition is stale");
    }
    return {};
}

BindingHandle BindingState::current_binding(StreamHandle stream) const noexcept {
    try {
        const auto& state_pointer = streams_[stream_index(stream, streams_.size())];
        if (state_pointer == nullptr) {
            return {};
        }
        const StreamBindingState& state = *state_pointer;
        return BindingHandle::from_raw(state.current_binding.load(std::memory_order_acquire));
    } catch (...) {
        return {};
    }
}

void BindingState::set_current_binding(StreamHandle stream, BindingHandle binding) noexcept {
    try {
        const auto& state_pointer = streams_[stream_index(stream, streams_.size())];
        if (state_pointer == nullptr) {
            return;
        }
        StreamBindingState& state = *state_pointer;
        state.current_binding.store(binding.value(), std::memory_order_release);
    } catch (...) {
    }
}

bool BindingState::retire_if_drained(BindingHandle binding) noexcept {
    try {
        BindingEntry* entry = bindings_.find(binding);
        if (entry == nullptr || entry->admission_inflight->load(std::memory_order_acquire) != 0) {
            return false;
        }
        return transition(binding, BindingLifecycle::Draining, BindingLifecycle::Retired).ok();
    } catch (...) {
        return false;
    }
}

}  // namespace rodaq
