#include "manifest.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <new>
#include <utility>

#include <rodaq/plugin/abi.hpp>
#include <rodaq/plugin/version.hpp>
#include <rodaq/storage/validation.hpp>

#include "../persistence/decimal.hpp"
#include "../persistence/digest.hpp"
#include "../persistence/json.hpp"
#include "../ros2_qos.hpp"
#include "../uuid.hpp"

namespace rodaq::artifact {
namespace {

using persistence::Json;
constexpr auto kDomain = "rodaq.segment-manifest.v1";
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
        limits.max_streams > 0 && limits.max_streams <= 1024 && limits.max_epochs > 0 &&
        limits.max_epochs <= 1024 && limits.max_qos_profiles <= 256);
}

persistence::JsonLimits json_limits(const ManifestLimits& limits) {
    return {
        limits.max_bytes,
        36,
        64 * 1024,
        256,
        std::max(
            {4096u, limits.max_files, limits.max_descriptors, limits.max_streams, limits.max_epochs,
             limits.max_qos_profiles}),
        256 + 2 * persistence::JsonLimits{}.max_nodes +
            static_cast<std::size_t>(limits.max_files) * 4 +
            static_cast<std::size_t>(limits.max_descriptors) *
                (16 + static_cast<std::size_t>(limits.max_qos_profiles) * 13) +
            static_cast<std::size_t>(limits.max_streams) * 9 +
            static_cast<std::size_t>(limits.max_epochs) * 14};
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

std::uint64_t number(const Json& object, const char* field) {
    return take(persistence::unsigned_decimal(text(object, field)));
}

std::uint32_t narrow(const Json& object, const char* field) {
    const auto value = number(object, field);
    require(value <= std::numeric_limits<std::uint32_t>::max());
    return static_cast<std::uint32_t>(value);
}

std::int64_t signed_number(const Json& object, const char* field) {
    return take(persistence::signed_decimal(text(object, field)));
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

void validate_plugin(const plugin::PluginBinaryInfo& identity) {
    require(
        plugin::abi::valid_identifier(identity.plugin_id) &&
        identity.plugin_version.size() <= 128 && identity.sha256.size() == 64 &&
        std::all_of(identity.sha256.begin(), identity.sha256.end(), [](char byte) {
            return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
        }));
    take(plugin::validate_plugin_version(identity.plugin_version));
}

void validate_descriptor(const ManifestDescriptor& descriptor, const ManifestLimits& limits) {
    require(
        descriptor.stream_id != 0 && descriptor.descriptor_id != 0 &&
        valid_text(descriptor.channel_name, 1024) && valid_text(descriptor.message_encoding, 64) &&
        valid_text(descriptor.schema_name, 1024, true) &&
        valid_text(descriptor.schema_encoding, 64, true) &&
        persistence::valid_digest(descriptor.schema_digest) &&
        (descriptor.schema_size != 0 || descriptor.schema_digest == kEmptySchema) &&
        descriptor.offered_qos.size() <= limits.max_qos_profiles);
    // Reuse the common typed text policy without fabricating schema backing.
    const auto token = [](const std::string& value) {
        return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x21 && byte <= 0x7e;
        });
    };
    require(token(descriptor.message_encoding) && token(descriptor.schema_encoding));
    if (descriptor.data_profile == RODAQ_DATA_PROFILE_GENERIC) {
        require(descriptor.ros2_type_description_hash.empty() && descriptor.offered_qos.empty());
    } else {
        require(
            descriptor.data_profile == RODAQ_DATA_PROFILE_ROS2 && descriptor.schema_size != 0 &&
            !descriptor.schema_name.empty() && descriptor.message_encoding == "cdr" &&
            (descriptor.schema_encoding == "ros2msg" || descriptor.schema_encoding == "ros2idl") &&
            valid_text(descriptor.ros2_type_description_hash, 128, true) &&
            token(descriptor.ros2_type_description_hash));
    }
    for (std::size_t index = 0; index < descriptor.offered_qos.size(); ++index) {
        require(canonical::valid_qos(descriptor.offered_qos[index]));
        if (index != 0) {
            require(
                canonical::qos_key(descriptor.offered_qos[index - 1]) <
                canonical::qos_key(descriptor.offered_qos[index]));
        }
    }
}

