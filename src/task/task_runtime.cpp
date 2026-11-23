#include "task_service_state.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <new>
#include <utility>

namespace rodaq::task {
namespace {

DataImpact combine(DataImpact left, DataImpact right) noexcept {
    return static_cast<DataImpact>(
        std::max(static_cast<unsigned>(left), static_cast<unsigned>(right)));
}

StreamRuntimeFacts stream_facts(
    const pipeline::StreamCallbackFacts& source, const pipeline::CallbackFacts& callbacks) {
    StreamRuntimeFacts result;
    result.entry_ready = callbacks.transport_ready && !callbacks.terminal_failure &&
                         source.descriptor_ready && source.stream_ready;
    result.source_ready = source.active_sources != 0;
    result.active_sources = source.active_sources;
    result.received_records = source.received_records;
    result.received_logical_bytes = source.received_logical_bytes;
    if (source.last_receive_realtime_ns) {
        result.last_receive_at =
            WallTime{std::chrono::nanoseconds{*source.last_receive_realtime_ns}};
    }
    result.accepted_records = source.submissions[RODAQ_SUBMIT_ACCEPTED];
    constexpr std::array<RecordRejectionReason, 7> reasons{
        RecordRejectionReason::GateClosed,       RecordRejectionReason::ObsoleteInstance,
        RecordRejectionReason::StaleDescriptor,  RecordRejectionReason::DescriptorNotReady,
        RecordRejectionReason::InvalidRecord,    RecordRejectionReason::RecordTooLarge,
        RecordRejectionReason::ResourceExhausted};
    for (std::size_t i = 0; i < reasons.size(); ++i) {
        const auto count = source.submissions[RODAQ_SUBMIT_GATE_CLOSED + i];
        if (count != 0) {
            result.rejections.push_back({reasons[i], count});
        }
    }
    result.native_dropped_records = source.reported_counts[RODAQ_ACQUISITION_EVENT_DROPPED];
    result.native_overflow_records = source.reported_counts[RODAQ_ACQUISITION_EVENT_OVERFLOW];
    result.evidence_complete = source.evidence_complete && callbacks.evidence_complete;
    return result;
}

}  // namespace

Result<TaskRuntimeSnapshot> capture_task_runtime(
    const TaskEntry& entry, const TaskConfig& config, const TaskSnapshot& snapshot) {
    try {
        TaskRuntimeSnapshot result;
        result.task_id = snapshot.task_id;
        result.active_config_version = snapshot.active_config_version;
        result.control_generation = snapshot.control_generation;
        result.snapshot_sequence = snapshot.snapshot_sequence;
        result.captured_at = snapshot.captured_at;
        if (snapshot.current_operation) {
            result.data_impact = snapshot.current_operation->data_impact;
        }
        std::vector<RunPipeline> observed;
        if (entry.execution) {
            std::lock_guard<std::mutex> lock(entry.execution->registration);
            if (entry.execution->control->active_config_version == snapshot.active_config_version) {
                observed = entry.execution->pipelines;
            }
        }
        std::vector<pipeline::PipelineSnapshot> pipelines;
        pipelines.reserve(observed.size());
        for (const auto& selected : observed) {
            auto actual = selected.controller->snapshot();
            if (!actual.ok()) {
                return actual.status();
            }
            pipelines.push_back(std::move(actual.value()));
        }
        result.pipelines.reserve(config.pipelines.size());
        for (const auto& definition : config.pipelines) {
            PipelineRuntimeSnapshot row;
            row.pipeline_id = definition.pipeline_id;
            row.required = definition.required;
            row.streams.reserve(definition.streams.size());
            for (const auto& stream : definition.streams) {
                row.streams.push_back({stream.stream_id, stream.stream_key, stream.name, {}});
            }
            for (std::size_t at = 0; at < pipelines.size(); ++at) {
                const auto& source = pipelines[at];
                if (source.identity.pipeline_id != definition.pipeline_id) {
                    continue;
                }
                auto callbacks = observed[at].controller->callback_facts();
                if (!callbacks.ok()) {
                    return callbacks.status();
                }
                PipelineRuntimeFacts facts;
                facts.instance_id = pipeline::to_string(source.identity.instance_id);
                facts.control_generation = source.identity.control_generation;
                facts.prepare_status = observed[at].prepare_status;
                if (facts.prepare_status == StatusCode::Ok) {
                    facts.effective_config_id = observed[at].effective_config_id;
                }
                facts.admission_open = source.gate.open && !source.gate.control_fence;
                facts.quiescent = source.quiescence.settled;
                facts.writer_failure = source.failure;
                facts.transport_ready =
                    callbacks.value().transport_ready && !source.quiescence.source_quiesced;
                facts.acquisition_terminal_failure = callbacks.value().terminal_failure;
                facts.accepted_records = source.records.accepted;
                facts.written_records = source.records.written;
                facts.known_failed_records = source.records.known_failed;
                facts.persistence_unknown_records = source.records.unknown;
                facts.unsettled_records = source.quiescence.unsettled_records;
                facts.queued_records = source.queue.queued_records;
                facts.queued_logical_bytes = source.queue.queued_logical_bytes;
                facts.commit_items = source.quiescence.commit_inflight;
                if (facts.persistence_unknown_records != 0 ||
                    source.records.unresolved_owners != 0) {
                    facts.data_impact = DataImpact::PersistenceUnknown;
                } else if (facts.known_failed_records != 0) {
                    facts.data_impact = DataImpact::AcceptedKnownFailed;
                }
                for (auto& stream : row.streams) {
                    const auto found = std::find_if(
                        callbacks.value().streams.begin(), callbacks.value().streams.end(),
                        [&](const auto& item) { return item.stream_id == stream.stream_id; });
                    if (found != callbacks.value().streams.end()) {
                        stream.facts = stream_facts(*found, callbacks.value());
                        if (source.quiescence.source_quiesced) {
                            stream.facts->entry_ready = false;
                            stream.facts->source_ready = false;
                        }
                        if (stream.facts->native_dropped_records != 0 ||
                            stream.facts->native_overflow_records != 0) {
                            facts.data_impact =
                                combine(facts.data_impact, DataImpact::PreAdmissionOnly);
                        }
                    }
                }
                result.evidence_complete = result.evidence_complete &&
                                           callbacks.value().evidence_complete &&
                                           source.records.unresolved_owners == 0;
                result.data_impact = combine(result.data_impact, facts.data_impact);
                row.facts = std::move(facts);
                result.freshness = SnapshotFreshness::Fresh;
                break;
            }
            result.pipelines.push_back(std::move(row));
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

void project_runtime_rates(PublishedTask& current, const PublishedTask* previous) noexcept {
    auto& next = current.runtime;
    if (previous == nullptr || next.freshness != SnapshotFreshness::Fresh ||
        previous->runtime.freshness != SnapshotFreshness::Fresh ||
        next.active_config_version != previous->runtime.active_config_version ||
        next.pipelines.size() != previous->runtime.pipelines.size()) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        current.observed_at - previous->observed_at);
    if (elapsed <= std::chrono::nanoseconds::zero()) {
        return;
    }
    next.window = elapsed;
    const auto seconds = std::chrono::duration<long double>(elapsed).count();
    long double accepted = 0;
    bool complete = next.evidence_complete && previous->runtime.evidence_complete;
    for (std::size_t i = 0; i < next.pipelines.size(); ++i) {
        auto& row = next.pipelines[i];
        const auto& old = previous->runtime.pipelines[i];
        if (row.pipeline_id != old.pipeline_id || !row.facts || !old.facts ||
            row.facts->instance_id != old.facts->instance_id ||
            row.facts->accepted_records < old.facts->accepted_records ||
            row.streams.size() != old.streams.size()) {
            complete = false;
            continue;
        }
        accepted += row.facts->accepted_records - old.facts->accepted_records;
        for (std::size_t j = 0; j < row.streams.size(); ++j) {
            auto& stream = row.streams[j];
            const auto& before = old.streams[j];
            if (stream.stream_id != before.stream_id || !stream.facts || !before.facts ||
                !stream.facts->evidence_complete || !before.facts->evidence_complete ||
                stream.facts->received_records < before.facts->received_records ||
                stream.facts->received_logical_bytes < before.facts->received_logical_bytes) {
                continue;
            }
            const auto records = stream.facts->received_records - before.facts->received_records;
            const auto bytes =
                stream.facts->received_logical_bytes - before.facts->received_logical_bytes;
            stream.facts->receiving = records != 0;
            stream.facts->received_records_per_second = static_cast<double>(records / seconds);
            stream.facts->received_logical_bytes_per_second = static_cast<double>(bytes / seconds);
        }
    }
    if (complete) {
        next.accepted_records_per_second = static_cast<double>(accepted / seconds);
    }
}

}  // namespace rodaq::task
