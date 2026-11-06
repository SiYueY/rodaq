#include "daq/daq_impl.hpp"

#include "plugin/plugin_quarantine.hpp"

#include <utility>

namespace rodaq {

Status daq_status(StatusCode status_code, std::string message) {
    return Status(status_code, "daq", 0, std::move(message));
}

DAQ::DAQ(DAQConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

DAQ::~DAQ() = default;

Result<void> DAQ::configure() { return impl_->configure(); }

Result<void> DAQ::start() { return impl_->start(); }

Result<void> DAQ::stop() { return impl_->stop(); }

DAQState DAQ::state() const noexcept { return impl_->state(); }

Result<DAQLifecycleSnapshot> DAQ::lifecycle_snapshot() const { return impl_->lifecycle_snapshot(); }

Status DAQ::primary_failure() const { return impl_->primary_failure(); }

Result<std::vector<Status>> DAQ::secondary_failures() const { return impl_->secondary_failures(); }

std::uint64_t DAQ::secondary_failure_overflow_count() const noexcept {
    return impl_->secondary_failure_overflow_count();
}

Result<DAQStats> DAQ::stats() const { return impl_->stats(); }

Result<PayloadStats> DAQ::payload_stats() const { return impl_->payload_stats(); }

DAQ::Impl::Impl(DAQConfig config)
: options_(std::move(config)),
  acquisition_states_(options_.acquisitions.size(), AcquisitionActivationState::Empty) {}

DAQ::Impl::LifecycleOwner::LifecycleOwner(Impl& owner)
: owner_(owner), lock_(owner.lifecycle_mutex_) {
    owner_.lifecycle_condition_.wait(lock_, [&] { return !owner_.lifecycle_owned_; });
    owner_.lifecycle_owned_ = true;
}

DAQ::Impl::LifecycleOwner::~LifecycleOwner() {
    owner_.lifecycle_owned_ = false;
    owner_.lifecycle_condition_.notify_all();
}

DAQState DAQ::Impl::state() const noexcept {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (active_ != nullptr && active_->phase() == ActivationPhase::Failed) {
        return DAQState::Failed;
    }
    return state_;
}

Result<DAQLifecycleSnapshot> DAQ::Impl::lifecycle_snapshot() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (active_ == nullptr) {
        DAQLifecycleSnapshot snapshot;
        snapshot.state = state_;
        snapshot.generation = activation_generation_;
        return snapshot;
    }
    return active_->lifecycle_snapshot();
}

Status DAQ::Impl::primary_failure() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (active_ == nullptr) {
        if (quarantined_ != nullptr) {
            return quarantined_->primary_failure();
        }
        return last_primary_failure_;
    }
    return active_->primary_failure();
}

Result<std::vector<Status>> DAQ::Impl::secondary_failures() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (active_ != nullptr) {
        return active_->secondary_failures();
    }
    if (quarantined_ != nullptr) {
        return quarantined_->secondary_failures();
    }
    if (!last_primary_failure_.ok()) {
        return last_secondary_failures_;
    }
    return daq_status(StatusCode::InvalidState, "DAQ has no failure history");
}

std::uint64_t DAQ::Impl::secondary_failure_overflow_count() const noexcept {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (active_ != nullptr) {
        return active_->secondary_failure_overflow_count();
    }
    if (quarantined_ != nullptr) {
        return quarantined_->secondary_failure_overflow_count();
    }
    return last_secondary_failure_overflow_count_;
}

Result<DAQStats> DAQ::Impl::stats() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (active_ != nullptr) {
        return active_->stats();
    }
    if (quarantined_ != nullptr) {
        return quarantined_->stats();
    }
    if (last_stats_.has_value()) {
        return last_stats_.value();
    }
    return daq_status(StatusCode::InvalidState, "DAQ has no active statistics");
}

Result<PayloadStats> DAQ::Impl::payload_stats() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (active_ != nullptr) {
        return active_->payload_pool_statistics();
    }
    if (quarantined_ != nullptr) {
        return quarantined_->payload_pool_statistics();
    }
    if (last_payload_stats_.has_value()) {
        return last_payload_stats_.value();
    }
    return daq_status(StatusCode::InvalidState, "DAQ has no active Payload statistics");
}

DAQ::Impl::~Impl() {
    // Close the lifetime-safe mailbox before joining the sole supervisor.
    failure_signal_->close();
    if (failure_worker_.joinable()) {
        failure_worker_.join();
    }
    plugin::PluginQuarantine::process().reap_completed();
    if (active_ != nullptr) {
        static_cast<void>(stop());
    }
    LifecycleOwner owner(*this);
    if (quarantined_ != nullptr) {
        if (activation_slot_.has_value()) {
            transfer_quarantine();
        }
        quarantined_.reset();
    } else {
        release_activation_slot();
    }
    plugin::PluginQuarantine::process().reap_completed();
}

}  // namespace rodaq
