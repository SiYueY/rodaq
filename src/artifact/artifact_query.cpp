#include "artifact_catalog.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../persistence/decimal.hpp"
#include "../persistence/json.hpp"
#include "../uuid.hpp"
#include "catalog_state.hpp"

namespace rodaq::artifact {
namespace {

struct PageKey {
    WallTime committed_at;
    std::uint64_t id{0};
};

bool earlier(const ArtifactSummary& a, const ArtifactSummary& b) noexcept {
    return a.committed_at > b.committed_at ||
           (a.committed_at == b.committed_at && a.artifact_id.value < b.artifact_id.value);
}

Result<std::string> filter_digest(const ArtifactQuery& query) {
    if (query.limit == 0 || query.limit > 256 || query.cursor.size() > 256 ||
        (query.task_id && !uuid::nonzero(query.task_id->value)) ||
        (query.pipeline_id && *query.pipeline_id == 0) ||
        (query.format && (query.format->empty() || query.format->size() > 128)) ||
        (query.recording_range &&
         (!query.task_id || query.recording_range->begin_ns > query.recording_range->end_ns)) ||
        (query.receive_wall_time_range &&
         query.receive_wall_time_range->begin > query.receive_wall_time_range->end) ||
        (query.committed_at && query.committed_at->begin > query.committed_at->end)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    using persistence::Json;
    const auto wall_range = [](const auto& range) -> Json {
        return range ? Json{{"begin", std::to_string(range->begin.time_since_epoch().count())},
                            {"end", std::to_string(range->end.time_since_epoch().count())}}
                     : Json(nullptr);
    };
    return persistence::semantic_digest(
        {{"task_id", query.task_id ? Json(to_string(*query.task_id)) : Json(nullptr)},
         {"pipeline_id",
          query.pipeline_id ? Json(std::to_string(*query.pipeline_id)) : Json(nullptr)},
         {"format", query.format ? Json(*query.format) : Json(nullptr)},
         {"recording_range", query.recording_range
                                 ? Json{{"begin", std::to_string(query.recording_range->begin_ns)},
                                        {"end", std::to_string(query.recording_range->end_ns)}}
                                 : Json(nullptr)},
         {"receive_wall_time_range", wall_range(query.receive_wall_time_range)},
         {"committed_at", wall_range(query.committed_at)}},
        "rodaq.artifact-query.v1");
}

Result<std::optional<PageKey>> parse_cursor(std::string_view cursor, std::string_view digest) {
    if (cursor.empty()) {
        return std::optional<PageKey>{};
    }
    const std::string prefix = "aq1:" + std::string(digest.substr(7)) + ":";
    if (cursor.substr(0, prefix.size()) != prefix) {
        return Status(StatusCode::InvalidArgument, {});
    }
    cursor.remove_prefix(prefix.size());
    const auto delimiter = cursor.find(':');
    if (delimiter == std::string_view::npos) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto wall = persistence::signed_decimal(cursor.substr(0, delimiter));
    auto id = persistence::unsigned_decimal(cursor.substr(delimiter + 1));
    if (!wall.ok() || !id.ok() || id.value() == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return std::optional<PageKey>{
        PageKey{WallTime{std::chrono::nanoseconds{wall.value()}}, id.value()}};
}

bool matches(
    const ArtifactSummary& value, const ArtifactQuery& query, const std::optional<PageKey>& key) {
    return (!query.task_id || value.task_id == *query.task_id) &&
           (!query.pipeline_id || value.pipeline_id == *query.pipeline_id) &&
           (!query.format || value.format == *query.format) &&
           (!query.recording_range ||
            (value.recording_range.end_ns >= query.recording_range->begin_ns &&
             value.recording_range.begin_ns <= query.recording_range->end_ns)) &&
           (!query.receive_wall_time_range ||
            (value.receive_wall_time_range.end >= query.receive_wall_time_range->begin &&
             value.receive_wall_time_range.begin <= query.receive_wall_time_range->end)) &&
           (!query.committed_at || (value.committed_at >= query.committed_at->begin &&
                                    value.committed_at <= query.committed_at->end)) &&
           (!key || value.committed_at < key->committed_at ||
            (value.committed_at == key->committed_at && value.artifact_id.value > key->id));
}

}  // namespace

Result<ArtifactPage> ArtifactCatalog::query(const ArtifactQuery& query) const {
    auto state = state_;
    try {
        auto digest = filter_digest(query);
        if (!digest.ok()) {
            return digest.status();
        }
        auto key = parse_cursor(query.cursor, digest.value());
        if (!key.ok()) {
            return key.status();
        }
        std::vector<std::shared_ptr<CatalogEntry>> selected;
        selected.reserve(state->slots.size());
        {
            std::lock_guard<std::mutex> lock(state->index);
            if (state->options.volume->protection() != StatusCode::Ok) {
                return Status(state->options.volume->protection(), {});
            }
            for (const auto& slot : state->slots) {
                if (slot.visible && slot.entry &&
                    matches(slot.entry->info.summary, query, key.value())) {
                    selected.push_back(slot.entry);
                }
            }
        }
        std::sort(selected.begin(), selected.end(), [](const auto& a, const auto& b) {
            return earlier(a->info.summary, b->info.summary);
        });
        ArtifactPage result;
        const auto count = std::min<std::size_t>(selected.size(), query.limit);
        result.items.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            result.items.push_back(selected[i]->info.summary);
        }
        result.has_more = selected.size() > count;
        if (result.has_more) {
            const auto& last = result.items.back();
            result.next_cursor = "aq1:" + digest.value().substr(7) + ":" +
                                 std::to_string(last.committed_at.time_since_epoch().count()) +
                                 ":" + std::to_string(last.artifact_id.value);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<ArtifactInfo> ArtifactCatalog::get(ArtifactId id) const {
    if (id.value == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto state = state_;
    try {
        std::lock_guard<std::mutex> lock(state->index);
        if (state->options.volume->protection() != StatusCode::Ok) {
            return Status(state->options.volume->protection(), {});
        }
        const auto at = state->find(id.value);
        if (at == state->slots.size()) {
            return Status(StatusCode::NotFound, {});
        }
        const auto& slot = state->slots[at];
        if (!slot.visible || !slot.entry) {
            return Status(slot.failure == StatusCode::Ok ? StatusCode::NotFound : slot.failure, {});
        }
        return slot.entry->info;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::artifact
