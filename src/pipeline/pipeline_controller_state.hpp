#pragma once

#include "pipeline_controller.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include <rodaq/acquisition/interface.hpp>

namespace rodaq::pipeline {

class PipelineControlCall {
public:
    explicit PipelineControlCall(std::atomic<std::uint32_t>& count) noexcept {
        std::uint32_t empty = 0;
        if (count.compare_exchange_strong(empty, 1)) {
            count_ = &count;
        }
    }

    ~PipelineControlCall() noexcept {
        if (count_) {
            count_->store(0);
        }
    }

    explicit operator bool() const noexcept { return count_ != nullptr; }

    PipelineControlCall(const PipelineControlCall&) = delete;
    PipelineControlCall& operator=(const PipelineControlCall&) = delete;

private:
    std::atomic<std::uint32_t>* count_{nullptr};
};

struct PipelineController::WriterDomain {
    std::shared_ptr<RecordingInstance> instance;
    std::shared_ptr<RecordingSegment> segment;
    std::unique_ptr<PipelineWriter> writer;
    std::shared_ptr<ExecutionLane> lane;
    std::chrono::nanoseconds timeout;
    std::atomic<StatusCode> failure{StatusCode::Ok};
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::uint32_t jobs{0};
    bool scheduled{false};
    bool rotating{false};
    std::weak_ptr<Work> pump;

    void fail(StatusCode error) noexcept {
        auto expected = StatusCode::Ok;
        (void)failure.compare_exchange_strong(expected, error);
        (void)instance->establish_control_fence();
    }

    struct Operation final : Work {
        std::shared_ptr<WriterDomain> owner;
        std::function<Result<void>()> body;
        std::mutex mutex;
        std::condition_variable returned;
        std::optional<Result<void>> result;

        void run() noexcept override {
            Result<void> actual;
            try {
                actual = body();
            } catch (const std::bad_alloc&) {
                actual = Status(StatusCode::ResourceExhausted, {});
            } catch (...) {
                actual = Status(StatusCode::Internal, {});
            }
            if (!actual.ok()) {
                owner->fail(actual.status().code());
            }
            {
                std::lock_guard<std::mutex> lock(owner->mutex);
                --owner->jobs;
                owner->changed.notify_all();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                result.emplace(std::move(actual));
            }
            returned.notify_all();
        }
    };

    Result<void> execute(std::shared_ptr<WriterDomain> self, std::function<Result<void>()> body) {
        try {
            auto job = std::make_shared<Operation>();
            job->owner = std::move(self);
            job->body = std::move(body);
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++jobs;
            }
            auto queued = lane->submit(job);
            if (!queued.ok()) {
                std::lock_guard<std::mutex> lock(mutex);
                --jobs;
                changed.notify_all();
                return queued.status();
            }
            std::unique_lock<std::mutex> lock(job->mutex);
            // Actual wait, not the Task caller's observation deadline. The
            // bounded frame retains all Native inputs until the actual return.
            job->returned.wait(lock, [&] { return job->result.has_value(); });
            return std::move(*job->result);
        } catch (const std::bad_alloc&) {
            return Status(StatusCode::ResourceExhausted, {});
        }
    }

