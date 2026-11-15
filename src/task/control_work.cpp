#include "task_controller.hpp"

#include "task_store.hpp"
#include "../pipeline/stream_identity_store.hpp"

#include <limits>
#include <new>
#include <utility>

namespace rodaq::task {
namespace {

Result<void> prepare_config(
    TaskStore& store, pipeline::StreamIdentityStore& identities, const ControlWork& work) {
    const auto& change = *work.config_change;
    if (change.projection.config_version != work.candidate->active_config_version) {
        return Status(StatusCode::DataLoss, {});
    }
    auto created = store.create_config(change.assigned.config);
    if (!created.ok()) {
        return created.status();
    }
    if (created.value().version != work.candidate->active_config_version) {
        return Status(StatusCode::DataLoss, {});
    }
    return identities.apply_config_change(change.projection, change.expected_identity_revision);
}

// Control alone selects active Config. A failed Configure may leave the
// identity projection ahead; select actual active bindings while preserving
// every durable high-water and reserved Sequence/Descriptor range.
Result<void> reconcile_config(
    TaskStore& store, pipeline::StreamIdentityStore& identities, ControlWorkResult& result,
    const TaskConfigLimits& limits, bool uncertain_identity) {
    if (!result.authority.ok()) {
        return result.authority.status();
    }
    auto state = [&]() -> Result<pipeline::StreamIdentityState> {
        if (!uncertain_identity) {
            return identities.load();
        }
        auto reconciled = identities.reconcile();
        if (!reconciled.ok()) {
            return reconciled.status();
        }
        if (!reconciled.value()) {
            return Status(StatusCode::DataLoss, {});
        }
        return std::move(*reconciled.value());
    }();
    if (!state.ok()) {
        return state.status();
    }
    auto config = store.load_config(result.authority.value().active_config_version);
    if (!config.ok()) {
        return config.status();
    }
    AssignedConfig selected{
        std::move(config.value().config), state.value().pipeline_id_high_water,
        state.value().stream_id_high_water};
    auto projection = identity_projection(selected, limits);
    if (!projection.ok()) {
        return projection.status();
    }
    auto applied =
        identities.apply_config_change(projection.value(), state.value().record_revision);
    if (!applied.ok()) {
        return applied.status();
    }
    auto actual = identities.load();
    if (!actual.ok()) {
        return actual.status();
    }
    auto active = std::make_shared<const TaskConfig>(std::move(selected.config));
    result.identities = std::move(actual.value());
    result.active_config = std::move(active);
    return {};
}

}  // namespace

ControlWorkResult execute_control_work(
    TaskStore& store, const ControlWork& work, pipeline::StreamIdentityStore* identities,
    const TaskConfigLimits& limits) {
    if (work.work_id == 0 || !work.candidate ||
        work.expected_revision == std::numeric_limits<std::uint64_t>::max() ||
        work.candidate->record_revision != work.expected_revision + 1 ||
        (work.kind != ControlWorkKind::Publish && work.kind != ControlWorkKind::Reconcile) ||
        (work.config_change && !identities)) {
        return {work.work_id, Status(StatusCode::InvalidArgument, {}), {}};
    }
    try {
        bool uncertain_identity = work.kind == ControlWorkKind::Reconcile;
        auto result = [&]() -> ControlWorkResult {
            Status failure;
            if (work.kind == ControlWorkKind::Publish) {
                // Allocate the exact response before any publish can have effects.
                auto candidate = *work.candidate;
                if (work.config_change) {
                    auto prepared = prepare_config(store, *identities, work);
                    if (!prepared.ok()) {
                        uncertain_identity = true;
                        failure = std::move(prepared).status();
                    }
                }
                if (failure.ok()) {
                    auto published = store.publish_control(candidate, work.expected_revision);
                    if (published.ok()) {
                        return {work.work_id, std::move(candidate), {}};
                    }
                    failure = std::move(published).status();
                }
            }
            auto reconciled = store.reconcile();
            if (!reconciled.ok()) {
                return {work.work_id, std::move(reconciled).status(), std::move(failure)};
            }
            if (!reconciled.value()) {
                return {
                    work.work_id, Status(StatusCode::DataLoss, "Task Control authority missing"),
                    std::move(failure)};
            }
            return {work.work_id, std::move(*reconciled.value()), std::move(failure)};
        }();
        if (identities && result.authority.ok()) {
            auto reconciled =
                reconcile_config(store, *identities, result, limits, uncertain_identity);
            if (!reconciled.ok()) {
                return {work.work_id, reconciled.status(), result.publication_failure};
            }
        }
        return result;
    } catch (const std::bad_alloc&) {
        // A local allocation failure is not evidence of a definite rollback.
        return {work.work_id, Status(StatusCode::ResourceExhausted, {}), {}};
    }
}

}  // namespace rodaq::task
