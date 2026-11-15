#include "stream_identity.hpp"

#include "../persistence/decimal.hpp"
#include "../persistence/json.hpp"

#include <algorithm>
#include <initializer_list>
#include <new>
#include <utility>

namespace rodaq::pipeline {
namespace {

using persistence::Json;
constexpr std::string_view kDomain = "rodaq.stream-identity.v1";

persistence::JsonLimits limits(std::size_t maximum) {
    return {stream_identity_max_bytes(maximum), 4, 128, 12, maximum, 16 + maximum * 7};
}

bool visible(std::string_view text) noexcept {
    return !text.empty() && text.size() <= 128 &&
           std::all_of(text.begin(), text.end(), [](unsigned char byte) {
               return byte >= 0x21 && byte <= 0x7e;
           });
}

bool fields(const Json& object, std::initializer_list<const char*> names) {
    if (!object.is_object() || object.size() != names.size()) {
        return false;
    }
    return std::all_of(
        names.begin(), names.end(), [&](const char* name) { return object.contains(name); });
}

const std::string* text(const Json& document, const char* name) noexcept {
    const auto field = document.find(name);
    return field != document.end() && field->is_string() ? &field->get_ref<const std::string&>()
                                                         : nullptr;
}

bool decimal(const Json& document, const char* field, std::uint64_t& output) {
    const auto* value = text(document, field);
    if (value == nullptr) {
        return false;
    }
    auto parsed = persistence::unsigned_decimal(*value);
    if (!parsed.ok()) {
        return false;
    }
    output = parsed.value();
    return true;
}

}  // namespace

std::size_t stream_identity_max_bytes(std::size_t maximum) noexcept {
    // Includes the worst-case escaped visible binding key and per-entry fields.
    // Callers validate the deployment identity bound before using this size.
    return 1024 + maximum * 1024;
}

Result<void> validate_identity_projection(
    const StreamIdentityProjection& projection, std::size_t max_identities) {
    if (max_identities == 0 || max_identities > 65536 || projection.streams.empty() ||
        projection.streams.size() > max_identities || projection.pipeline_id_high_water == 0 ||
        projection.stream_id_high_water == 0 ||
        !persistence::valid_digest(projection.config_version)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    StreamId previous = 0;
    for (const auto& binding : projection.streams) {
        if (binding.stream_id <= previous || binding.stream_id > projection.stream_id_high_water ||
            !visible(binding.stream_key)) {
            return Status(StatusCode::InvalidArgument, {});
        }
        previous = binding.stream_id;
    }
    return {};
}

Result<void> validate_stream_identity(
    const StreamIdentityState& state, std::size_t max_identities) {
    if (max_identities == 0 || max_identities > 65536 || state.streams.size() > max_identities ||
        std::all_of(
            state.task_id.value.begin(), state.task_id.value.end(),
            [](std::uint8_t byte) { return byte == 0; }) ||
        state.record_revision == 0 || state.pipeline_id_high_water == 0 ||
        state.stream_id_high_water == 0 ||
        !persistence::valid_digest(state.observed_config_version)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    StreamId previous = 0;
    for (const auto& stream : state.streams) {
        if (stream.stream_id <= previous || stream.stream_id > state.stream_id_high_water ||
            !visible(stream.stream_key) || stream.reserved_end_exclusive == 0 ||
            stream.reserved_descriptor_end_exclusive == 0 ||
            stream.retired != stream.retired_at_config_version.has_value() ||
            (stream.retired_at_config_version &&
             !persistence::valid_digest(*stream.retired_at_config_version))) {
            return Status(StatusCode::InvalidArgument, {});
        }
        previous = stream.stream_id;
    }
    return {};
}

Result<std::string> encode_stream_identity(
    const StreamIdentityState& state, std::size_t max_identities) {
    auto valid = validate_stream_identity(state, max_identities);
    if (!valid.ok()) {
        return std::move(valid).status();
    }
    try {
        Json streams = Json::array();
        for (const auto& stream : state.streams) {
            streams.push_back(
                {{"stream_id", std::to_string(stream.stream_id)},
                 {"stream_key", stream.stream_key},
                 {"reserved_end_exclusive", std::to_string(stream.reserved_end_exclusive)},
                 {"reserved_descriptor_end_exclusive",
                  std::to_string(stream.reserved_descriptor_end_exclusive)},
                 {"retired", stream.retired},
                 {"retired_at_config_version", stream.retired_at_config_version
                                                   ? Json(*stream.retired_at_config_version)
                                                   : Json(nullptr)}});
        }
        const Json document{
            {"format_version", "1.0"},
            {"task_id", to_string(state.task_id)},
            {"record_revision", std::to_string(state.record_revision)},
            {"pipeline_id_high_water", std::to_string(state.pipeline_id_high_water)},
            {"stream_id_high_water", std::to_string(state.stream_id_high_water)},
            {"observed_config_version", state.observed_config_version},
            {"streams", std::move(streams)}};
        return persistence::encode_integrity(document, kDomain, limits(max_identities));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<StreamIdentityState> decode_stream_identity(
    std::string_view bytes, std::size_t max_identities) {
    if (max_identities == 0 || max_identities > 65536) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto parsed = persistence::parse_json(bytes, limits(max_identities));
    if (!parsed.ok()) {
        return parsed.status().code() == StatusCode::InvalidArgument
                   ? Status(StatusCode::DataLoss, {})
                   : std::move(parsed).status();
    }
    try {
        const auto& document = parsed.value();
        if (!fields(
                document,
                {"format_version", "task_id", "record_revision", "pipeline_id_high_water",
                 "stream_id_high_water", "observed_config_version", "streams", "content_digest"}) ||
            text(document, "format_version") == nullptr || text(document, "task_id") == nullptr ||
            text(document, "observed_config_version") == nullptr ||
            !document.at("streams").is_array()) {
            return Status(StatusCode::DataLoss, {});
        }
        if (*text(document, "format_version") != "1.0") {
            return Status(StatusCode::Unavailable, {});
        }
        StreamIdentityState state;
        auto task_id = parse_task_id(*text(document, "task_id"));
        if (!task_id.ok() || !decimal(document, "record_revision", state.record_revision) ||
            !decimal(document, "pipeline_id_high_water", state.pipeline_id_high_water) ||
            !decimal(document, "stream_id_high_water", state.stream_id_high_water)) {
            return Status(StatusCode::DataLoss, {});
        }
        state.task_id = task_id.value();
        state.observed_config_version = *text(document, "observed_config_version");
        state.streams.reserve(document.at("streams").size());
        for (const auto& entry : document.at("streams")) {
            StreamIdentity stream;
            if (!fields(
                    entry, {"stream_id", "stream_key", "reserved_end_exclusive",
                            "reserved_descriptor_end_exclusive", "retired",
                            "retired_at_config_version"}) ||
                !decimal(entry, "stream_id", stream.stream_id) ||
                !decimal(entry, "reserved_end_exclusive", stream.reserved_end_exclusive) ||
                !decimal(
                    entry, "reserved_descriptor_end_exclusive",
                    stream.reserved_descriptor_end_exclusive) ||
                text(entry, "stream_key") == nullptr || !entry.at("retired").is_boolean()) {
                return Status(StatusCode::DataLoss, {});
            }
            stream.stream_key = *text(entry, "stream_key");
            stream.retired = entry.at("retired").get<bool>();
            if (!entry.at("retired_at_config_version").is_null()) {
                const auto* version = text(entry, "retired_at_config_version");
                if (version == nullptr) {
                    return Status(StatusCode::DataLoss, {});
                }
                stream.retired_at_config_version = *version;
            }
            state.streams.push_back(std::move(stream));
        }
        auto valid = validate_stream_identity(state, max_identities);
        if (!valid.ok()) {
            return Status(StatusCode::DataLoss, {});
        }
        auto verified = persistence::verify_integrity(document, kDomain, limits(max_identities));
        if (!verified.ok()) {
            return std::move(verified).status();
        }
        return state;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::pipeline
