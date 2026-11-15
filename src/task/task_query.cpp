#include "task_service_state.hpp"

#include <algorithm>
#include <atomic>
#include <new>
#include <optional>
#include <string_view>

namespace rodaq {

using task::TaskEntry;

Result<TaskSnapshot> TaskService::get_task(TaskId id) const {
    auto entry = state_->find(id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    try {
        return std::atomic_load(&entry->published)->snapshot;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<TaskConfig> TaskService::get_active_config(TaskId id) const {
    auto entry = state_->find(id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    try {
        return *std::atomic_load(&entry->published)->config;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<OperationSnapshot> TaskService::get_operation(TaskId id, OperationId operation) const {
    auto entry = state_->find(id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    try {
        const auto published = std::atomic_load(&entry->published);
        for (const auto& item : published->control->operation_history.entries) {
            if (item.snapshot.operation_id == operation) {
                return item.snapshot;
            }
        }
        return Status(StatusCode::NotFound, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<OperationPage> TaskService::list_operations(TaskId id, const PageRequest& request) const {
    auto entry = state_->find(id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    const auto published = std::atomic_load(&entry->published);
    return task::operation_page(id, published->control->operation_history, request);
}

Result<TaskPage> TaskService::list_tasks(const PageRequest& request) const {
    if (request.limit == 0 || request.limit > 256 || request.cursor.size() > 64) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        std::optional<TaskId> after;
        if (!request.cursor.empty()) {
            constexpr std::string_view prefix = "tasks:v1:";
            if (request.cursor.substr(0, prefix.size()) != prefix) {
                return Status(StatusCode::InvalidArgument, {});
            }
            auto parsed = parse_task_id(std::string_view(request.cursor).substr(prefix.size()));
            if (!parsed.ok()) {
                return Status(StatusCode::InvalidArgument, {});
            }
            after = parsed.value();
        }
        std::vector<std::shared_ptr<TaskEntry>> selected;
        selected.reserve(request.limit);
        TaskPage page;
        {
            std::lock_guard<std::mutex> lock(state_->catalog_mutex);
            for (const auto& entry : state_->entries) {
                if (after && !(after.value() < entry->id)) {
                    continue;
                }
                if (selected.size() == request.limit) {
                    page.has_more = true;
                    break;
                }
                selected.push_back(entry);
            }
        }
        page.items.reserve(selected.size());
        for (const auto& entry : selected) {
            page.items.push_back(
                {std::atomic_load(&entry->published)->snapshot,
                 {},
                 {},
                 SnapshotFreshness::NotObserved});
        }
        if (page.has_more) {
            page.next_cursor = "tasks:v1:" + to_string(page.items.back().task.task_id);
        }
        return page;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq
