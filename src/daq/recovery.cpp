#include "daq/daq_impl.hpp"
#include "plugin/acquisition_plugin_instance.hpp"
#include "plugin/plugin_quarantine.hpp"
#include "plugin/storage_plugin_instance.hpp"
#include <rodaq/plugin/abi_support.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace rodaq {

namespace {

using Clock = std::chrono::steady_clock;

Status abi_failure_status(
    rodaq_status_code status, const rodaq_status& error, std::string_view fallback) {
    auto converted = status_from_abi(status, &error, "runtime");
    if (!converted.ok()) {
        return converted;
    }
    return daq_status(StatusCode::InternalError, std::string(fallback));
}

}  // namespace

void DAQ::Impl::failure_loop() noexcept {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(failure_signal_->mutex);
            failure_signal_->condition.wait(
                lock, [this] { return failure_signal_->requested || failure_signal_->closed; });
            if (failure_signal_->closed) {
                return;
            }
            failure_signal_->requested = false;
        }
        LifecycleOwner owner(*this);
        // Start's abort and an already owning Stop finish their own cleanup.
        if (active_ != nullptr && active_->has_primary_failure()) {
            static_cast<void>(stop_internal());
        }
    }
}
void DAQ::Impl::abort_start() noexcept {
    aborting_ = true;
    if (active_ == nullptr) {
        return;
    }
    active_->mark_stopping();
    const bool descriptor_worker_closed = outside_lifecycle([&] {
        return active_->close_and_join_descriptor_reconciliation(
            Clock::now() + options_.stop_progress_timeout);
    });
    if (start_quarantine_required_ || active_->requires_quarantine() || !descriptor_worker_closed) {
        capture_activation_outcome();
        quarantined_ = std::move(active_);
        start_quarantine_required_ = false;
        state_ = DAQState::Failed;
        return;
    }
    if (active_->pipeline() != nullptr) {
        active_->pipeline()->close_admission();
        active_->pipeline()->close_dispatch();
        active_->pipeline()->join_dispatch();
        active_->pipeline()->close_callbacks();
        active_->pipeline()->join_completion();
    }
    bool unsafe_to_release = false;
    std::shared_ptr<ActivationState> state = active_;
    for (std::size_t offset = 0; offset < acquisition_states_.size(); ++offset) {
        const std::size_t index = acquisition_states_.size() - offset - 1U;
        const AcquisitionActivationState activation_state = acquisition_states_[index];
        if (!active_->acquisition_instance(index).has_value() ||
            (activation_state != AcquisitionActivationState::StartAttempted &&
             activation_state != AcquisitionActivationState::Started &&
             activation_state != AcquisitionActivationState::Quiesced)) {
            continue;
        }

        if (activation_state != AcquisitionActivationState::Quiesced) {
            auto quiesced = run_bounded_call(
                "abort-quiesce",
                [state, index]() -> Result<void> {
                    auto control = state->acquisition_instance(index)->required_control_interface();
                    if (!control.ok()) {
                        return std::move(control).status();
                    }
                    const auto* table =
                        static_cast<const rodaq_acquisition_control*>(control->vtable);
                    rodaq_status error{};
                    const rodaq_status_code status =
                        table->quiesce_plugin(control->context, &error);
                    if (status != RODAQ_STATUS_OK) {
                        return abi_failure_status(
                            status, error, "Acquisition quiescence failed during abort");
                    }
                    return {};
                },
                Clock::now() + options_.stop_progress_timeout);
            if (quiesced.status != BoundedCallOutcome::Outcome::Completed) {
                active_->request_failure(
                    quiesced.status == BoundedCallOutcome::Outcome::TimedOut
                        ? daq_status(
                              StatusCode::Timeout,
                              "Acquisition quiescence timed out during activation abort")
                        : std::move(quiesced.error));
                unsafe_to_release = true;
                break;
            }
            acquisition_states_[index] = AcquisitionActivationState::Quiesced;
        }

        auto stopped = run_bounded_call(
            "abort-stop", [state, index]() { return state->acquisition_instance(index)->stop(); },
            Clock::now() + options_.stop_progress_timeout);
        if (stopped.status != BoundedCallOutcome::Outcome::Completed) {
            active_->request_failure(
                stopped.status == BoundedCallOutcome::Outcome::TimedOut
                    ? daq_status(
                          StatusCode::Timeout, "Acquisition stop timed out during activation abort")
                    : std::move(stopped.error));
            unsafe_to_release = true;
            break;
        }
        acquisition_states_[index] = AcquisitionActivationState::Stopped;
    }
    if (!unsafe_to_release && storage_open_ && storage_state_ == StorageActivationState::Opened &&
        active_->storage_instance().has_value()) {
        rodaq_storage_close_request close_request{};
        close_request.struct_size = sizeof(close_request);
        close_request.disposition = RODAQ_STORAGE_CLOSE_ACTIVATION_ABORT;
        auto closed = run_bounded_call(
            "abort-close",
            [state, close_request]() { return state->storage_instance()->close(close_request); },
            Clock::now() + options_.stop_progress_timeout);
        if (closed.status == BoundedCallOutcome::Outcome::Completed) {
            storage_state_ = StorageActivationState::Closed;
        } else {
            active_->request_failure(
                closed.status == BoundedCallOutcome::Outcome::TimedOut
                    ? daq_status(
                          StatusCode::Timeout, "Storage close timed out during activation abort")
                    : std::move(closed.error));
            storage_state_ = StorageActivationState::Quarantined;
            unsafe_to_release = true;
        }
    }
    if (!unsafe_to_release) {
        for (std::size_t index = 0; index < options_.acquisitions.size(); ++index) {
            if (!active_->acquisition_instance(index).has_value() ||
                !active_->acquisition_instance(index)->valid()) {
                continue;
            }
            auto destroyed = run_bounded_call(
                "abort-destroy-acquisition",
                [state, index]() {
                    state->acquisition_instance(index)->destroy();
                    return Result<void>();
                },
                Clock::now() + options_.stop_progress_timeout);
            if (destroyed.status != BoundedCallOutcome::Outcome::Completed) {
                active_->request_failure(
                    destroyed.status == BoundedCallOutcome::Outcome::TimedOut
                        ? daq_status(
                              StatusCode::Timeout,
                              "Acquisition destroy timed out during activation abort")
                        : std::move(destroyed.error));
                unsafe_to_release = true;
                break;
            }
        }
    }
    if (!unsafe_to_release && active_->storage_instance().has_value() &&
        active_->storage_instance()->valid()) {
        auto destroyed = run_bounded_call(
            "abort-destroy-storage",
            [state]() {
                state->storage_instance()->destroy();
                return Result<void>();
            },
            Clock::now() + options_.stop_progress_timeout);
        if (destroyed.status != BoundedCallOutcome::Outcome::Completed) {
            active_->request_failure(
                destroyed.status == BoundedCallOutcome::Outcome::TimedOut
                    ? daq_status(
                          StatusCode::Timeout, "Storage destroy timed out during activation abort")
                    : std::move(destroyed.error));
            unsafe_to_release = true;
        }
    }
    if (!outside_lifecycle([&] {
            return active_->shutdown_logging(Clock::now() + options_.logging.shutdown_timeout);
        })) {
        active_->quarantine_logging("DAQ logging shutdown timed out during abort");
        active_->record_secondary_failure(daq_status(
            StatusCode::Timeout, "DAQ logging shutdown timed out during activation abort"));
    }
    if (unsafe_to_release) {
        capture_activation_outcome();
        quarantined_ = std::move(active_);
    } else {
        capture_activation_outcome();
        active_.reset();
        release_activation_slot();
    }
    if (!unsafe_to_release) {
        std::fill(
            acquisition_states_.begin(), acquisition_states_.end(),
            AcquisitionActivationState::Empty);
        storage_state_ = StorageActivationState::Empty;
    }
    if (!unsafe_to_release) {
        storage_open_ = false;
    }
    start_quarantine_required_ = false;
    state_ = DAQState::Failed;
}

