#include "stream_identity_store.hpp"

#include "../persistence/json.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace rodaq::pipeline {
namespace {

Result<std::string> document_digest(std::string_view bytes, std::size_t maximum) {
    auto parsed = persistence::parse_json(
        bytes, {stream_identity_max_bytes(maximum), 4, 128, 12, maximum, 16 + maximum * 7});
    if (!parsed.ok()) {
        return std::move(parsed).status();
    }
    const auto found = parsed.value().find("content_digest");
    if (found == parsed.value().end() || !found->is_string()) {
        return Status(StatusCode::DataLoss, {});
    }
    return found->get_ref<const std::string&>();
}

auto find_stream(StreamIdentityState& state, StreamId stream_id) {
    return std::lower_bound(
        state.streams.begin(), state.streams.end(), stream_id,
        [](const auto& stream, StreamId id) { return stream.stream_id < id; });
}

auto find_stream(const StreamIdentityState& state, StreamId stream_id) {
    return std::lower_bound(
        state.streams.begin(), state.streams.end(), stream_id,
        [](const auto& stream, StreamId id) { return stream.stream_id < id; });
}

bool active_bindings_match(
    const StreamIdentityState& state, const StreamIdentityProjection& projection) noexcept {
    std::size_t index = 0;
    for (const auto& stream : state.streams) {
        if (!stream.retired) {
            if (index == projection.streams.size() ||
                stream.stream_id != projection.streams[index].stream_id ||
                stream.stream_key != projection.streams[index].stream_key) {
                return false;
            }
            ++index;
        }
    }
    return index == projection.streams.size();
}

bool matches_projection(
    const StreamIdentityState& state, const StreamIdentityProjection& projection) noexcept {
    return state.observed_config_version == projection.config_version &&
           state.pipeline_id_high_water == projection.pipeline_id_high_water &&
           state.stream_id_high_water == projection.stream_id_high_water &&
           active_bindings_match(state, projection);
}

Result<void> successor(const StreamIdentityState& old_state, const StreamIdentityState& next) {
    if (old_state.record_revision == std::numeric_limits<std::uint64_t>::max()) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    if (!(old_state.task_id == next.task_id) ||
        next.record_revision != old_state.record_revision + 1 ||
        next.pipeline_id_high_water < old_state.pipeline_id_high_water ||
        next.stream_id_high_water < old_state.stream_id_high_water) {
        return Status(StatusCode::DataLoss, {});
    }
    for (const auto& old_stream : old_state.streams) {
        const auto stream = find_stream(next, old_stream.stream_id);
        if (stream == next.streams.end() || stream->stream_id != old_stream.stream_id ||
            stream->stream_key != old_stream.stream_key ||
            stream->reserved_end_exclusive < old_stream.reserved_end_exclusive ||
            stream->reserved_descriptor_end_exclusive <
                old_stream.reserved_descriptor_end_exclusive) {
            return Status(StatusCode::DataLoss, {});
        }
    }
    return {};
}

}  // namespace

struct StreamIdentityStore::State {
    std::filesystem::path directory;
    std::string name;
    TaskId task_id;
    IdentityStoreMode mode{IdentityStoreMode::Existing};
    std::size_t maximum{0};
    persistence::FileFaultHook faults;
    std::mutex execution;
    int lease{-1};
    bool protected_state{true};
    std::optional<StreamIdentityState> current;
    std::string content_digest;
    std::string pending_digest;
    std::string retained_temporary;

    ~State() noexcept {
        if (lease >= 0) {
            close(lease);
        }
    }

