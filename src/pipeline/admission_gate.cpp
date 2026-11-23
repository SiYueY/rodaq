#include "admission_gate.hpp"

#include <limits>
#include <utility>

namespace rodaq::pipeline {

AdmissionCommit::AdmissionCommit(AdmissionGate& gate, std::unique_lock<std::mutex> lock) noexcept
: gate_(&gate), lock_(std::move(lock)) {}

AdmissionCommit::AdmissionCommit(AdmissionCommit&& other) noexcept
: gate_(std::exchange(other.gate_, nullptr)),
  lock_(std::move(other.lock_)),
  committed_(other.committed_) {}

AdmissionCommit& AdmissionCommit::operator=(AdmissionCommit&& other) noexcept {
    if (this != &other) {
        release();
        gate_ = std::exchange(other.gate_, nullptr);
        lock_ = std::move(other.lock_);
        committed_ = other.committed_;
    }
    return *this;
}

AdmissionCommit::~AdmissionCommit() noexcept { release(); }

void AdmissionCommit::release() noexcept {
    if (gate_ != nullptr) {
        --gate_->commit_inflight_;
        gate_ = nullptr;
        lock_.unlock();
    }
}

bool AdmissionCommit::mark_committed() noexcept {
    if (gate_ == nullptr || committed_) {
        return false;
    }
    ++gate_->accepted_records_;
    committed_ = true;
    return true;
}

AdmissionGate::AdmissionGate(
    InstanceId instance_id, ControlGeneration authority_generation) noexcept
: instance_id_(instance_id),
  valid_(valid_instance_id(instance_id) && authority_generation != 0),
  authority_generation_(authority_generation) {}

GateOpenResult AdmissionGate::open_admission(InstanceId instance_id, ControlGeneration generation) {
    if (!valid_ || !(instance_id == instance_id_) ||
        control_fence_.load(std::memory_order_acquire)) {
        return GateOpenResult::Obsolete;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != authority_generation_ || control_fence_.load(std::memory_order_acquire)) {
        return GateOpenResult::Obsolete;
    }
    open_ = true;
    ever_opened_.store(true, std::memory_order_release);
    return GateOpenResult::Opened;
}

Result<AdmissionCommit> AdmissionGate::enter_commit(
    InstanceId instance_id, ControlGeneration generation) {
    if (!valid_ || !(instance_id == instance_id_)) {
        return Status(StatusCode::Conflict, {});
    }
    if (control_fence_.load(std::memory_order_acquire)) {
        return Status(StatusCode::Conflict, {});
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (generation != authority_generation_ || control_fence_.load(std::memory_order_acquire)) {
        return Status(StatusCode::Conflict, {});
    }
    if (!open_) {
        return Status(StatusCode::InvalidState, {});
    }
    if (accepted_records_ == std::numeric_limits<std::uint64_t>::max()) {
        control_fence_.store(true, std::memory_order_release);
        open_ = false;
        return Status(StatusCode::ResourceExhausted, {});
    }
    ++commit_inflight_;
    // Generation and instance are stable under this mutex. Only the atomic
    // early control fence can change between admission checks.
    if (control_fence_.load(std::memory_order_acquire)) {
        --commit_inflight_;
        return Status(StatusCode::Conflict, {});
    }
    return AdmissionCommit(*this, std::move(lock));
}

Result<std::unique_lock<std::mutex>> AdmissionGate::enter_segment_cut(
    InstanceId instance_id, ControlGeneration generation) {
    if (!valid_ || !(instance_id == instance_id_) ||
        control_fence_.load(std::memory_order_acquire)) {
        return Status(StatusCode::Conflict, {});
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (generation != authority_generation_ || control_fence_.load(std::memory_order_acquire)) {
        return Status(StatusCode::Conflict, {});
    }
    if (!open_) {
        return Status(StatusCode::InvalidState, {});
    }
    return lock;
}

Result<void> AdmissionGate::establish_control_fence(InstanceId instance_id) noexcept {
    if (!valid_ || !(instance_id == instance_id_)) {
        return Status(StatusCode::Conflict, {});
    }
    control_fence_.store(true, std::memory_order_release);
    return {};
}

Result<std::unique_lock<std::mutex>> AdmissionGate::enter_descriptor_cut(
    InstanceId instance_id, ControlGeneration generation) {
    if (!valid_ || !(instance_id == instance_id_) ||
        control_fence_.load(std::memory_order_acquire)) {
        return Status(StatusCode::Conflict, {});
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (generation != authority_generation_ || control_fence_.load(std::memory_order_acquire)) {
        return Status(StatusCode::Conflict, {});
    }
    return lock;
}

Result<void> AdmissionGate::advance_authority(
    InstanceId instance_id, ControlGeneration generation) {
    if (!valid_ || !(instance_id == instance_id_)) {
        return Status(StatusCode::Conflict, {});
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation < authority_generation_) {
        return Status(StatusCode::Conflict, {});
    }
    control_fence_.store(true, std::memory_order_release);
    authority_generation_ = generation;
    open_ = false;
    return {};
}

GateSnapshot AdmissionGate::snapshot_locked() const noexcept {
    const auto fenced = control_fence_.load(std::memory_order_acquire);
    return {instance_id_,      authority_generation_, valid_ && open_ && !fenced,
            !valid_ || fenced, commit_inflight_,      accepted_records_,
            ever_opened()};
}

Result<GateSnapshot> AdmissionGate::close_and_capture(InstanceId instance_id) {
    if (!valid_ || !(instance_id == instance_id_)) {
        return Status(StatusCode::Conflict, {});
    }
    control_fence_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    return snapshot_locked();
}

GateSnapshot AdmissionGate::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_locked();
}

}  // namespace rodaq::pipeline