std::string_view origin_text(time::ClockEpochOrigin origin) {
    switch (origin) {
        case time::ClockEpochOrigin::Boot:
            return "boot";
        case time::ClockEpochOrigin::AuthorityRestart:
            return "authority_restart";
        case time::ClockEpochOrigin::RealtimeJump:
            return "realtime_jump";
    }
    throw InvalidManifest{Status(StatusCode::InvalidArgument, {})};
}

time::ClockEpochOrigin parse_origin(std::string_view value) {
    for (const auto origin :
         {time::ClockEpochOrigin::Boot, time::ClockEpochOrigin::AuthorityRestart,
          time::ClockEpochOrigin::RealtimeJump}) {
        if (origin_text(origin) == value) {
            return origin;
        }
    }
    throw InvalidManifest{Status(StatusCode::InvalidArgument, {})};
}

void validate_identity(const ManifestIdentity& identity) {
    require(
        identity.segment_id != 0 && uuid::nonzero(identity.task_id.value) &&
        identity.pipeline_id != 0 && identity.segment_sequence != 0 &&
        identity.control_generation != 0 && uuid::nonzero(identity.volume_id.value) &&
        persistence::valid_digest(identity.config_version) &&
        persistence::valid_digest(identity.effective_config_id) &&
        valid_text(identity.qualified_runtime_profile_id, 256) && valid_text(identity.format, 128));
    take(uuid::parse(identity.instance_id));
    validate_plugin(identity.acquisition);
    validate_plugin(identity.storage);
    require(identity.playback_anchor.boot_id != 0);
    if (identity.effective_configuration) {
        const auto& effective = *identity.effective_configuration;
        require(effective.is_object());
        require(
            take(persistence::semantic_digest(effective, "rodaq.effective-config.v1")) ==
            identity.effective_config_id);
    }
}

