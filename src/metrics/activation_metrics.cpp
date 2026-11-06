#include "activation/activation_internal.hpp"

namespace rodaq {

Result<PayloadStats> ActivationState::payload_pool_statistics() const {
    try {
        return payload_pool_.stats();
    } catch (const std::bad_alloc&) {
        return activation_status(
            StatusCode::ResourceExhausted, "Payload statistics allocation failed");
    } catch (const std::exception& error) {
        return activation_status(StatusCode::InternalError, error.what());
    }
}

void ActivationState::record_storage_latency(
    StorageLatencyKind kind, std::int64_t duration_ns) noexcept {
    LatencyHistogram* histogram = nullptr;
    switch (kind) {
        case StorageLatencyKind::Handoff:
            histogram = storage_handoff_latency_.get();
            break;
        case StorageLatencyKind::PayloadConsumed:
            histogram = payload_consumed_latency_.get();
            break;
        case StorageLatencyKind::PersistenceCompleted:
            histogram = persistence_latency_.get();
            break;
    }
    if (histogram != nullptr && duration_ns >= 0) {
        static_cast<void>(histogram->record(duration_ns));
    }
}

void ActivationState::record_stop_latency() noexcept {
    if (!stop_started_valid_ || stop_latency_ == nullptr) {
        return;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - stop_started_).count();
    if (elapsed >= 0) {
        static_cast<void>(stop_latency_->record(elapsed));
    }
    stop_started_valid_ = false;
}

void ActivationState::fill_latency_stats(DAQStats& snapshot) const noexcept {
    if (admission_wait_latency_ != nullptr) {
        snapshot.latencies.admission_wait = latency_snapshot(*admission_wait_latency_);
    }
    if (storage_handoff_latency_ != nullptr) {
        snapshot.latencies.storage_handoff = latency_snapshot(*storage_handoff_latency_);
    }
    if (payload_consumed_latency_ != nullptr) {
        snapshot.latencies.payload_consumed = latency_snapshot(*payload_consumed_latency_);
    }
    if (persistence_latency_ != nullptr) {
        snapshot.latencies.persistence = latency_snapshot(*persistence_latency_);
    }
    if (stop_latency_ != nullptr) {
        snapshot.latencies.stop = latency_snapshot(*stop_latency_);
    }
}

DAQStats ActivationState::stats() const noexcept {
    DAQStats snapshot = stats_.snapshot();
    snapshot.logging = current_logging_stats();
    fill_latency_stats(snapshot);
    return snapshot;
}

}  // namespace rodaq
