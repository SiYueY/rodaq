#include "task_service_state.hpp"

#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include "../work_id.hpp"

namespace rodaq {

Result<void> TaskService::State::enqueue_cleanup(
    const std::shared_ptr<task::TaskEntry>& entry, std::shared_ptr<Work> body, bool allow_defer) {
    try {
        auto identity = next_work_id();
        if (!identity.ok()) {
            return identity.status();
        }
        auto frame = std::make_shared<CleanupFrame>();
        frame->owner = shared_from_this();
        frame->entry = entry;
        frame->body = std::move(body);
        frame->work_id = identity.value();
        task::CleanupWork work{identity.value(), std::move(frame)};
        {
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            if (entry->cleanup_active_id != 0 || entry->cleanup_ready.frame) {
                if (!allow_defer) {
                    return Status(StatusCode::Conflict, {});
                }
                if (entry->cleanup_pending.frame) {
                    return Status(StatusCode::ResourceExhausted, {});
                }
                entry->cleanup_pending = std::move(work);
            } else {
                entry->cleanup_ready = std::move(work);
            }
        }
        // An owned, durably accepted flight survives a full/closed lane. Its
        // observer may time out; the same frame remains available for retry.
        (void)retry_cleanup(entry);
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> TaskService::State::retry_cleanup(const std::shared_ptr<task::TaskEntry>& entry) {
    task::CleanupWork work;
    {
        std::lock_guard<std::mutex> lock(entry->control_mutex);
        if (entry->cleanup_active_id != 0 || !entry->cleanup_ready.frame) {
            return {};
        }
        work = std::exchange(entry->cleanup_ready, task::CleanupWork{});
        entry->cleanup_active_id = work.work_id;
    }
    auto admitted = options->execution->cleanup_lane->submit(work.frame);
    if (!admitted.ok()) {
        std::lock_guard<std::mutex> lock(entry->control_mutex);
        if (entry->cleanup_active_id != work.work_id || entry->cleanup_ready.frame) {
            std::terminate();
        }
        entry->cleanup_active_id = 0;
        entry->cleanup_ready = std::move(work);
    }
    return admitted;
}

void TaskService::State::CleanupFrame::run() noexcept {
    body->run();
    // Actual body/input destruction precedes releasing this Task's Native
    // execution slot. It runs outside Task locks and can retain late owners.
    body.reset();
    {
        std::lock_guard<std::mutex> lock(entry->control_mutex);
        if (entry->cleanup_active_id != work_id || entry->cleanup_ready.frame) {
            std::terminate();
        }
        entry->cleanup_active_id = 0;
        entry->cleanup_ready = std::exchange(entry->cleanup_pending, task::CleanupWork{});
    }
    (void)owner->retry_cleanup(entry);
}

}  // namespace rodaq