    void wake() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (scheduled || rotating) {
                return;
            }
            scheduled = true;
        }
        auto work = pump.lock();
        auto queued = work ? lane->submit(std::move(work))
                           : Result<void>(Status(StatusCode::InvalidState, {}));
        if (!queued.ok()) {
            fail(queued.status().code());
            std::lock_guard<std::mutex> lock(mutex);
            scheduled = false;
            changed.notify_all();
        }
    }

    struct Pump final : Work {
        std::shared_ptr<WriterDomain> owner;
        void run() noexcept override {
            auto segment = std::atomic_load(&owner->segment);
            bool continue_draining = true;
            try {
                auto deadline = plugin::Deadline::after(owner->timeout);
                if (!deadline.ok()) {
                    owner->fail(deadline.status().code());
                    continue_draining = false;
                } else if (owner->writer && segment) {
                    auto result = owner->writer->write_available(
                        owner->instance->queue, segment, deadline.value());
                    if (!result.ok()) {
                        owner->fail(result.status().code());
                        continue_draining = false;
                    } else if (result.value().failure != StatusCode::Ok) {
                        owner->fail(result.value().failure);
                    }
                } else {
                    owner->fail(StatusCode::InvalidState);
                    continue_draining = false;
                }
            } catch (...) {
                owner->fail(StatusCode::Internal);
                continue_draining = false;
            }
            bool more = false;
            {
                std::lock_guard<std::mutex> lock(owner->mutex);
                // Publish happens before wake. The same lock serializes the
                // empty->idle cut with a concurrent producer's next wake.
                more = continue_draining && !owner->rotating &&
                       owner->instance->queue.snapshot().queued_records != 0;
                if (!more) {
                    owner->scheduled = false;
                    owner->changed.notify_all();
                }
            }
            if (more) {
                auto next = owner->pump.lock();
                auto queued = next ? owner->lane->submit(std::move(next))
                                   : Result<void>(Status(StatusCode::InvalidState, {}));
                if (!queued.ok()) {
                    owner->fail(queued.status().code());
                    std::lock_guard<std::mutex> lock(owner->mutex);
                    owner->scheduled = false;
                    owner->changed.notify_all();
                }
            }
        }
    };

    Result<void> wait_idle(plugin::Deadline deadline) {
        std::unique_lock<std::mutex> lock(mutex);
        const auto idle = [&] { return jobs == 0 && !scheduled; };
        if (deadline.is_infinite()) {
            changed.wait(lock, idle);
            return {};
        }
        auto remaining = deadline.remaining();
        if (!remaining.ok()) {
            return remaining.status();
        }
        if (!changed.wait_for(lock, remaining.value(), idle)) {
            return Status(StatusCode::DeadlineExceeded, {});
        }
        return {};
    }
};

struct RotatedSegment {
    std::shared_ptr<RecordingSegment> segment;
    std::uint64_t work_id{0};
    std::optional<SealedPipelineSegment> handoff;
};

Result<void> add_segment_records(
    SegmentMetadataSnapshot& aggregate, const SegmentMetadataSnapshot& segment,
    bool completed_only = false);

struct PipelineController::State {
    explicit State(PipelineOptions value) : options(std::move(value)) {}
    Result<void> freeze_effective_configuration();

    PipelineOptions options;
    std::shared_ptr<WriterDomain> output;
    std::shared_ptr<Work> pump;
    std::vector<ReadinessPolicy> policies;
    std::optional<plugin::PluginLibrary> acquisition_library;
    std::optional<plugin::PluginLibrary> storage_library;
    std::optional<plugin::PluginInstance> acquisition_instance;
    std::optional<plugin::PluginInstance> storage_instance;
    std::optional<acquisition::Handle> acquisition;
    std::atomic<std::uint32_t> control{0};
    std::atomic<bool> prepared{false};
    std::atomic<bool> configuration_rejected{false};
    std::atomic<bool> source_quiesced{true};
    std::atomic<bool> storage_closed{true};
    std::atomic<bool> source_closed{true};
    std::atomic<bool> artifact_settled{false};
    std::atomic<bool> creation_unresolved{false};
    std::atomic<std::uint64_t> artifact_work{0};
    std::atomic<std::uint64_t> prepared_at{0};
    std::mutex open_mutex;
    bool opened{false};
    std::uint64_t opened_at{0};
    std::uint64_t recording_base{0};
    bool prepare_attempted{false};
    std::atomic<bool> artifact_handed_off{false};
    std::atomic<bool> empty_settled{false};
    // Fixed two-Segment envelope: current Native drain plus successor, or
    // current writable Segment plus one retained Artifact handoff. No new
    // rotation is admitted until that retained handoff actually settles.
    mutable std::mutex segments;
    std::shared_ptr<RecordingSegment> successor;
    std::shared_ptr<RotatedSegment> retained;
    // Lifetime record counters only. Resource/Descriptor/Epoch gauges belong
    // exclusively to the at-most-two currently retained Segment owners.
    SegmentMetadataSnapshot completed;
    std::atomic<std::uint64_t> segment_started_at{0};
    std::uint64_t successor_started_at{0};
    bool rotation_seal_attempted{false};
    std::array<std::pair<std::uint64_t, std::uint64_t>, 2> acknowledged{};
    std::size_t acknowledged_next{0};

    Result<std::optional<SealedPipelineSegment>> finish_rotation(plugin::Deadline deadline);
};

}  // namespace rodaq::pipeline