    Result<void> check_actual() {
        auto bytes = persistence::read_file(directory, name, stream_identity_max_bytes(maximum));
        if (!bytes.ok()) {
            protected_state = true;
            return std::move(bytes).status();
        }
        auto decoded = decode_stream_identity(bytes.value(), maximum);
        if (!decoded.ok()) {
            protected_state = true;
            return std::move(decoded).status();
        }
        auto digest = document_digest(bytes.value(), maximum);
        if (!digest.ok()) {
            return std::move(digest).status();
        }
        if (digest.value() != content_digest) {
            protected_state = true;
            return Status(
                StatusCode::DataLoss, "Stream identity authority changed outside its owner");
        }
        return {};
    }

    Result<void> publish(StreamIdentityState next) {
        if (current) {
            auto ordered = successor(*current, next);
            if (!ordered.ok()) {
                return std::move(ordered).status();
            }
            auto actual = check_actual();
            if (!actual.ok()) {
                return std::move(actual).status();
            }
        }
        auto bytes = encode_stream_identity(next, maximum);
        if (!bytes.ok()) {
            return std::move(bytes).status();
        }
        auto digest = document_digest(bytes.value(), maximum);
        if (!digest.ok()) {
            return std::move(digest).status();
        }
        auto published = persistence::publish_file(
            directory, name, bytes.value(),
            current ? persistence::FilePublishMode::Replace : persistence::FilePublishMode::Create,
            stream_identity_max_bytes(maximum), faults);
        if (published.disposition != persistence::FileDisposition::Durable) {
            protected_state = true;
            pending_digest = std::move(digest).value();
            retained_temporary = std::move(published.retained_temporary);
            return std::move(published.status);
        }
        current = std::move(next);
        content_digest = std::move(digest).value();
        pending_digest.clear();
        return {};
    }

    Result<std::optional<StreamIdentityState>> recover() {
        protected_state = true;
        auto bytes = persistence::read_file(directory, name, stream_identity_max_bytes(maximum));
        std::optional<StreamIdentityState> recovered;
        std::string digest;
        if (bytes.ok()) {
            auto state = decode_stream_identity(bytes.value(), maximum);
            if (!state.ok()) {
                return std::move(state).status();
            }
            if (!(state.value().task_id == task_id)) {
                return Status(StatusCode::DataLoss, {});
            }
            auto actual_digest = document_digest(bytes.value(), maximum);
            if (!actual_digest.ok()) {
                return std::move(actual_digest).status();
            }
            digest = std::move(actual_digest).value();
            if ((!current && !pending_digest.empty() && digest != pending_digest) ||
                (current && digest != content_digest && digest != pending_digest)) {
                return Status(StatusCode::DataLoss, "Unexpected Stream identity candidate");
            }
            if (current && digest != content_digest) {
                auto ordered = successor(*current, state.value());
                if (!ordered.ok()) {
                    return std::move(ordered).status();
                }
            }
            auto synced = persistence::sync_file(directory, name, faults);
            if (!synced.ok()) {
                return std::move(synced).status();
            }
            recovered = std::move(state).value();
        } else if (bytes.status().code() != StatusCode::NotFound) {
            return std::move(bytes).status();
        } else if (current || mode == IdentityStoreMode::Existing) {
            return Status(StatusCode::DataLoss, "Stream identity authority is missing");
        }
        if (!retained_temporary.empty()) {
            std::error_code error;
            std::filesystem::remove(directory / retained_temporary, error);
            if (error) {
                return Status(StatusCode::Unavailable, {});
            }
        }
        auto synced = persistence::sync_directory(directory, faults);
        if (!synced.ok()) {
            return std::move(synced).status();
        }
        auto response = recovered;
        current = std::move(recovered);
        content_digest = std::move(digest);
        retained_temporary.clear();
        pending_digest.clear();
        protected_state = false;
        return response;
    }
};

StreamIdentityStore::StreamIdentityStore(std::unique_ptr<State> state) noexcept
: state_(std::move(state)) {}

StreamIdentityStore::~StreamIdentityStore() noexcept = default;

