#include "task_controller.hpp"

#include "task_store.hpp"

#include <limits>
#include <new>
#include <utility>

namespace rodaq::task {

ControlWorkResult execute_control_work(TaskStore& store, const ControlWork& work) {
    if (work.work_id == 0 || !work.candidate ||
        work.expected_revision == std::numeric_limits<std::uint64_t>::max() ||
        work.candidate->record_revision != work.expected_revision + 1 ||
        (work.kind != ControlWorkKind::Publish && work.kind != ControlWorkKind::Reconcile)) {
        return {work.work_id, Status(StatusCode::InvalidArgument, {}), {}};
    }
    try {
        Status failure;
        if (work.kind == ControlWorkKind::Publish) {
            // Retain the response before any write can become durable. A
            // successful Store return already proves this exact candidate.
            auto candidate = *work.candidate;
            auto published = store.publish_control(candidate, work.expected_revision);
            if (published.ok()) {
                return {work.work_id, std::move(candidate), {}};
            }
            failure = std::move(published).status();
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
    } catch (const std::bad_alloc&) {
        // The owner must reconcile a possibly effected publication; it never
        // converts a local result-allocation failure into a definite rollback.
        return {work.work_id, Status(StatusCode::ResourceExhausted, {}), {}};
    }
}

}  // namespace rodaq::task