void validate(const SegmentManifest& manifest, const ManifestLimits& limits) {
    valid_limits(limits);
    validate_identity(manifest.identity);
    require(
        !manifest.files.empty() && manifest.files.size() <= limits.max_files &&
        !manifest.descriptors.empty() && manifest.descriptors.size() <= limits.max_descriptors &&
        !manifest.coverage.empty() && manifest.coverage.size() <= limits.max_streams &&
        !manifest.epochs.empty() && manifest.epochs.size() <= limits.max_epochs);
    std::uint64_t file_bytes = 0;
    for (std::size_t index = 0; index < manifest.files.size(); ++index) {
        const auto& file = manifest.files[index];
        require(
            file.relative_path.size() <= 4096 &&
            storage::abi::valid_relative_path(file.relative_path) &&
            file.relative_path != "manifest.json" && persistence::valid_digest(file.digest));
        add(file_bytes, file.physical_size);
        for (std::size_t previous = 0; previous < index; ++previous) {
            require(manifest.files[previous].relative_path != file.relative_path);
        }
    }
    require(
        manifest.identity.playback_anchor.boot_id != 0 &&
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
    std::uint64_t epoch_records = 0;
    std::uint64_t first_receive = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t last_receive = 0;
    for (std::size_t index = 0; index < manifest.epochs.size(); ++index) {
        const auto& epoch = manifest.epochs[index];
        require(
            epoch.epoch_id != 0 && epoch.accepted_records != 0 &&
            epoch.first_receive_monotonic <= epoch.last_receive_monotonic &&
            epoch.first_receive_monotonic >= manifest.first_receive_monotonic &&
            epoch.last_receive_monotonic <= manifest.last_receive_monotonic &&
            epoch.jump.has_value() == (epoch.origin == time::ClockEpochOrigin::RealtimeJump));
        (void)origin_text(epoch.origin);
        if (epoch.jump) {
            require(
                epoch.jump->previous_epoch_id != 0 &&
                epoch.jump->previous_epoch_id != epoch.epoch_id && epoch.jump->delta_ns != 0 &&
                epoch.jump->detected_monotonic_ns <= epoch.first_receive_monotonic);
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            require(manifest.epochs[previous].epoch_id != epoch.epoch_id);
        }
        add(epoch_records, epoch.accepted_records);
        first_receive = std::min(first_receive, epoch.first_receive_monotonic);
        last_receive = std::max(last_receive, epoch.last_receive_monotonic);
    }
    require(
        epoch_records == accepted && first_receive == manifest.first_receive_monotonic &&
        last_receive == manifest.last_receive_monotonic);
}

Json qos_document(const rodaq_ros2_qos_profile_t& qos) {
    return {
        {"history", std::to_string(qos.history)},
        {"reliability", std::to_string(qos.reliability)},
        {"durability", std::to_string(qos.durability)},
        {"liveliness", std::to_string(qos.liveliness)},
        {"depth", std::to_string(qos.depth)},
        {"deadline_sec", std::to_string(qos.deadline.sec)},
        {"deadline_nsec", std::to_string(qos.deadline.nsec)},
        {"lifespan_sec", std::to_string(qos.lifespan.sec)},
        {"lifespan_nsec", std::to_string(qos.lifespan.nsec)},
        {"lease_sec", std::to_string(qos.liveliness_lease_duration.sec)},
        {"lease_nsec", std::to_string(qos.liveliness_lease_duration.nsec)},
        {"avoid_ros_namespace_conventions", std::to_string(qos.avoid_ros_namespace_conventions)}};
}

rodaq_ros2_qos_profile_t read_qos(const Json& value) {
    fields(
        value, {"history", "reliability", "durability", "liveliness", "depth", "deadline_sec",
                "deadline_nsec", "lifespan_sec", "lifespan_nsec", "lease_sec", "lease_nsec",
                "avoid_ros_namespace_conventions"});
    rodaq_ros2_qos_profile_t qos{};
    qos.history = narrow(value, "history");
    qos.reliability = narrow(value, "reliability");
    qos.durability = narrow(value, "durability");
    qos.liveliness = narrow(value, "liveliness");
    qos.depth = number(value, "depth");
    qos.deadline = {number(value, "deadline_sec"), number(value, "deadline_nsec")};
    qos.lifespan = {number(value, "lifespan_sec"), number(value, "lifespan_nsec")};
    qos.liveliness_lease_duration = {number(value, "lease_sec"), number(value, "lease_nsec")};
    const auto avoid = number(value, "avoid_ros_namespace_conventions");
    require(avoid <= 1);
    qos.avoid_ros_namespace_conventions = static_cast<std::uint8_t>(avoid);
    require(canonical::valid_qos(qos));
    return qos;
}

Json plugin_document(const plugin::PluginBinaryInfo& value) {
    return {
        {"plugin_id", value.plugin_id},
        {"plugin_version", value.plugin_version},
        {"artifact_sha256", value.sha256}};
}

plugin::PluginBinaryInfo read_plugin(const Json& value) {
    fields(value, {"plugin_id", "plugin_version", "artifact_sha256"});
    return {
        text(value, "plugin_id"), text(value, "plugin_version"), text(value, "artifact_sha256")};
}

Json descriptor_document(const ManifestDescriptor& value) {
    Json qos = Json::array();
    for (const auto& profile : value.offered_qos) {
        qos.push_back(qos_document(profile));
    }
    return {
        {"stream_id", std::to_string(value.stream_id)},
        {"descriptor_id", std::to_string(value.descriptor_id)},
        {"accepted_records", std::to_string(value.accepted_records)},
        {"data_profile", std::to_string(value.data_profile)},
        {"channel_name", value.channel_name},
        {"message_encoding", value.message_encoding},
        {"schema_name", value.schema_name},
        {"schema_encoding", value.schema_encoding},
        {"schema_digest", value.schema_digest},
        {"schema_size", std::to_string(value.schema_size)},
        {"ros2_type_description_hash", value.ros2_type_description_hash},
        {"offered_qos", std::move(qos)}};
}

ManifestDescriptor read_descriptor(const Json& value, const ManifestLimits& limits) {
    fields(
        value, {"stream_id", "descriptor_id", "accepted_records", "data_profile", "channel_name",
                "message_encoding", "schema_name", "schema_encoding", "schema_digest",
                "schema_size", "ros2_type_description_hash", "offered_qos"});
    ManifestDescriptor result;
    result.stream_id = number(value, "stream_id");
    result.descriptor_id = number(value, "descriptor_id");
    result.accepted_records = number(value, "accepted_records");
    result.data_profile = narrow(value, "data_profile");
    result.channel_name = text(value, "channel_name");
    result.message_encoding = text(value, "message_encoding");
    result.schema_name = text(value, "schema_name");
    result.schema_encoding = text(value, "schema_encoding");
    result.schema_digest = text(value, "schema_digest");
    result.schema_size = narrow(value, "schema_size");
    result.ros2_type_description_hash = text(value, "ros2_type_description_hash");
    require(
        value.at("offered_qos").is_array() &&
        value.at("offered_qos").size() <= limits.max_qos_profiles);
    for (const auto& profile : value.at("offered_qos")) {
        result.offered_qos.push_back(read_qos(profile));
    }
    return result;
}

Json document(const SegmentManifest& manifest) {
    Json files = Json::array();
    for (const auto& value : manifest.files) {
        files.push_back(
            {{"relative_path", value.relative_path},
             {"physical_size", std::to_string(value.physical_size)},
             {"digest", value.digest}});
    }
    Json descriptors = Json::array();
    for (const auto& value : manifest.descriptors) {
        descriptors.push_back(descriptor_document(value));
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
    Json epochs = Json::array();
    for (const auto& value : manifest.epochs) {
        Json jump = nullptr;
        if (value.jump) {
            jump = {
                {"previous_epoch_id", std::to_string(value.jump->previous_epoch_id)},
                {"delta_ns", std::to_string(value.jump->delta_ns)},
                {"detected_monotonic_ns", std::to_string(value.jump->detected_monotonic_ns)}};
        }
        epochs.push_back(
            {{"epoch_id", std::to_string(value.epoch_id)},
             {"origin", origin_text(value.origin)},
             {"jump", std::move(jump)},
             {"accepted_records", std::to_string(value.accepted_records)},
             {"first_receive_monotonic", std::to_string(value.first_receive_monotonic)},
             {"last_receive_monotonic", std::to_string(value.last_receive_monotonic)},
             {"max_sample_error_bound_ns", std::to_string(value.max_sample_error_bound_ns)}});
    }
    Json result{
        {"format_version", "1.0"},
        {"segment_id", std::to_string(manifest.identity.segment_id)},
        {"task_id", to_string(manifest.identity.task_id)},
        {"pipeline_id", std::to_string(manifest.identity.pipeline_id)},
        {"instance_id", manifest.identity.instance_id},
        {"segment_sequence", std::to_string(manifest.identity.segment_sequence)},
        {"config_version", manifest.identity.config_version},
        {"effective_config_id", manifest.identity.effective_config_id},
        {"control_generation", std::to_string(manifest.identity.control_generation)},
        {"reset_generation_at_commit",
         std::to_string(manifest.identity.reset_generation_at_commit)},
        {"volume_id", to_string(manifest.identity.volume_id)},
        {"acquisition", plugin_document(manifest.identity.acquisition)},
        {"storage", plugin_document(manifest.identity.storage)},
        {"qualified_runtime_profile_id", manifest.identity.qualified_runtime_profile_id},
        {"format", manifest.identity.format},
        {"logical_bytes", std::to_string(manifest.logical_bytes)},
        {"files", std::move(files)},
        {"descriptors", std::move(descriptors)},
        {"coverage", std::move(coverage)},
        {"system_boot_id", std::to_string(manifest.identity.playback_anchor.boot_id)},
        {"playback_anchor_monotonic",
         std::to_string(manifest.identity.playback_anchor.monotonic_ns)},
        {"playback_anchor_realtime", std::to_string(manifest.identity.playback_anchor.realtime_ns)},
        {"clock_epochs", std::move(epochs)},
        {"first_receive_monotonic", std::to_string(manifest.first_receive_monotonic)},
        {"last_receive_monotonic", std::to_string(manifest.last_receive_monotonic)},
        {"min_receive_realtime", std::to_string(manifest.min_receive_realtime)},
        {"max_receive_realtime", std::to_string(manifest.max_receive_realtime)},
        {"recording_begin_ns", std::to_string(manifest.recording_begin_ns)},
        {"recording_end_ns", std::to_string(manifest.recording_end_ns)}};
    if (manifest.identity.effective_configuration) {
        result["effective_configuration"] = *manifest.identity.effective_configuration;
    }
    return result;
}

}  // namespace

Result<ManifestDescriptor> capture_manifest_descriptor(
    const storage::StreamDescriptor& descriptor, std::uint64_t accepted_records,
    std::uint32_t max_schema_bytes, std::uint32_t max_qos_profiles) {
    if (descriptor.schema.size > max_schema_bytes || max_qos_profiles > 256 ||
        descriptor.ros2_offered_qos_profile_count > max_qos_profiles ||
        !storage::abi::valid_descriptor(&descriptor)) {
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
        result.data_profile = descriptor.data_profile;
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
        result.ros2_type_description_hash = copy(descriptor.ros2_type_description_hash);
        if (descriptor.ros2_offered_qos_profile_count != 0) {
            result.offered_qos.assign(
                descriptor.ros2_offered_qos_profiles,
                descriptor.ros2_offered_qos_profiles + descriptor.ros2_offered_qos_profile_count);
            std::sort(
                result.offered_qos.begin(), result.offered_qos.end(),
                [](const auto& left, const auto& right) {
                    return canonical::qos_key(left) < canonical::qos_key(right);
                });
            result.offered_qos.erase(
                std::unique(
                    result.offered_qos.begin(), result.offered_qos.end(),
                    [](const auto& left, const auto& right) {
                        return canonical::qos_key(left) == canonical::qos_key(right);
                    }),
                result.offered_qos.end());
        }
        return result;
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

Result<std::size_t> manifest_base_size_bound(
    const ManifestIdentity& identity, const ManifestLimits& limits) {
    try {
        valid_limits(limits);
        validate_identity(identity);
        SegmentManifest sizing;
        sizing.identity = identity;
        sizing.first_receive_monotonic = std::numeric_limits<std::uint64_t>::max();
        sizing.last_receive_monotonic = std::numeric_limits<std::uint64_t>::max();
        sizing.min_receive_realtime = std::numeric_limits<std::int64_t>::min();
        sizing.max_receive_realtime = std::numeric_limits<std::int64_t>::min();
        sizing.recording_begin_ns = std::numeric_limits<std::uint64_t>::max();
        sizing.recording_end_ns = std::numeric_limits<std::uint64_t>::max();
        sizing.logical_bytes = std::numeric_limits<std::uint64_t>::max();
        auto shape = document(sizing);
        shape["content_digest"] = "sha256:" + std::string(64, '0');
        auto sizing_limits = json_limits(limits);
        sizing_limits.max_bytes = 64 * 1024 * 1024;
        auto encoded = persistence::canonical_json(shape, sizing_limits);
        if (!encoded.ok()) {
            return encoded.status();
        }
        return encoded.value().size();
    } catch (InvalidManifest& failure) {
        return std::move(failure.status);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::size_t> manifest_descriptor_size_bound(
    const ManifestDescriptor& descriptor, const ManifestLimits& limits) {
    try {
        valid_limits(limits);
        validate_descriptor(descriptor, limits);
        auto sizing = descriptor;
        sizing.accepted_records = std::numeric_limits<std::uint64_t>::max();
        auto sizing_limits = json_limits(limits);
        sizing_limits.max_bytes = 64 * 1024 * 1024;
        auto encoded = persistence::canonical_json(descriptor_document(sizing), sizing_limits);
        if (!encoded.ok()) {
            return encoded.status();
        }
        return encoded.value().size();
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
        return persistence::encode_integrity(document(manifest), kDomain, json_limits(limits));
    } catch (InvalidManifest& failure) {
        return std::move(failure.status);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::string> manifest_digest(const SegmentManifest& manifest, const ManifestLimits& limits) {
    try {
        validate(manifest, limits);
        return persistence::semantic_digest(document(manifest), kDomain, json_limits(limits));
    } catch (InvalidManifest& failure) {
        return std::move(failure.status);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<SegmentManifest> decode_manifest(std::string_view encoded, const ManifestLimits& limits) {
    try {
        valid_limits(limits);
        const auto value = take(persistence::parse_json(encoded, json_limits(limits)));
        fields(
            value,
            {"format_version",
             "segment_id",
             "task_id",
             "pipeline_id",
             "instance_id",
             "segment_sequence",
             "config_version",
             "effective_config_id",
             "control_generation",
             "reset_generation_at_commit",
             "volume_id",
             "acquisition",
             "storage",
             "qualified_runtime_profile_id",
             "format",
             "logical_bytes",
             "files",
             "descriptors",
             "coverage",
             "system_boot_id",
             "playback_anchor_monotonic",
             "playback_anchor_realtime",
             "clock_epochs",
             "first_receive_monotonic",
             "last_receive_monotonic",
             "min_receive_realtime",
             "max_receive_realtime",
             "recording_begin_ns",
             "recording_end_ns",
             "content_digest"},
            {"effective_configuration"});
        if (text(value, "format_version") != "1.0") {
            return Status(StatusCode::Unavailable, {});
        }
        SegmentManifest manifest;
        manifest.identity.segment_id = number(value, "segment_id");
        manifest.identity.task_id = take(parse_task_id(text(value, "task_id")));
        manifest.identity.pipeline_id = number(value, "pipeline_id");
        manifest.identity.instance_id = text(value, "instance_id");
        manifest.identity.segment_sequence = number(value, "segment_sequence");
        manifest.identity.config_version = text(value, "config_version");
        manifest.identity.effective_config_id = text(value, "effective_config_id");
        if (value.contains("effective_configuration")) {
            manifest.identity.effective_configuration = value.at("effective_configuration");
        }
        manifest.identity.control_generation = number(value, "control_generation");
        manifest.identity.reset_generation_at_commit = number(value, "reset_generation_at_commit");
        manifest.identity.volume_id = take(parse_volume_id(text(value, "volume_id")));
        manifest.identity.acquisition = read_plugin(value.at("acquisition"));
        manifest.identity.storage = read_plugin(value.at("storage"));
        manifest.identity.qualified_runtime_profile_id =
            text(value, "qualified_runtime_profile_id");
        manifest.identity.format = text(value, "format");
        manifest.logical_bytes = number(value, "logical_bytes");
        for (const auto* list : {"files", "descriptors", "coverage", "clock_epochs"}) {
            require(value.at(list).is_array());
        }
        require(
            value.at("files").size() <= limits.max_files &&
            value.at("descriptors").size() <= limits.max_descriptors &&
            value.at("coverage").size() <= limits.max_streams &&
            value.at("clock_epochs").size() <= limits.max_epochs);
        for (const auto& file : value.at("files")) {
            fields(file, {"relative_path", "physical_size", "digest"});
            manifest.files.push_back(
                {text(file, "relative_path"), number(file, "physical_size"), text(file, "digest")});
        }
        for (const auto& descriptor : value.at("descriptors")) {
            manifest.descriptors.push_back(read_descriptor(descriptor, limits));
        }
        for (const auto& coverage : value.at("coverage")) {
            fields(
                coverage, {"stream_id", "accepted", "written", "known_failed", "unknown",
                           "first_sequence", "last_sequence"});
            manifest.coverage.push_back(
                {number(coverage, "stream_id"), number(coverage, "accepted"),
                 number(coverage, "written"), number(coverage, "known_failed"),
                 number(coverage, "unknown"), number(coverage, "first_sequence"),
                 number(coverage, "last_sequence")});
        }
        manifest.identity.playback_anchor = {
            number(value, "system_boot_id"), number(value, "playback_anchor_monotonic"),
            signed_number(value, "playback_anchor_realtime")};
        for (const auto& epoch : value.at("clock_epochs")) {
            fields(
                epoch, {"epoch_id", "origin", "jump", "accepted_records", "first_receive_monotonic",
                        "last_receive_monotonic", "max_sample_error_bound_ns"});
            ManifestEpoch summary;
            summary.epoch_id = number(epoch, "epoch_id");
            summary.origin = parse_origin(text(epoch, "origin"));
            summary.accepted_records = number(epoch, "accepted_records");
            summary.first_receive_monotonic = number(epoch, "first_receive_monotonic");
            summary.last_receive_monotonic = number(epoch, "last_receive_monotonic");
            summary.max_sample_error_bound_ns = number(epoch, "max_sample_error_bound_ns");
            if (!epoch.at("jump").is_null()) {
                const auto& jump = epoch.at("jump");
                fields(jump, {"previous_epoch_id", "delta_ns", "detected_monotonic_ns"});
                summary.jump = time::ClockJump{
                    number(jump, "previous_epoch_id"), signed_number(jump, "delta_ns"),
                    number(jump, "detected_monotonic_ns")};
            }
            manifest.epochs.push_back(summary);
        }
        manifest.first_receive_monotonic = number(value, "first_receive_monotonic");
        manifest.last_receive_monotonic = number(value, "last_receive_monotonic");
        manifest.min_receive_realtime = signed_number(value, "min_receive_realtime");
        manifest.max_receive_realtime = signed_number(value, "max_receive_realtime");
        manifest.recording_begin_ns = number(value, "recording_begin_ns");
        manifest.recording_end_ns = number(value, "recording_end_ns");
        validate(manifest, limits);
        take(persistence::verify_integrity(value, kDomain, json_limits(limits)));
        return manifest;
    } catch (InvalidManifest& failure) {
        return failure.status.code() == StatusCode::InvalidArgument
                   ? Status(StatusCode::DataLoss, {})
                   : std::move(failure.status);
    } catch (const Json::exception&) {
        return Status(StatusCode::DataLoss, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::artifact