Result<std::unique_ptr<StreamIdentityStore>> StreamIdentityStore::open(
    const std::filesystem::path& directory, TaskId task_id, IdentityStoreMode mode,
    std::size_t max_identities, persistence::FileFaultHook faults) {
    if (max_identities == 0 || max_identities > 65536 ||
        (mode != IdentityStoreMode::Materializing && mode != IdentityStoreMode::Existing) ||
        std::all_of(task_id.value.begin(), task_id.value.end(), [](std::uint8_t byte) {
            return byte == 0;
        })) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto state = std::make_unique<State>();
        state->directory = directory;
        state->name = to_string(task_id) + ".json";
        state->task_id = task_id;
        state->mode = mode;
        state->maximum = max_identities;
        state->faults = std::move(faults);
        state->lease = ::open(
            (directory / (to_string(task_id) + ".lock")).c_str(),
            O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        struct stat attributes {};
        if (state->lease < 0 || fstat(state->lease, &attributes) != 0 ||
            !S_ISREG(attributes.st_mode)) {
            return Status(StatusCode::Unavailable, {});
        }
        if (flock(state->lease, LOCK_EX | LOCK_NB) != 0) {
            return Status(
                errno == EWOULDBLOCK ? StatusCode::Conflict : StatusCode::Unavailable, {});
        }
        auto recovered = state->recover();
        if (!recovered.ok()) {
            return std::move(recovered).status();
        }
        return std::unique_ptr<StreamIdentityStore>(new StreamIdentityStore(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<void> StreamIdentityStore::initialize(const StreamIdentityProjection& projection) {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    auto valid = validate_identity_projection(projection, state_->maximum);
    if (!valid.ok()) {
        return std::move(valid).status();
    }
    try {
        if (state_->current) {
            auto actual = state_->check_actual();
            if (!actual.ok()) {
                return std::move(actual).status();
            }
            return matches_projection(*state_->current, projection)
                       ? Result<void>{}
                       : Result<void>{Status(StatusCode::Conflict, {})};
        }
        StreamIdentityState initial;
        initial.task_id = state_->task_id;
        initial.observed_config_version = projection.config_version;
        initial.pipeline_id_high_water = projection.pipeline_id_high_water;
        initial.stream_id_high_water = projection.stream_id_high_water;
        initial.streams.reserve(projection.streams.size());
        for (const auto& binding : projection.streams) {
            initial.streams.push_back(
                {binding.stream_id, binding.stream_key, 1, 1, false, std::nullopt});
        }
        return state_->publish(std::move(initial));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> StreamIdentityStore::apply_config_change(
    const StreamIdentityProjection& projection, std::uint64_t expected_revision) {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    if (!state_->current) {
        return Status(StatusCode::InvalidState, {});
    }
    auto valid = validate_identity_projection(projection, state_->maximum);
    if (!valid.ok()) {
        return std::move(valid).status();
    }
    const auto& previous = *state_->current;
    if (previous.record_revision != expected_revision ||
        projection.pipeline_id_high_water < previous.pipeline_id_high_water ||
        projection.stream_id_high_water < previous.stream_id_high_water) {
        return Status(StatusCode::Conflict, {});
    }
    if (previous.observed_config_version == projection.config_version &&
        !active_bindings_match(previous, projection)) {
        return Status(StatusCode::Conflict, "Config projection changed without a new version");
    }
    if (matches_projection(previous, projection)) {
        try {
            return state_->check_actual();
        } catch (const std::bad_alloc&) {
            return Status(StatusCode::ResourceExhausted, {});
        }
    }
    if (previous.record_revision == std::numeric_limits<std::uint64_t>::max()) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    try {
        auto next = previous;
        ++next.record_revision;
        next.observed_config_version = projection.config_version;
        next.pipeline_id_high_water = projection.pipeline_id_high_water;
        next.stream_id_high_water = projection.stream_id_high_water;
        // Additions are checked against the original high-water, not against
        // a candidate already changed by earlier entries in this same update.
        for (const auto& binding : projection.streams) {
            const auto existing = find_stream(previous, binding.stream_id);
            if (existing != previous.streams.end() && existing->stream_id == binding.stream_id) {
                if (existing->stream_key != binding.stream_key) {
                    return Status(StatusCode::Conflict, "Stable Stream binding cannot change");
                }
            } else {
                if (binding.stream_id <= previous.stream_id_high_water) {
                    return Status(
                        StatusCode::Conflict, "Retired or unknown Stream ID cannot be reused");
                }
                if (next.streams.size() >= state_->maximum) {
                    return Status(StatusCode::ResourceExhausted, {});
                }
                next.streams.push_back(
                    {binding.stream_id, binding.stream_key, 1, 1, false, std::nullopt});
            }
        }
        std::sort(
            next.streams.begin(), next.streams.end(),
            [](const auto& left, const auto& right) { return left.stream_id < right.stream_id; });
        for (auto& stream : next.streams) {
            const auto binding = std::lower_bound(
                projection.streams.begin(), projection.streams.end(), stream.stream_id,
                [](const auto& entry, StreamId id) { return entry.stream_id < id; });
            const bool active =
                binding != projection.streams.end() && binding->stream_id == stream.stream_id;
            if (active) {
                stream.retired = false;
                stream.retired_at_config_version.reset();
            } else if (!stream.retired) {
                stream.retired = true;
                stream.retired_at_config_version = projection.config_version;
            }
        }
        return state_->publish(std::move(next));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<IdentityRange> StreamIdentityStore::reserve_ids(
    StreamId stream_id, std::uint32_t requested_count, bool descriptor_ids) {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    if (!state_->current) {
        return Status(StatusCode::InvalidState, {});
    }
    if (stream_id == 0 || requested_count == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto found = find_stream(*state_->current, stream_id);
    if (found == state_->current->streams.end() || found->stream_id != stream_id) {
        return Status(StatusCode::NotFound, {});
    }
    if (found->retired) {
        return Status(StatusCode::InvalidState, {});
    }
    const auto begin =
        descriptor_ids ? found->reserved_descriptor_end_exclusive : found->reserved_end_exclusive;
    if (requested_count > std::numeric_limits<std::uint64_t>::max() - begin ||
        state_->current->record_revision == std::numeric_limits<std::uint64_t>::max()) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    const IdentityRange range{begin, begin + requested_count};
    try {
        auto next = *state_->current;
        ++next.record_revision;
        auto target = find_stream(next, stream_id);
        if (descriptor_ids) {
            target->reserved_descriptor_end_exclusive = range.end_exclusive;
        } else {
            target->reserved_end_exclusive = range.end_exclusive;
        }
        auto published = state_->publish(std::move(next));
        if (!published.ok()) {
            return std::move(published).status();
        }
        return range;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<IdentityRange> StreamIdentityStore::reserve(
    StreamId stream_id, std::uint32_t requested_count) {
    return reserve_ids(stream_id, requested_count, false);
}

Result<IdentityRange> StreamIdentityStore::reserve_descriptors(
    StreamId stream_id, std::uint32_t requested_count) {
    return reserve_ids(stream_id, requested_count, true);
}

Result<StreamIdentityState> StreamIdentityStore::load() {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    if (!state_->current) {
        return Status(StatusCode::NotFound, {});
    }
    try {
        auto actual = state_->check_actual();
        if (!actual.ok()) {
            return std::move(actual).status();
        }
        return *state_->current;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::optional<StreamIdentityState>> StreamIdentityStore::reconcile() {
    std::unique_lock<std::mutex> execution(state_->execution, std::try_to_lock);
    if (!execution.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    try {
        return state_->recover();
    } catch (const std::bad_alloc&) {
        state_->protected_state = true;
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        state_->protected_state = true;
        return Status(StatusCode::Unavailable, {});
    }
}

}  // namespace rodaq::pipeline