bool DAQ::Impl::recover_activation() {
    LifecycleOwner owner(*this);
    active_->reap_quarantined_workers();
    if (recovery_abandoned_) {
        return false;
    }
    if (pending_call_ != nullptr && !pending_call_->joined->load(std::memory_order_acquire)) {
        return false;
    }
    if (active_->requires_quarantine()) {
        return false;
    }
    if (pending_call_ != nullptr && pending_call_->site.find("destroy") != std::string::npos &&
        pending_call_->result.ok()) {
        pending_call_.reset();
    }
    if (aborting_) {
        // A timed-out activation call has finished; its original output was
        // never published, so abort now owns only the published instances.
        if (pending_call_ != nullptr && (pending_call_->site == "activation-call" ||
                                         pending_call_->site == "activation-value")) {
            pending_call_.reset();
        }
        start_quarantine_required_ = false;
        abort_start();
    } else {
        static_cast<void>(stop_internal());
    }
    if (quarantined_ != nullptr) {
        active_ = std::move(quarantined_);
    }
    return active_ == nullptr;
}

void DAQ::Impl::transfer_quarantine() {
    auto recovery = std::make_shared<Impl>(options_);
    recovery->active_ = quarantined_;
    recovery->state_ = DAQState::Failed;
    recovery->recovering_ = true;
    recovery->activation_generation_ = activation_generation_;
    recovery->activation_slot_ = activation_slot_;
    recovery->acquisition_states_ = acquisition_states_;
    recovery->storage_state_ = storage_state_;
    recovery->storage_open_ = storage_open_;
    recovery->storage_flushed_ = storage_flushed_;
    recovery->aborting_ = aborting_;
    recovery->recovery_abandoned_ = recovery_abandoned_;
    recovery->pending_call_ = std::move(pending_call_);
    recovery->effective_storage_flush_timeout_ = effective_storage_flush_timeout_;
    recovery->effective_storage_close_timeout_ = effective_storage_close_timeout_;
    std::weak_ptr<Impl> weak = recovery;
    if (!plugin::PluginQuarantine::process().retain_activation(
            *activation_slot_, recovery, "DAQ activation quarantine", [weak] {
                auto owner = weak.lock();
                return owner != nullptr && owner->recover_activation();
            })) {
        std::abort();
    }
    activation_slot_.reset();
}

