#include "pipeline_controller.hpp"

#include <rodaq/acquisition/interface.hpp>
#include <rodaq/acquisition/service.hpp>
#include <rodaq/plugin/registry.hpp>

#include "../work_id.hpp"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <new>
#include <utility>

namespace rodaq::pipeline {
namespace {

class ControlCall {
public:
    explicit ControlCall(std::atomic<std::uint32_t>& count) noexcept {
        std::uint32_t empty = 0;
        if (count.compare_exchange_strong(empty, 1)) {
            count_ = &count;
        }
    }

    ~ControlCall() noexcept {
        if (count_) {
            count_->store(0);
        }
    }

    explicit operator bool() const noexcept { return count_ != nullptr; }
    ControlCall(const ControlCall&) = delete;
    ControlCall& operator=(const ControlCall&) = delete;

private:
    std::atomic<std::uint32_t>* count_{nullptr};
};

}  // namespace

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
            if (scheduled) {
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
                more = continue_draining && owner->instance->queue.snapshot().queued_records != 0;
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

struct PipelineController::State {
    explicit State(PipelineOptions value) : options(std::move(value)) {}
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
    std::atomic<bool> source_quiesced{true};
    std::atomic<bool> storage_closed{true};
    std::atomic<bool> source_closed{true};
    bool artifact_settled{false};
    std::atomic<bool> creation_unresolved{false};
    std::atomic<std::uint64_t> artifact_work{0};
    std::atomic<std::uint64_t> prepared_at{0};
    std::mutex open_mutex;
    bool opened{false};
    std::uint64_t opened_at{0};
    std::uint64_t recording_base{0};
    bool prepare_attempted{false};
    bool artifact_handed_off{false};
    bool empty_settled{false};
};

PipelineController::PipelineController(std::shared_ptr<State> state) noexcept
: state_(std::move(state)) {}

PipelineController::~PipelineController() noexcept {
    // No destructor performs business quiesce or waits on unknown Native work.
    // Composition must retain failed/timed-out owners until actual settlement.
    if (state_->control.load() != 0 || !state_->source_closed.load() ||
        !state_->storage_closed.load() || state_->artifact_work.load() != 0 ||
        (state_->creation_unresolved &&
         state_->options.plugins->phase() != plugin::PluginManagerPhase::Stopped)) {
        std::terminate();
    }
}

Result<std::shared_ptr<PipelineController>> PipelineController::create(PipelineOptions options) {
    if (!options.plugins || !options.writer_lane || !options.clock || !options.volume ||
        !options.capacity || !options.ledger || !options.receive_clock ||
        options.identity.pipeline_id != options.definition.pipeline_id ||
        options.definition.streams.size() != options.streams.size() ||
        options.max_prepared_descriptors == 0 || options.max_prepared_descriptors > 65536 ||
        options.native_timeout <= std::chrono::nanoseconds::zero() ||
        options.native_timeout > std::chrono::hours{1}) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto lane = options.writer_lane->snapshot();
    if (!lane.accepting || lane.worker_count != 1 ||
        lane.capacity < options.limits.callback_concurrency + 2ULL) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        const auto anchor = options.clock->anchor();
        options.envelope.system_boot_id = anchor.boot_id;
        options.envelope.playback_anchor_monotonic = anchor.monotonic_ns;
        options.envelope.playback_anchor_realtime = anchor.realtime_ns;
        auto state = std::make_shared<State>(std::move(options));
        state->output = std::make_shared<WriterDomain>();
        auto output = state->output;
        output->lane = state->options.writer_lane;
        output->timeout = state->options.native_timeout;
        auto pump = std::make_shared<WriterDomain::Pump>();
        pump->owner = output;
        output->pump = pump;
        state->pump = std::move(pump);
        std::weak_ptr<WriterDomain> weak = output;
        auto instance = RecordingInstance::create(
            state->options.identity, std::move(state->options.streams), state->options.limits,
            state->options.clock,
            [weak](auto segment, auto descriptor, auto now, auto deadline) -> Result<void> {
                auto owner = weak.lock();
                if (!owner) {
                    return Status(StatusCode::InvalidState, {});
                }
                return owner->execute(
                    owner, [owner, segment = std::move(segment), descriptor = std::move(descriptor),
                            now, deadline] {
                        if (!owner->writer) {
                            return Result<void>(Status(StatusCode::InvalidState, {}));
                        }
                        return owner->writer->prepare_descriptor(
                            segment, descriptor, now, deadline);
                    });
            },
            state->options.receive_clock,
            [weak] {
                if (auto owner = weak.lock()) {
                    owner->wake();
                }
            });
        if (!instance.ok()) {
            return instance.status();
        }
        output->instance = std::move(instance.value());
        state->policies.reserve(state->options.definition.streams.size());
        for (const auto& binding : output->instance->streams.bindings()) {
            const auto found = std::find_if(
                state->options.definition.streams.begin(), state->options.definition.streams.end(),
                [&](const auto& stream) { return stream.stream_id == binding.stream_id; });
            if (found == state->options.definition.streams.end() ||
                found->stream_key !=
                    std::string_view(binding.stream_key.data, binding.stream_key.size)) {
                return Status(StatusCode::InvalidArgument, {});
            }
            state->policies.push_back(found->readiness_policy);
        }
        return std::shared_ptr<PipelineController>(new PipelineController(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<PreparedPipeline> PipelineController::prepare(plugin::Deadline deadline) {
    ControlCall call(state_->control);
    if (!call) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->prepare_attempted || state_->output->instance->gate.admission_fenced()) {
        return Status(StatusCode::InvalidState, {});
    }
    try {
        state_->prepare_attempted = true;
        auto& options = state_->options;
        auto output = state_->output;
        auto sample = options.receive_clock();
        if (!sample.ok()) {
            return sample.status();
        }
        auto storage = options.plugins->load(
            options.definition.storage.plugin_id, options.definition.storage.plugin_version);
        if (!storage.ok()) {
            return storage.status();
        }
        state_->storage_library.emplace(std::move(storage.value()));
        plugin::ServiceRegistry empty;
        auto frozen = empty.freeze();
        if (!frozen.ok()) {
            return frozen.status();
        }
        auto created_storage =
            state_->storage_library->create_instance(empty, options.definition.storage.json);
        if (!created_storage.ok()) {
            state_->creation_unresolved = true;
            return created_storage.status();
        }
        state_->storage_instance.emplace(std::move(created_storage.value()));
        state_->storage_closed = false;
        auto handle = state_->storage_instance->query<storage::Interface>();
        if (!handle.ok()) {
            return handle.status();
        }
        auto storage_handle = std::make_shared<storage::Handle>(std::move(handle.value()));
        auto prepared = output->execute(
            output, [output, native = std::move(storage_handle), envelope = options.envelope,
                     volume = options.volume, identity = options.identity,
                     catalog = options.max_prepared_descriptors, deadline]() mutable {
                auto writer = PipelineWriter::prepare(
                    std::move(*native), envelope, volume, output->instance->gate,
                    identity.instance_id, catalog, deadline);
                if (!writer.ok()) {
                    return Result<void>(writer.status());
                }
                output->writer = std::move(writer.value());
                return Result<void>{};
            });
        if (!prepared.ok()) {
            return prepared.status();
        }
        auto acquisition = options.plugins->load(
            options.definition.acquisition.plugin_id,
            options.definition.acquisition.plugin_version);
        if (!acquisition.ok()) {
            return acquisition.status();
        }
        state_->acquisition_library.emplace(std::move(acquisition.value()));
        auto identity = options.manifest;
        identity.acquisition = state_->acquisition_library->binary();
        identity.storage = state_->storage_library->binary();
        identity.playback_anchor = options.clock->anchor();
        identity.volume_id = options.volume->identity();
        if (!(identity.task_id == options.identity.task_id) ||
            identity.pipeline_id != options.identity.pipeline_id ||
            identity.instance_id != to_string(options.identity.instance_id) ||
            identity.control_generation != options.identity.control_generation) {
            return Status(StatusCode::InvalidArgument, {});
        }
        auto registration = options.ledger->reserve_registration(identity.segment_id);
        if (!registration.ok()) {
            return registration.status();
        }
        std::vector<StreamId> streams;
        for (const auto& binding : output->instance->streams.bindings()) {
            streams.push_back(binding.stream_id);
        }
        auto segment = RecordingSegment::prepare(
            std::move(identity), std::move(streams), options.envelope,
            output->writer->capacity_model(), options.manifest_limits, *options.capacity,
            std::move(registration.value()), sample.value().monotonic_ns);
        if (!segment.ok()) {
            return segment.status();
        }
        std::atomic_store(&output->segment, segment.value());
        for (const auto& binding : output->instance->streams.bindings()) {
            auto* stream = output->instance->streams.find(binding.stream_id);
            std::lock_guard<std::mutex> lock(stream->mutex);
            stream->segment = segment.value();
        }
        if (output->instance->gate.admission_fenced()) {
            segment.value()->close_admission();
            return Status(StatusCode::Conflict, {});
        }
        plugin::ServiceRegistry services;
        auto provided = services.add_shared<acquisition::Service>(output->instance);
        if (!provided.ok()) {
            return provided.status();
        }
        frozen = services.freeze();
        if (!frozen.ok()) {
            return frozen.status();
        }
        auto created_source = state_->acquisition_library->create_instance(
            services, options.definition.acquisition.json);
        if (!created_source.ok()) {
            state_->creation_unresolved = true;
            return created_source.status();
        }
        state_->acquisition_instance.emplace(std::move(created_source.value()));
        state_->source_closed = false;
        state_->source_quiesced = false;
        auto source = state_->acquisition_instance->query<acquisition::Interface>();
        if (!source.ok()) {
            return source.status();
        }
        state_->acquisition.emplace(std::move(source.value()));
        const auto& bindings = output->instance->streams.bindings();
        const acquisition::PrepareConfig config{
            {sizeof(config)},
            bindings.data(),
            static_cast<std::uint32_t>(bindings.size()),
            options.limits.callback_concurrency};
        auto result = state_->acquisition->prepare(config, deadline);
        if (!result.ok()) {
            return result.status();
        }
        result = state_->acquisition->start(deadline);
        if (!result.ok()) {
            return result.status();
        }
        result = output->instance->wait_ready(state_->policies, deadline);
        if (!result.ok()) {
            return result.status();
        }
        // Prepared is the completed readiness boundary, not entry into Native
        // preparation. Task persists this actual boundary before authorizing Open.
        sample = options.receive_clock();
        if (!sample.ok()) {
            return sample.status();
        }
        if (!options.clock->snapshot().ok()) {
            return Status(StatusCode::InvalidState, {});
        }
        state_->prepared_at = sample.value().monotonic_ns;
        state_->prepared = true;
        return PreparedPipeline{
            options.identity.pipeline_id, options.identity.instance_id,
            options.identity.control_generation, sample.value().monotonic_ns,
            options.clock->anchor().boot_id};
    } catch (const std::bad_alloc&) {
        (void)state_->output->instance->establish_control_fence();
        return Status(StatusCode::ResourceExhausted, {});
    }
}

GateOpenResult PipelineController::open_admission(const OpenAuthorization& authorization) {
    const auto& identity = state_->options.identity;
    if (!state_->prepared.load() || !(authorization.task_id == identity.task_id) ||
        authorization.pipeline_id != identity.pipeline_id ||
        !(authorization.instance_id == identity.instance_id) ||
        authorization.control_generation != identity.control_generation ||
        authorization.effective_config_id != state_->options.manifest.effective_config_id ||
        authorization.prepared_monotonic != state_->prepared_at.load() ||
        authorization.boot_id != state_->options.clock->anchor().boot_id ||
        authorization.opened_monotonic < authorization.prepared_monotonic) {
        return GateOpenResult::Obsolete;
    }
    std::lock_guard<std::mutex> lock(state_->open_mutex);
    if (!state_->options.clock->snapshot().ok()) {
        return GateOpenResult::Obsolete;
    }
    if (state_->opened) {
        if (state_->opened_at != authorization.opened_monotonic ||
            state_->recording_base != authorization.recording_base) {
            return GateOpenResult::Obsolete;
        }
        return state_->output->instance->gate.open_admission(
            identity.instance_id, identity.control_generation);
    }
    if (!state_->output->instance->ready(state_->policies)) {
        return GateOpenResult::Obsolete;
    }
    auto timeline = state_->output->instance->freeze_open_timeline(
        authorization.opened_monotonic, authorization.recording_base);
    if (!timeline.ok()) {
        return GateOpenResult::Obsolete;
    }
    const auto result = state_->output->instance->gate.open_admission(
        identity.instance_id, identity.control_generation);
    if (result == GateOpenResult::Opened) {
        state_->opened = true;
        state_->opened_at = authorization.opened_monotonic;
        state_->recording_base = authorization.recording_base;
    }
    return result;
}

Result<void> PipelineController::establish_control_fence() noexcept {
    return state_->output->instance->establish_control_fence();
}

Result<GateSnapshot> PipelineController::close_admission(ControlGeneration generation) {
    auto& instance = *state_->output->instance;
    auto advanced = instance.gate.advance_authority(instance.identity.instance_id, generation);
    if (!advanced.ok()) {
        return advanced.status();
    }
    (void)instance.establish_control_fence();
    auto closed = instance.gate.close_and_capture(instance.identity.instance_id);
    if (!closed.ok()) {
        return closed.status();
    }
    if (auto segment = std::atomic_load(&state_->output->segment)) {
        segment->close_admission();
    }
    return closed;
}

Result<std::optional<SealedPipelineSegment>> PipelineController::drain(plugin::Deadline deadline) {
    ControlCall call(state_->control);
    if (!call) {
        return Status(StatusCode::Conflict, {});
    }
    auto output = state_->output;
    if (state_->artifact_settled) {
        return std::optional<SealedPipelineSegment>{};
    }
    if (state_->artifact_work.load() != 0) {
        return Status(StatusCode::Conflict, {});
    }
    if (!output->instance->gate.admission_fenced()) {
        return Status(StatusCode::InvalidState, {});
    }
    if (!state_->source_quiesced && state_->acquisition) {
        auto stopped = state_->acquisition->quiesce(deadline);
        if (!stopped.ok()) {
            return stopped.status();
        }
        state_->source_quiesced = true;
    }
    if (!state_->source_quiesced && !state_->source_closed) {
        return Status(StatusCode::InvalidState, {});
    }
    auto idle = output->wait_idle(deadline);
    if (!idle.ok()) {
        return idle.status();
    }
    auto segment = std::atomic_load(&output->segment);
    if (!segment) {
        return std::optional<SealedPipelineSegment>{};
    }
    segment->close_admission();
    if (state_->artifact_handed_off) {
        return Status(StatusCode::Conflict, {});
    }
    // A failed wake is still an owned accepted set. A final explicit Writer
    // job drains it; an observer timeout never discards or classifies it.
    auto drained = output->execute(output, [output, segment, deadline] {
        while (output->instance->queue.snapshot().queued_records != 0) {
            auto step = output->writer->write_available(output->instance->queue, segment, deadline);
            if (!step.ok()) {
                return Result<void>(step.status());
            }
        }
        return Result<void>{};
    });
    if (!drained.ok()) {
        return drained.status();
    }
    const auto facts = segment->metadata.snapshot();
    if (facts.pending_records || facts.pending_descriptors || facts.native_inflight ||
        facts.unresolved_owners) {
        return Status(StatusCode::InvalidState, {});
    }
    if (facts.accepted == 0) {
        return std::optional<SealedPipelineSegment>{};
    }
    auto id = next_work_id();
    if (!id.ok()) {
        return id.status();
    }
    state_->artifact_work = id.value();
    if (facts.unknown != 0 || facts.written == 0) {
        return Status(StatusCode::DataLoss, {});
    }
    std::optional<storage::SegmentSealResult> seal;
    auto sealed = output->execute(output, [output, segment, deadline, &seal] {
        auto result = output->writer->seal(segment, deadline);
        if (!result.ok()) {
            return Result<void>(result.status());
        }
        seal.emplace(std::move(result.value()));
        return Result<void>{};
    });
    if (!sealed.ok()) {
        return sealed.status();
    }
    state_->artifact_handed_off = true;
    return std::optional<SealedPipelineSegment>{SealedPipelineSegment{
        std::move(segment), std::move(*seal), state_->options.envelope,
        state_->options.manifest_limits, id.value()}};
}

Result<void> PipelineController::acknowledge_artifact(
    std::uint64_t work_id, std::uint64_t segment_id) {
    ControlCall call(state_->control);
    if (!call) {
        return Status(StatusCode::Conflict, {});
    }
    if (segment_id != state_->options.manifest.segment_id || work_id == 0 ||
        !state_->artifact_handed_off) {
        return Status(StatusCode::Conflict, {});
    }
    auto segment = std::atomic_load(&state_->output->segment);
    if (!segment || !segment->capacity.snapshot().sealed) {
        return Status(StatusCode::InvalidState, {});
    }
    auto expected = work_id;
    if (!state_->artifact_work.compare_exchange_strong(expected, 0)) {
        return Status(StatusCode::Conflict, {});
    }
    state_->artifact_settled = true;
    return {};
}

Result<void> PipelineController::quiesce(plugin::Deadline deadline) {
    if (state_->artifact_work.load() != 0) {
        return Status(StatusCode::InvalidState, {});
    }
    return cleanup(deadline);
}

Result<void> PipelineController::cleanup(plugin::Deadline deadline) {
    ControlCall call(state_->control);
    if (!call) {
        return Status(StatusCode::Conflict, {});
    }
    auto output = state_->output;
    if (output->instance->gate.admission_fenced() && !state_->acquisition &&
        state_->acquisition_instance && !state_->source_closed) {
        auto closed = state_->acquisition_instance->close(deadline);
        if (!closed.ok()) {
            return closed.status();
        }
        state_->source_closed = true;
        state_->source_quiesced = true;
    }
    if (!output->instance->gate.admission_fenced() || !state_->source_quiesced ||
        output->instance->queue.snapshot().queued_records != 0) {
        return Status(StatusCode::InvalidState, {});
    }
    auto idle = output->wait_idle(deadline);
    if (!idle.ok()) {
        return idle.status();
    }
    if (output->writer && !state_->storage_closed) {
        auto quiet = output->execute(
            output, [output, deadline] { return output->writer->quiesce(deadline); });
        if (!quiet.ok()) {
            return quiet.status();
        }
    }
    if (state_->storage_instance && !state_->storage_closed) {
        auto closed = state_->storage_instance->close(deadline);
        if (!closed.ok()) {
            return closed.status();
        }
        state_->storage_closed = true;
    }
    if (state_->acquisition_instance && !state_->source_closed) {
        auto closed = state_->acquisition_instance->close(deadline);
        if (!closed.ok()) {
            return closed.status();
        }
        state_->source_closed = true;
    }
    // Failed SDK create may retain private Framework initialization/cleanup.
    // Without a public per-create completion handle, only a completed global
    // shutdown proves that unknown ownership gone; never invent per-Task idle.
    if (state_->creation_unresolved &&
        state_->options.plugins->phase() != plugin::PluginManagerPhase::Stopped) {
        return Status(StatusCode::InvalidState, {});
    }
    auto segment = std::atomic_load(&output->segment);
    if (segment && segment->metadata.snapshot().accepted == 0 && !state_->empty_settled) {
        auto verified = state_->options.volume->verify();
        if (!verified.ok()) {
            return verified.status();
        }
        const auto path = state_->options.volume->artifacts_directory() /
                          (std::to_string(state_->options.manifest.segment_id) + ".tmp");
        std::error_code error;
        const auto actual = std::filesystem::symlink_status(path, error);
        if ((error && error != std::errc::no_such_file_or_directory) ||
            actual.type() != std::filesystem::file_type::not_found) {
            return Status(StatusCode::DataLoss, {});
        }
        segment->close_admission();
        auto settled = segment->capacity.settle_aborted_after_cleanup();
        if (!settled.ok()) {
            return settled.status();
        }
        state_->empty_settled = true;
    }
    if (state_->artifact_work.load() != 0) {
        return Status(StatusCode::InvalidState, {});
    }
    return {};
}

Result<void> PipelineController::acknowledge_deleted(
    std::uint64_t work_id, std::uint64_t segment_id) {
    ControlCall call(state_->control);
    if (!call || work_id == 0 || segment_id != state_->options.manifest.segment_id ||
        state_->artifact_work.load() != work_id || !state_->storage_closed ||
        !state_->source_closed) {
        return Status(StatusCode::Conflict, {});
    }
    auto segment = std::atomic_load(&state_->output->segment);
    if (!segment) {
        return Status(StatusCode::InvalidState, {});
    }
    const auto facts = segment->metadata.snapshot();
    if (facts.pending_records || facts.pending_descriptors || facts.native_inflight ||
        facts.unresolved_owners || state_->output->instance->queue.snapshot().queued_records != 0) {
        return Status(StatusCode::InvalidState, {});
    }
    auto settled = segment->capacity.settle_aborted_after_cleanup();
    if (!settled.ok()) {
        return settled.status();
    }
    state_->artifact_work = 0;
    state_->artifact_settled = true;
    return {};
}

Result<PipelineSnapshot> PipelineController::snapshot() const {
    PipelineSnapshot result;
    result.identity = state_->options.identity;
    result.gate = state_->output->instance->gate.snapshot();
    result.queue = state_->output->instance->queue.snapshot();
    if (auto segment = std::atomic_load(&state_->output->segment)) {
        result.records = segment->metadata.snapshot();
        result.segment_id = segment->metadata.segment_id();
        result.quiescence.admission_candidates = segment->capacity.snapshot().candidates;
    }
    result.artifact_work_id = state_->artifact_work.load();
    auto callbacks = state_->output->instance->callback_facts();
    if (!callbacks.ok()) {
        return callbacks.status();
    }
    result.prepared = state_->prepared.load();
    result.failure = state_->output->failure.load();
    auto& quiet = result.quiescence;
    quiet.gate_closed = !result.gate.open;
    quiet.control_fenced = result.gate.control_fence;
    quiet.control_inflight = state_->control.load();
    {
        std::lock_guard<std::mutex> lock(state_->output->mutex);
        quiet.writer_inflight = state_->output->jobs + (state_->output->scheduled ? 1 : 0);
    }
    quiet.source_quiesced = state_->source_quiesced.load() && state_->source_closed.load();
    quiet.storage_closed = state_->storage_closed.load();
    quiet.callbacks = callbacks.value().callbacks;
    quiet.queued_records = result.queue.queued_records;
    quiet.unsettled_records = result.records.pending_records + result.records.unresolved_owners +
                              result.records.native_inflight;
    quiet.buffered_records = state_->output->instance->records.snapshot().records;
    quiet.unsettled_descriptors = result.records.pending_descriptors;
    quiet.commit_inflight = state_->artifact_work.load() != 0 ? 1 : 0;
    quiet.settled = quiet.gate_closed && quiet.control_fenced && result.gate.commit_inflight == 0 &&
                    quiet.control_inflight == 0 && quiet.writer_inflight == 0 &&
                    quiet.source_quiesced && quiet.storage_closed && quiet.callbacks == 0 &&
                    quiet.queued_records == 0 && result.queue.reserved_slots == 0 &&
                    quiet.unsettled_records == 0 && quiet.unsettled_descriptors == 0 &&
                    quiet.admission_candidates == 0 && quiet.buffered_records == 0 &&
                    quiet.commit_inflight == 0 &&
                    (!state_->creation_unresolved ||
                     state_->options.plugins->phase() == plugin::PluginManagerPhase::Stopped);
    return result;
}

Result<CallbackFacts> PipelineController::callback_facts() const {
    return state_->output->instance->callback_facts();
}

}  // namespace rodaq::pipeline
