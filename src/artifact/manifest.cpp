#include "manifest.hpp"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <new>
#include <utility>

#include <rodaq/canonical/validation.hpp>
#include <rodaq/plugin/abi.hpp>
#include <rodaq/plugin/version.hpp>
#include <rodaq/storage/validation.hpp>

#include "../persistence/decimal.hpp"
#include "../persistence/digest.hpp"
#include "../persistence/json.hpp"
#include "../uuid.hpp"

namespace rodaq::artifact {
namespace {

using persistence::Json;

constexpr std::string_view kManifestFormatVersion = "2.0";
constexpr std::string_view kEmptySchema =
    "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

struct InvalidManifest {
    Status status;
};

void require(bool condition) {
    if (!condition) {
        throw InvalidManifest{Status(StatusCode::InvalidArgument, {})};
    }
}

template <typename Value>
Value take(Result<Value> result) {
    if (!result.ok()) {
        throw InvalidManifest{std::move(result).status()};
    }
    return std::move(result).value();
}

void valid_limits(const ManifestLimits& limits) {
    require(
        limits.max_bytes > 0 && limits.max_bytes <= 64 * 1024 * 1024 && limits.max_files > 0 &&
        limits.max_files <= 4096 && limits.max_descriptors > 0 && limits.max_descriptors <= 1024 &&
        limits.max_streams > 0 && limits.max_streams <= 1024 &&
        limits.max_descriptor_metadata_bytes > 0 &&
        limits.max_descriptor_metadata_bytes <= 64 * 1024 * 1024);
}

void fields(
    const Json& object, std::initializer_list<const char*> names,
    std::initializer_list<const char*> optional = {}) {
    std::size_t present = 0;
    for (const auto* name : optional) {
        present += object.contains(name) ? 1 : 0;
    }
    require(object.is_object() && object.size() == names.size() + present);
    for (const auto* name : names) {
        require(object.contains(name));
    }
}

const std::string& text(const Json& object, const char* field) {
    const auto& value = object.at(field);
    require(value.is_string());
    return value.get_ref<const std::string&>();
}

std::uint64_t unsigned_value(const Json& object, const char* field) {
    const auto& value = object.at(field);
    if (value.is_string()) {
        return take(persistence::unsigned_decimal(value.get_ref<const std::string&>()));
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto signed_val = value.get<std::int64_t>();
        require(signed_val >= 0);
        return static_cast<std::uint64_t>(signed_val);
    }
    throw InvalidManifest{Status(StatusCode::InvalidArgument, {})};
}

std::uint32_t narrow_unsigned(const Json& object, const char* field) {
    const auto value = unsigned_value(object, field);
    require(value <= std::numeric_limits<std::uint32_t>::max());
    return static_cast<std::uint32_t>(value);
}

std::int64_t signed_value(const Json& object, const char* field) {
    const auto& value = object.at(field);
    if (value.is_string()) {
        return take(persistence::signed_decimal(value.get_ref<const std::string&>()));
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_unsigned()) {
        const auto unsigned_val = value.get<std::uint64_t>();
        require(
            unsigned_val <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
        return static_cast<std::int64_t>(unsigned_val);
    }
    throw InvalidManifest{Status(StatusCode::InvalidArgument, {})};
}

void add(std::uint64_t& total, std::uint64_t value) {
    require(value <= std::numeric_limits<std::uint64_t>::max() - total);
    total += value;
}

bool valid_text(const std::string& value, std::size_t max_bytes, bool allow_empty = false) {
    return (allow_empty || !value.empty()) && value.size() <= max_bytes &&
           value.find('\0') == std::string::npos &&
           plugin::abi::utf8_prefix(value, value.size()) == value.size();
}

void validate_plugin(const plugin::PluginInfo& identity) {
    require(
        plugin::abi::valid_identifier(identity.plugin_id) && identity.plugin_version.size() <= 128);
    take(plugin::validate_plugin_version(identity.plugin_version));
}

Json parse_metadata_object(
    std::string_view format, std::string_view json, std::uint32_t max_bytes) {
    if (format.empty() && json.empty()) {
        return Json{};
    }
    if (format.empty() || json.empty() || !canonical::valid_metadata_format(format)) {
        throw InvalidManifest{Status(StatusCode::InvalidArgument, "Invalid descriptor metadata")};
    }
    if (json.size() > max_bytes) {
        throw InvalidManifest{
            Status(StatusCode::ResourceExhausted, "Descriptor metadata exceeds byte limit")};
    }
    auto parsed = persistence::parse_json(
        json, {max_bytes, kMaxDescriptorMetadataDepth, kMaxDescriptorMetadataNodes});
    if (!parsed.ok()) {
        throw InvalidManifest{std::move(parsed).status()};
    }
    if (!parsed.value().is_object()) {
        throw InvalidManifest{
            Status(StatusCode::InvalidArgument, "Descriptor metadata JSON must be an object")};
    }
    const auto normalized = parsed.value().dump();
    if (normalized.size() > max_bytes) {
        throw InvalidManifest{Status(
            StatusCode::ResourceExhausted, "Normalized descriptor metadata exceeds byte limit")};
    }
    return std::move(parsed).value();
}

void validate_descriptor(const ManifestDescriptor& descriptor, const ManifestLimits& limits) {
    require(
        descriptor.stream_id != 0 && descriptor.descriptor_id != 0 &&
        valid_text(descriptor.channel_name, 1024) && valid_text(descriptor.message_encoding, 64) &&
        valid_text(descriptor.schema_name, 1024, true) &&
        valid_text(descriptor.schema_encoding, 64, true) &&
        persistence::valid_digest(descriptor.schema_digest) &&
        (descriptor.schema_size != 0 || descriptor.schema_digest == kEmptySchema));
    const auto token = [](const std::string& value) {
        return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x21 && byte <= 0x7e;
        });
    };
    require(token(descriptor.message_encoding) && token(descriptor.schema_encoding));
    if (!descriptor.metadata_format.empty() || !descriptor.metadata_json.empty()) {
        const auto obj = parse_metadata_object(
            descriptor.metadata_format, descriptor.metadata_json,
            limits.max_descriptor_metadata_bytes);
        require(obj.dump() == descriptor.metadata_json);
    }
}

void validate_identity(const ManifestIdentity& identity) {
    require(
        identity.segment_id != 0 && uuid::nonzero(identity.task_id.value) &&
        identity.pipeline_id != 0 && identity.segment_sequence != 0 &&
        identity.control_generation != 0 && uuid::nonzero(identity.volume_id.value) &&
        valid_text(identity.config_version, 128) &&
        valid_text(identity.deployment_generation, 128) &&
        valid_text(identity.qualified_runtime_profile_id, 256) && valid_text(identity.format, 128));
    take(uuid::parse(identity.instance_id));
    validate_plugin(identity.acquisition);
    validate_plugin(identity.storage);
}

void validate(const SegmentManifest& manifest, const ManifestLimits& limits) {
    valid_limits(limits);
    validate_identity(manifest.identity);
    require(
        !manifest.files.empty() && manifest.files.size() <= limits.max_files &&
        !manifest.descriptors.empty() && manifest.descriptors.size() <= limits.max_descriptors &&
        !manifest.coverage.empty() && manifest.coverage.size() <= limits.max_streams);
    std::uint64_t file_bytes = 0;
    for (std::size_t index = 0; index < manifest.files.size(); ++index) {
        const auto& file = manifest.files[index];
        require(
            file.relative_path.size() <= 4096 &&
            storage::abi::valid_relative_path(file.relative_path) &&
            file.relative_path != "manifest.json");
        add(file_bytes, file.physical_size);
        for (std::size_t previous = 0; previous < index; ++previous) {
            require(manifest.files[previous].relative_path != file.relative_path);
        }
    }
    require(
        manifest.first_receive_monotonic >= manifest.identity.playback_anchor.monotonic_ns &&
        manifest.first_receive_monotonic <= manifest.last_receive_monotonic &&
        manifest.min_receive_realtime <= manifest.max_receive_realtime &&
        manifest.recording_begin_ns <= manifest.recording_end_ns);
    std::uint64_t accepted = 0;
    for (std::size_t index = 0; index < manifest.coverage.size(); ++index) {
        const auto& coverage = manifest.coverage[index];
        require(
            coverage.stream_id != 0 && coverage.accepted != 0 && coverage.first_sequence != 0 &&
            coverage.first_sequence <= coverage.last_sequence &&
            coverage.accepted - 1 <= coverage.last_sequence - coverage.first_sequence &&
            coverage.written <= coverage.accepted &&
            coverage.known_failed <= coverage.accepted - coverage.written &&
            coverage.unknown == coverage.accepted - coverage.written - coverage.known_failed);
        for (std::size_t previous = 0; previous < index; ++previous) {
            require(manifest.coverage[previous].stream_id != coverage.stream_id);
        }
        add(accepted, coverage.accepted);
        std::uint64_t descriptor_records = 0;
        for (const auto& descriptor : manifest.descriptors) {
            if (descriptor.stream_id == coverage.stream_id) {
                add(descriptor_records, descriptor.accepted_records);
            }
        }
        require(descriptor_records == coverage.accepted);
    }
    for (std::size_t index = 0; index < manifest.descriptors.size(); ++index) {
        const auto& descriptor = manifest.descriptors[index];
        validate_descriptor(descriptor, limits);
        require(descriptor.accepted_records != 0);
        require(std::any_of(
            manifest.coverage.begin(), manifest.coverage.end(),
            [&](const auto& coverage) { return coverage.stream_id == descriptor.stream_id; }));
        for (std::size_t previous = 0; previous < index; ++previous) {
            const auto& old = manifest.descriptors[previous];
            require(
                old.stream_id != descriptor.stream_id ||
                old.descriptor_id != descriptor.descriptor_id);
        }
    }
}

Json plugin_document(const plugin::PluginInfo& value) {
    return {{"plugin_id", value.plugin_id}, {"plugin_version", value.plugin_version}};
}

plugin::PluginInfo read_plugin(const Json& value) {
    fields(value, {"plugin_id", "plugin_version"});
    return {text(value, "plugin_id"), {}, text(value, "plugin_version")};
}

Json descriptor_document(const ManifestDescriptor& value, std::uint32_t max_metadata_bytes) {
    Json result{
        {"stream_id", std::to_string(value.stream_id)},
        {"descriptor_id", std::to_string(value.descriptor_id)},
        {"accepted_records", std::to_string(value.accepted_records)},
        {"channel_name", value.channel_name},
        {"message_encoding", value.message_encoding},
        {"schema_name", value.schema_name},
        {"schema_encoding", value.schema_encoding},
        {"schema_digest", value.schema_digest},
        {"schema_size", value.schema_size}};
    if (!value.metadata_format.empty()) {
        result["metadata"] = {
            {"format", value.metadata_format},
            {"json", parse_metadata_object(
                         value.metadata_format, value.metadata_json, max_metadata_bytes)}};
    }
    return result;
}

ManifestDescriptor read_descriptor(const Json& value, const ManifestLimits& limits) {
    fields(
        value,
        {"stream_id", "descriptor_id", "accepted_records", "channel_name", "message_encoding",
         "schema_name", "schema_encoding", "schema_digest", "schema_size"},
        {"metadata"});
    ManifestDescriptor result;
    result.stream_id = unsigned_value(value, "stream_id");
    result.descriptor_id = unsigned_value(value, "descriptor_id");
    result.accepted_records = unsigned_value(value, "accepted_records");
    result.channel_name = text(value, "channel_name");
    result.message_encoding = text(value, "message_encoding");
    result.schema_name = text(value, "schema_name");
    result.schema_encoding = text(value, "schema_encoding");
    result.schema_digest = text(value, "schema_digest");
    result.schema_size = narrow_unsigned(value, "schema_size");
    if (value.contains("metadata")) {
        const auto& meta = value.at("metadata");
        fields(meta, {"format", "json"});
        result.metadata_format = text(meta, "format");
        require(canonical::valid_metadata_format(result.metadata_format));
        require(meta.at("json").is_object());
        result.metadata_json = meta.at("json").dump();
        if (result.metadata_json.size() > limits.max_descriptor_metadata_bytes) {
            throw InvalidManifest{Status(
                StatusCode::ResourceExhausted, "Manifest descriptor metadata exceeds byte limit")};
        }
    }
    return result;
}

Json document(const SegmentManifest& manifest, const ManifestLimits& limits) {
    Json files = Json::array();
    for (const auto& value : manifest.files) {
        files.push_back(
            {{"relative_path", value.relative_path},
             {"physical_size", std::to_string(value.physical_size)}});
    }
    Json descriptors = Json::array();
    for (const auto& value : manifest.descriptors) {
        descriptors.push_back(descriptor_document(value, limits.max_descriptor_metadata_bytes));
    }
    Json coverage = Json::array();
    for (const auto& value : manifest.coverage) {
        coverage.push_back(
            {{"stream_id", std::to_string(value.stream_id)},
             {"accepted", std::to_string(value.accepted)},
             {"written", std::to_string(value.written)},
             {"known_failed", std::to_string(value.known_failed)},
             {"unknown", std::to_string(value.unknown)},
             {"first_sequence", std::to_string(value.first_sequence)},
             {"last_sequence", std::to_string(value.last_sequence)}});
    }
    return {
        {"format_version", std::string(kManifestFormatVersion)},
        {"segment_id", std::to_string(manifest.identity.segment_id)},
        {"task_id", to_string(manifest.identity.task_id)},
        {"pipeline_id", std::to_string(manifest.identity.pipeline_id)},
        {"instance_id", manifest.identity.instance_id},
        {"segment_sequence", std::to_string(manifest.identity.segment_sequence)},
        {"committed_at_ns",
         std::to_string(manifest.identity.committed_at.time_since_epoch().count())},
        {"config_version", manifest.identity.config_version},
        {"deployment_generation", manifest.identity.deployment_generation},
        {"control_generation", std::to_string(manifest.identity.control_generation)},
        {"volume_id", to_string(manifest.identity.volume_id)},
        {"acquisition", plugin_document(manifest.identity.acquisition)},
        {"storage", plugin_document(manifest.identity.storage)},
        {"qualified_runtime_profile_id", manifest.identity.qualified_runtime_profile_id},
        {"format", manifest.identity.format},
        {"logical_bytes", std::to_string(manifest.logical_bytes)},
        {"files", std::move(files)},
        {"descriptors", std::move(descriptors)},
        {"coverage", std::move(coverage)},
        {"playback_anchor_monotonic",
         std::to_string(manifest.identity.playback_anchor.monotonic_ns)},
        {"playback_anchor_realtime", std::to_string(manifest.identity.playback_anchor.realtime_ns)},
        {"first_receive_monotonic", std::to_string(manifest.first_receive_monotonic)},
        {"last_receive_monotonic", std::to_string(manifest.last_receive_monotonic)},
        {"min_receive_realtime", std::to_string(manifest.min_receive_realtime)},
        {"max_receive_realtime", std::to_string(manifest.max_receive_realtime)},
        {"recording_begin_ns", std::to_string(manifest.recording_begin_ns)},
        {"recording_end_ns", std::to_string(manifest.recording_end_ns)}};
}

}  // namespace