Result<void> DAQ::Impl::quarantine_failure() {
    if (active_ != nullptr) {
        quarantined_ = std::move(active_);
    }
    state_ = DAQState::Failed;
    if (activation_slot_.has_value() && !recovering_) {
        transfer_quarantine();
    }
    return daq_status(StatusCode::Unavailable, "DAQ stop requires quarantine");
}

void DAQ::Impl::capture_activation_outcome() noexcept {
    if (active_ == nullptr) {
        return;
    }
    try {
        active_->record_stop_latency();
        last_stats_ = active_->stats();
        auto payload_stats = active_->payload_pool_statistics();
        if (payload_stats.ok()) {
            last_payload_stats_ = std::move(payload_stats.value());
        }
        auto primary = active_->primary_failure();
        if (!primary.ok()) {
            auto secondary = active_->secondary_failures();
            if (secondary.ok()) {
                last_secondary_failures_ = std::move(secondary.value());
            }
            last_secondary_failure_overflow_count_ = active_->secondary_failure_overflow_count();
            last_primary_failure_ = std::move(primary);
        }
    } catch (...) {
    }
}

void DAQ::Impl::release_activation_slot() noexcept {
    if (activation_slot_.has_value()) {
        plugin::PluginQuarantine::process().release_activation(*activation_slot_);
        activation_slot_.reset();
    }
}

}  // namespace rodaq
