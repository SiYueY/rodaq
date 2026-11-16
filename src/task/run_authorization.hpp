#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "control_record.hpp"
#include "recovery_store.hpp"
#include "../pipeline/pipeline_controller.hpp"

namespace rodaq::task {

struct RunPipeline {
    std::shared_ptr<pipeline::PipelineController> controller;
    std::string effective_config_id;
    bool required{true};
};

struct RunPrepareResult {
    bool barrier_passed{false};
    std::vector<StatusCode> pipelines;
};

// One accepted Start/Resume's actual work owner. Native preparation and Recovery
// publication run outside Task control. The bounded fence/Open cut runs inside
// Task control and does no I/O, Native calls or ordinary allocation.
class RunAuthorization {
public:
    static Result<std::unique_ptr<RunAuthorization>> create(
        const TaskControlRecord& accepted, StartPolicyMode policy,
        std::vector<RunPipeline> pipelines, std::shared_ptr<TaskRecoveryStore> recovery,
        time::BootIdentity boot, pipeline::ReceiveClock sample = &time::sample_clocks);

    Result<RunPrepareResult> prepare(plugin::Deadline deadline);
    // Actual Recovery execution, before interpreting the Prepare Barrier.
    // Also records successful preparations when a Required peer failed.
    Result<void> checkpoint_prepared();
    // Exact full-set publication; an uncertain write never grants permission.
    Result<void> publish_authorization(std::uint64_t recording_base);
    // Caller supplies its actual current durable Control on the serial control
    // turn; mere generation equality never substitutes for operation identity.
    Result<void> open(const TaskControlRecord& current);
    Result<void> fence() noexcept;

private:
    RunAuthorization() = default;
    bool current(const TaskControlRecord& control) const noexcept;

    TaskId task_id_;
    OperationId operation_id_;
    ControlGeneration generation_{0};
    std::string config_version_;
    StartPolicyMode policy_{StartPolicyMode::AllRequired};
    std::vector<RunPipeline> pipelines_;
    std::vector<std::optional<pipeline::PreparedPipeline>> prepared_;
    std::shared_ptr<TaskRecoveryStore> recovery_;
    time::BootIdentity boot_;
    pipeline::ReceiveClock sample_{nullptr};
    std::optional<TaskRecoveryRecord> prepared_record_;
    std::mutex opening_;
    bool fenced_{false};
    bool prepare_attempted_{false};
    bool barrier_passed_{false};
    bool publication_attempted_{false};
    bool authorized_{false};
    std::uint64_t opened_at_{0};
    std::uint64_t recording_base_{0};
};

}  // namespace rodaq::task