Result<std::string> normalize_descriptor_metadata(
    std::string_view format, std::string_view json, std::uint32_t max_bytes) {
    if (format.empty() && json.empty()) {
        return std::string{};
    }
    try {
        const auto obj = parse_metadata_object(format, json, max_bytes);
        return obj.dump();
    } catch (InvalidManifest& failure) {
        return std::move(failure.status);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<ManifestDescriptor> capture_manifest_descriptor(
    const storage::StreamDescriptor& descriptor, std::uint64_t accepted_records,
    std::uint32_t max_schema_bytes, std::uint32_t max_metadata_bytes) {
    if (descriptor.schema.size > max_schema_bytes || !storage::abi::valid_descriptor(&descriptor)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        const auto copy = [](rodaq_string_t value) {
            return value.size == 0 ? std::string{} : std::string(value.data, value.size);
        };
        ManifestDescriptor result;
        result.stream_id = descriptor.stream_id;
        result.descriptor_id = descriptor.descriptor_id;
        result.accepted_records = accepted_records;
        result.channel_name = copy(descriptor.channel_name);
        result.message_encoding = copy(descriptor.message_encoding);
        result.schema_name = copy(descriptor.schema_name);
        result.schema_encoding = copy(descriptor.schema_encoding);
        result.schema_size = descriptor.schema.size;
        auto digest = persistence::byte_digest(descriptor.schema.data, descriptor.schema.size);
        if (!digest.ok()) {
            return digest.status();
        }
        result.schema_digest = std::move(digest.value());
        if (descriptor.metadata.format.size != 0) {
            result.metadata_format = copy(descriptor.metadata.format);
            auto normalized = normalize_descriptor_metadata(
                result.metadata_format,
                {descriptor.metadata.json.data, descriptor.metadata.json.size}, max_metadata_bytes);
            if (!normalized.ok()) {
                return normalized.status();
            }
            result.metadata_json = std::move(normalized.value());
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::size_t> manifest_base_upper_bound(
    const ManifestIdentity& identity, std::size_t stream_count, std::uint32_t max_files,
    std::uint32_t max_path_bytes) {
    try {
        validate_identity(identity);
        SegmentManifest skeleton;
        skeleton.identity = identity;
        skeleton.identity.committed_at =
            WallTime{std::chrono::nanoseconds{std::numeric_limits<std::int64_t>::min()}};
        skeleton.logical_bytes = std::numeric_limits<std::uint64_t>::max();
        skeleton.first_receive_monotonic = std::numeric_limits<std::uint64_t>::max();
        skeleton.last_receive_monotonic = std::numeric_limits<std::uint64_t>::max();
        skeleton.min_receive_realtime = std::numeric_limits<std::int64_t>::min();
        skeleton.max_receive_realtime = std::numeric_limits<std::int64_t>::min();
        skeleton.recording_begin_ns = std::numeric_limits<std::uint64_t>::max();
        skeleton.recording_end_ns = std::numeric_limits<std::uint64_t>::max();
        skeleton.coverage.reserve(stream_count);
        for (std::size_t i = 0; i < stream_count; ++i) {
            skeleton.coverage.push_back(StreamCoverage{
                std::numeric_limits<std::uint64_t>::max(),
                std::numeric_limits<std::uint64_t>::max(),
                std::numeric_limits<std::uint64_t>::max(),
                std::numeric_limits<std::uint64_t>::max(),
                std::numeric_limits<std::uint64_t>::max(),
                std::numeric_limits<std::uint64_t>::max(),
                std::numeric_limits<std::uint64_t>::max()});
        }
        ManifestLimits dummy;
        std::size_t total = document(skeleton, dummy).dump().size();
        // Worst-case FileEvidence entry:
        // {"physical_size":"18446744073709551615","relative_path":"..."} Account for up to 6x JSON
        // escaping for any control byte in max_path_bytes.
        const std::size_t per_file = 64U + static_cast<std::size_t>(max_path_bytes) * 6U;
        if (max_files > 0 && per_file > (std::numeric_limits<std::size_t>::max() - total) /
                                            static_cast<std::size_t>(max_files)) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        total += static_cast<std::size_t>(max_files) * per_file;
        return total;
    } catch (InvalidManifest& failure) {
        return std::move(failure.status);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::size_t> manifest_descriptor_upper_bound(
    const ManifestDescriptor& descriptor, const ManifestLimits& limits) {
    try {
        valid_limits(limits);
        validate_descriptor(descriptor, limits);
        ManifestDescriptor worst = descriptor;
        worst.accepted_records = std::numeric_limits<std::uint64_t>::max();
        // +1 covers the JSON array comma separator between descriptor entries.
        return descriptor_document(worst, limits.max_descriptor_metadata_bytes).dump().size() + 1U;
    } catch (InvalidManifest& failure) {
        return std::move(failure.status);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> validate_manifest_identity(const ManifestIdentity& identity) {
    try {
        validate_identity(identity);
        return {};
    } catch (InvalidManifest& failure) {
        return std::move(failure.status);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> validate_manifest_limits(const ManifestLimits& limits) {
    try {
        valid_limits(limits);
        return {};
    } catch (InvalidManifest& failure) {
        return std::move(failure.status);
    }
}

Result<void> validate_manifest(const SegmentManifest& manifest, const ManifestLimits& limits) {
    try {
        validate(manifest, limits);
        return {};
    } catch (InvalidManifest& failure) {
        return std::move(failure.status);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::string> encode_manifest(const SegmentManifest& manifest, const ManifestLimits& limits) {
    try {
        validate(manifest, limits);
        auto encoded = document(manifest, limits).dump();
        if (encoded.size() > limits.max_bytes) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        return encoded;
    } catch (InvalidManifest& failure) {
        return std::move(failure.status);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<SegmentManifest> decode_manifest(std::string_view encoded, const ManifestLimits& limits) {
    try {
        valid_limits(limits);
        const std::size_t max_nodes = std::max<std::size_t>(65536, limits.max_bytes / 2);
        const auto value =
            take(persistence::parse_json(encoded, {limits.max_bytes, 48, max_nodes}));
        // Check format_version FIRST before validating version-specific fields so
        // pre-reset 1.0 manifests fail closed with Unavailable rather than DataLoss.
        if (!value.is_object() || !value.contains("format_version") ||
            !value.at("format_version").is_string()) {
            return Status(StatusCode::DataLoss, "Manifest missing string format_version");
        }
        const auto& version = value.at("format_version").get_ref<const std::string&>();
        if (version != kManifestFormatVersion) {
            std::string safe_version;
            safe_version.reserve(std::min<std::size_t>(version.size(), 64));
            for (const unsigned char ch : version) {
                if (safe_version.size() == 64) {
                    break;
                }
                safe_version.push_back(ch >= 0x20 && ch <= 0x7e ? static_cast<char>(ch) : '?');
            }
            return Status(
                StatusCode::Unavailable,
                "Incompatible Artifact manifest format_version '" + safe_version +
                    "' (expected '2.0'); retain existing volume data and select a new volume or "
                    "re-initialize after backup");
        }
        fields(
            value, {"format_version",
                    "segment_id",
                    "task_id",
                    "pipeline_id",
                    "instance_id",
                    "segment_sequence",
                    "committed_at_ns",
                    "config_version",
                    "deployment_generation",
                    "control_generation",
                    "volume_id",
                    "acquisition",
                    "storage",
                    "qualified_runtime_profile_id",
                    "format",
                    "logical_bytes",
                    "files",
                    "descriptors",
                    "coverage",
                    "playback_anchor_monotonic",
                    "playback_anchor_realtime",
                    "first_receive_monotonic",
                    "last_receive_monotonic",
                    "min_receive_realtime",
                    "max_receive_realtime",
                    "recording_begin_ns",
                    "recording_end_ns"});
        SegmentManifest manifest;
        manifest.identity.segment_id = unsigned_value(value, "segment_id");
        manifest.identity.task_id = take(parse_task_id(text(value, "task_id")));
        manifest.identity.pipeline_id = unsigned_value(value, "pipeline_id");
        manifest.identity.instance_id = text(value, "instance_id");
        manifest.identity.segment_sequence = unsigned_value(value, "segment_sequence");
        manifest.identity.committed_at =
            WallTime{std::chrono::nanoseconds{signed_value(value, "committed_at_ns")}};
        manifest.identity.config_version = text(value, "config_version");
        manifest.identity.deployment_generation = text(value, "deployment_generation");
        manifest.identity.control_generation = unsigned_value(value, "control_generation");
        manifest.identity.volume_id = take(parse_volume_id(text(value, "volume_id")));
        manifest.identity.acquisition = read_plugin(value.at("acquisition"));
        manifest.identity.storage = read_plugin(value.at("storage"));
        manifest.identity.qualified_runtime_profile_id =
            text(value, "qualified_runtime_profile_id");
        manifest.identity.format = text(value, "format");
        manifest.logical_bytes = unsigned_value(value, "logical_bytes");
        for (const auto* list : {"files", "descriptors", "coverage"}) {
            require(value.at(list).is_array());
        }
        require(
            value.at("files").size() <= limits.max_files &&
            value.at("descriptors").size() <= limits.max_descriptors &&
            value.at("coverage").size() <= limits.max_streams);
        for (const auto& file : value.at("files")) {
            fields(file, {"relative_path", "physical_size"});
            manifest.files.push_back(
                {text(file, "relative_path"), unsigned_value(file, "physical_size")});
        }
        for (const auto& descriptor : value.at("descriptors")) {
            manifest.descriptors.push_back(read_descriptor(descriptor, limits));
        }
        for (const auto& coverage : value.at("coverage")) {
            fields(
                coverage, {"stream_id", "accepted", "written", "known_failed", "unknown",
                           "first_sequence", "last_sequence"});
            manifest.coverage.push_back(
                {unsigned_value(coverage, "stream_id"), unsigned_value(coverage, "accepted"),
                 unsigned_value(coverage, "written"), unsigned_value(coverage, "known_failed"),
                 unsigned_value(coverage, "unknown"), unsigned_value(coverage, "first_sequence"),
                 unsigned_value(coverage, "last_sequence")});
        }
        manifest.identity.playback_anchor = {
            unsigned_value(value, "playback_anchor_monotonic"),
            signed_value(value, "playback_anchor_realtime")};
        manifest.first_receive_monotonic = unsigned_value(value, "first_receive_monotonic");
        manifest.last_receive_monotonic = unsigned_value(value, "last_receive_monotonic");
        manifest.min_receive_realtime = signed_value(value, "min_receive_realtime");
        manifest.max_receive_realtime = signed_value(value, "max_receive_realtime");
        manifest.recording_begin_ns = unsigned_value(value, "recording_begin_ns");
        manifest.recording_end_ns = unsigned_value(value, "recording_end_ns");
        validate(manifest, limits);
        return manifest;
    } catch (InvalidManifest& failure) {
        if (failure.status.code() == StatusCode::ResourceExhausted ||
            failure.status.code() == StatusCode::Unavailable) {
            return std::move(failure.status);
        }
        return Status(StatusCode::DataLoss, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::artifact
