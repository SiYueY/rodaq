#include "storage.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <new>
#include <tuple>
#include <unistd.h>

#include <rodaq/storage/capacity.hpp>
#include <rodaq/storage/validation.hpp>

#include "config.hpp"
#include "qos.hpp"
#include "rodaqmeta.hpp"

namespace rodaq::mcap_storage {
namespace {

constexpr std::string_view kLibrary = "rodaq/1.0";
constexpr std::string_view kProfile = "ros2";
constexpr std::string_view kDistro = RODAQ_MCAP_ROS_DISTRO;
constexpr std::uint64_t kMaxSchemaBytes = 1024 * 1024;
constexpr std::uint64_t kMaxCatalogBytes = 64 * 1024 * 1024;
constexpr std::uint32_t kMaxDescriptors = 65535;

std::string text(rodaq_string_t value) { return {value.data, value.size}; }

bool add(std::uint64_t& target, std::uint64_t amount) noexcept {
    if (amount > std::numeric_limits<std::uint64_t>::max() - target) {
        return false;
    }
    target += amount;
    return true;
}

bool same_schema(const mcap::Schema& a, const mcap::Schema& b) {
    // Exact bytes provide a stronger equality proof than a digest alone.
    return a.name == b.name && a.encoding == b.encoding && a.data == b.data;
}

}  // namespace

struct StorageBackend::Descriptor {
    mcap::Schema schema;
    mcap::Channel channel;
    std::uint64_t input_bytes{0};
    std::uint64_t segment{0};
    mcap::SchemaId schema_id{0};
    mcap::ChannelId channel_id{0};
};

bool StorageBackend::SchemaLess::operator()(const Descriptor* left, const Descriptor* right) const {
    return std::tie(left->schema.name, left->schema.encoding, left->schema.data) <
           std::tie(right->schema.name, right->schema.encoding, right->schema.data);
}

storage::CapacityModel capacity_model(std::string_view recorder_distro) noexcept {
    static_assert(sizeof(rodaq_ros2_qos_profile_t) == 80);
    constexpr auto kQosExpansion = (kMaxEncodedQosBytes + 79) / 80;
    // Schema + Channel framing, both metadata key/value length prefixes and
    // the empty QoS sequence. Every variable input byte is covered by expansion.
    constexpr auto kDescriptorFraming =
        23 + 25 + 8 + kOfferedQosKey.size() + 8 + kTypeHashKey.size() + 3;
    // Magic/Header + one fixed rosbag2 metadata record + sidecar Header.
    const auto fixed = 8 + 9 + 4 + kProfile.size() + 4 + kLibrary.size() + 9 + 4 +
                       std::string_view("rosbag2").size() + 4 + 4 +
                       std::string_view("ROS_DISTRO").size() + 4 + recorder_distro.size() +
                       kMetaHeaderBytes;
    // DataEnd + Footer + trailing magic + sidecar Footer, with no Summary.
    constexpr auto kSeal = (9 + 4) + (9 + 20) + 8 + kMetaFooterBytes;
    return {
        {sizeof(storage::CapacityModel)},
        1,
        1,
        31 + kMetaMaxEntryBytes,
        kQosExpansion,
        1,
        kDescriptorFraming,
        fixed,
        kSeal};
}

StorageBackend::StorageBackend(CheckedSink::Write mcap_write, CheckedSink::Write meta_write)
: mcap_write_(std::move(mcap_write)), meta_write_(std::move(meta_write)) {}

StorageBackend::~StorageBackend() noexcept {
    if (writer_) {
        writer_->terminate();
        writer_.reset();
    }
}

void StorageBackend::fail(StatusCode error) noexcept {
    if (failure_ == StatusCode::Ok) {
        failure_ = error;
    }
    phase_ = Phase::Failed;
}

Result<void> StorageBackend::configure(std::string_view input) {
    if (phase_ != Phase::New) {
        return Status(StatusCode::InvalidState, {});
    }
    auto valid = validate_config(input);
    if (valid.ok()) {
        phase_ = Phase::Configured;
    }
    return valid;
}

Result<storage::CapacityModel> StorageBackend::prepare(
    const storage::PrepareConfig& config, plugin::Deadline) {
    if (phase_ != Phase::Configured) {
        return Status(StatusCode::InvalidState, {});
    }
    if (!storage::abi::valid_prepare(&config)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (config.max_descriptors_per_segment > kMaxDescriptors || config.max_segment_file_count < 2 ||
        config.max_relative_path_bytes < 30) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    auto model = capacity_model(kDistro);
    auto maximum = storage::record_bound(model, config.max_record_bytes);
    if (!maximum.ok()) {
        return maximum.status();
    }
    auto fixed = model.fixed_segment_overhead_bytes;
    if (!add(fixed, model.seal_reserve_bytes) || !add(fixed, maximum.value()) ||
        fixed > config.max_segment_physical_bytes) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    limits_ = config;
    model_ = model;
    phase_ = Phase::Prepared;
    return model_;
}

Result<void> StorageBackend::prepare_descriptor(
    const storage::StreamDescriptor& input, plugin::Deadline) {
    if (phase_ != Phase::Prepared && phase_ != Phase::Open && phase_ != Phase::Sealed) {
        return Status(StatusCode::InvalidState, {});
    }
    if (input.schema.size > kMaxSchemaBytes ||
        input.ros2_offered_qos_profile_count > kMaxQosProfiles) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    if (!storage::abi::valid_descriptor(&input) || input.data_profile != RODAQ_DATA_PROFILE_ROS2) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const DescriptorKey key{input.stream_id, input.descriptor_id};
    auto existing = descriptors_.find(key);
    if (existing == descriptors_.end() && descriptors_.size() == kMaxDescriptors) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    auto qos = encode_qos(input.ros2_offered_qos_profiles, input.ros2_offered_qos_profile_count);
    if (!qos.ok()) {
        return qos.status();
    }
    Descriptor descriptor;
    auto schema = mcap::Schema(
        text(input.schema_name), text(input.schema_encoding),
        std::string_view(reinterpret_cast<const char*>(input.schema.data), input.schema.size));
    schema.id = 0;
    descriptor.schema = std::move(schema);
    auto channel = mcap::Channel(text(input.channel_name), text(input.message_encoding), 0);
    channel.id = 0;
    descriptor.channel = std::move(channel);
    descriptor.channel.metadata.emplace(kOfferedQosKey, std::move(qos.value()));
    if (input.ros2_type_description_hash.size != 0) {
        auto hash = text(input.ros2_type_description_hash);
        if (hash.size() != 71 || hash.substr(0, 7) != "RIHS01_" ||
            !std::all_of(hash.begin() + 7, hash.end(), [](unsigned char byte) {
                return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
            })) {
            return Status(StatusCode::InvalidArgument, {});
        }
        descriptor.channel.metadata.emplace(kTypeHashKey, std::move(hash));
    }
    descriptor.input_bytes = input.schema.size + input.schema_name.size +
                             input.schema_encoding.size + input.channel_name.size +
                             input.message_encoding.size + input.ros2_type_description_hash.size +
                             input.ros2_offered_qos_profile_count * std::uint64_t{80};
    if (existing != descriptors_.end()) {
        const auto& old = existing->second;
        return same_schema(old.schema, descriptor.schema) &&
                       old.channel.topic == descriptor.channel.topic &&
                       old.channel.messageEncoding == descriptor.channel.messageEncoding &&
                       old.channel.metadata == descriptor.channel.metadata
                   ? Result<void>{}
                   : Result<void>{Status(StatusCode::Conflict, {})};
    }
    auto bound = storage::descriptor_bound(model_, descriptor.input_bytes);
    if (!bound.ok()) {
        return bound.status();
    }
    if (bound.value() > kMaxCatalogBytes - catalog_bytes_) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    descriptors_.emplace(key, std::move(descriptor));
    catalog_bytes_ += bound.value();
    return {};
}

Result<void> StorageBackend::open_segment(const storage::SegmentSpec& segment, plugin::Deadline) {
    if (phase_ != Phase::Prepared && phase_ != Phase::Sealed) {
        return Status(StatusCode::InvalidState, {});
    }
    if (!storage::abi::valid_segment(&segment) || segment.staging_directory.size > 4095 ||
        segment.staging_directory.data[0] != '/' || segment.segment_id == segment_id_) {
        return Status(StatusCode::InvalidArgument, {});
    }
    // Once any file effect can happen, every exceptional/failed exit is a
    // contaminated Segment until actual quiesce. No retry or ordinary Seal.
    phase_ = Phase::Failed;
    try {
        if (writer_) {
            writer_->terminate();
            writer_.reset();
        }
        mcap_ = std::make_unique<CheckedSink>(mcap_write_);
        meta_ = std::make_unique<CheckedSink>(meta_write_);
        writer_ = std::make_unique<mcap::McapWriter>();
        mcap_name_ = std::to_string(segment.segment_id) + ".mcap";
        meta_name_ = std::to_string(segment.segment_id) + ".rodaqmeta";
        const auto directory_name = text(segment.staging_directory);
        const int directory =
            ::open(directory_name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directory < 0) {
            fail(StatusCode::Unavailable);
            return Status(failure_, {});
        }
        auto opened = mcap_->open(directory, mcap_name_.c_str());
        if (opened.ok()) {
            opened = meta_->open(directory, meta_name_.c_str());
        }
        ::close(directory);
        if (!opened.ok()) {
            fail(opened.status().code());
            return opened.status();
        }
        mcap::McapWriterOptions options{std::string(kProfile)};
        options.library = kLibrary;
        options.noChunking = true;
        options.noSummary = true;
        options.noMessageIndex = true;
        options.noChunkCRC = true;
        options.compression = mcap::Compression::None;
        writer_->open(*mcap_, options);
        mcap::Metadata metadata;
        metadata.name = "rosbag2";
        metadata.metadata.emplace("ROS_DISTRO", kDistro);
        const auto written = writer_->write(metadata);
        const auto header = encode_meta_header();
        meta_->write(header.data(), header.size());
        if (!written.ok() || mcap_->status() != StatusCode::Ok ||
            meta_->status() != StatusCode::Ok) {
            fail(
                mcap_->status() != StatusCode::Ok   ? mcap_->status()
                : meta_->status() != StatusCode::Ok ? meta_->status()
                                                    : StatusCode::DataLoss);
            return Status(failure_, {});
        }
        segment_id_ = segment.segment_id;
        paired_records_ = 0;
        input_bytes_ = 0;
        channel_count_ = 0;
        schemas_.clear();
        phase_ = Phase::Open;
        return {};
    } catch (const std::bad_alloc&) {
        fail(StatusCode::ResourceExhausted);
    } catch (...) {
        fail(StatusCode::Internal);
    }
    return Status(failure_, {});
}

Result<std::uint64_t> StorageBackend::playback_time(const storage::Record& record) const {
    if (record.system_boot_id != limits_.system_boot_id ||
        record.receive_monotonic < limits_.playback_anchor_monotonic) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto offset = record.receive_monotonic - limits_.playback_anchor_monotonic;
    const auto anchor = limits_.playback_anchor_realtime;
    std::uint64_t result;
    if (anchor < 0) {
        const auto magnitude = static_cast<std::uint64_t>(-(anchor + 1)) + 1;
        if (offset < magnitude) {
            return Status(StatusCode::InvalidArgument, {});
        }
        result = offset - magnitude;
    } else {
        result = static_cast<std::uint64_t>(anchor);
        if (!add(result, offset)) {
            return Status(StatusCode::ResourceExhausted, {});
        }
    }
    if (result > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    return result;
}

Result<void> StorageBackend::materialize(Descriptor& descriptor) {
    if (descriptor.segment == segment_id_) {
        return {};
    }
    if (channel_count_ == limits_.max_descriptors_per_segment) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    auto schema = schemas_.find(&descriptor);
    if (schema == schemas_.end()) {
        if (schemas_.size() == kMaxDescriptors) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        auto native = descriptor.schema;
        writer_->addSchema(native);
        if (native.id == 0) {
            return Status(StatusCode::DataLoss, {});
        }
        descriptor.schema_id = native.id;
        schemas_.emplace(&descriptor, native.id);
    } else {
        descriptor.schema_id = schema->second;
    }
    auto native = descriptor.channel;
    native.schemaId = descriptor.schema_id;
    writer_->addChannel(native);
    descriptor.channel_id = native.id;
    descriptor.segment = segment_id_;
    ++channel_count_;
    return {};
}

storage::WriteOutcome StorageBackend::write_batch(storage::Batch batch, plugin::Deadline) {
    if (phase_ != Phase::Open) {
        return Status(StatusCode::InvalidState, {});
    }
    if (batch.count > limits_.max_batch_records ||
        batch.count > limits_.max_segment_records - paired_records_) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    if (!storage::abi::valid_batch(batch)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::uint64_t bytes = 0;
    for (std::uint32_t i = 0; i < batch.count; ++i) {
        const auto& record = batch.records[i];
        const auto projected = playback_time(record);
        const auto encoded = encode_meta_entry(record, 0);
        if (!projected.ok() || !encoded.ok()) {
            return projected.ok() ? encoded.status() : projected.status();
        }
        if (record.payload.size > limits_.max_record_bytes || !add(bytes, record.payload.size) ||
            descriptors_.find({record.stream_id, record.descriptor_id}) == descriptors_.end()) {
            return Status(StatusCode::InvalidArgument, {});
        }
    }
    if (bytes > limits_.max_batch_bytes || bytes > limits_.max_segment_input_bytes - input_bytes_) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    storage::BatchResult outcome{
        {sizeof(storage::BatchResult)}, 0, RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN};
    try {
        for (std::uint32_t i = 0; i < batch.count; ++i) {
            const auto& record = batch.records[i];
            auto& descriptor = descriptors_.at({record.stream_id, record.descriptor_id});
            const auto record_bytes = storage::record_bound(model_, record.payload.size);
            if (!record_bytes.ok()) {
                return {record_bytes.status(), outcome};
            }
            auto bound = record_bytes.value();
            if (descriptor.segment != segment_id_) {
                const auto descriptor_bytes =
                    storage::descriptor_bound(model_, descriptor.input_bytes);
                if (!descriptor_bytes.ok() || !add(bound, descriptor_bytes.value())) {
                    return {Status(StatusCode::ResourceExhausted, {}), outcome};
                }
            }
            const auto usage = segment_usage();
            if (!usage.ok()) {
                return {usage.status(), outcome};
            }
            auto maximum = usage.value().physical_bytes;
            if (!add(maximum, bound) || !add(maximum, model_.seal_reserve_bytes) ||
                maximum > limits_.max_segment_physical_bytes) {
                return {Status(StatusCode::ResourceExhausted, {}), outcome};
            }
            outcome.tail_state = RODAQ_STORAGE_TAIL_UNKNOWN;
            auto ready = materialize(descriptor);
            if (!ready.ok()) {
                fail(ready.status().code());
                return {ready.status(), outcome};
            }
            const auto sidecar = encode_meta_entry(record, paired_records_).value();
            mcap::Message message;
            message.channelId = descriptor.channel_id;
            message.sequence = 0;
            message.logTime = playback_time(record).value();
            message.publishTime = message.logTime;
            for (std::uint32_t t = 0; t < record.timestamp_count; ++t) {
                const auto& timestamp = record.timestamps[t];
                if (timestamp.role == RODAQ_TIMESTAMP_ROLE_SOURCE_PUBLICATION &&
                    timestamp.clock_domain == RODAQ_CLOCK_DOMAIN_UNIX_EPOCH &&
                    timestamp.value >= 0) {
                    message.publishTime = static_cast<std::uint64_t>(timestamp.value);
                    break;
                }
            }
            message.data = reinterpret_cast<const std::byte*>(record.payload.data);
            message.dataSize = record.payload.size;
            const auto written = writer_->write(message);
            if (!written.ok() || mcap_->status() != StatusCode::Ok) {
                fail(mcap_->status() != StatusCode::Ok ? mcap_->status() : StatusCode::DataLoss);
                return {Status(failure_, {}), outcome};
            }
            meta_->write(sidecar.bytes.data(), sidecar.size);
            if (meta_->status() != StatusCode::Ok) {
                fail(meta_->status());
                return {Status(failure_, {}), outcome};
            }
            ++paired_records_;
            input_bytes_ += record.payload.size;
            ++outcome.confirmed_prefix;
            outcome.tail_state = outcome.confirmed_prefix == batch.count
                                     ? RODAQ_STORAGE_TAIL_NONE
                                     : RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN;
        }
        return {Status{}, outcome};
    } catch (const std::bad_alloc&) {
        fail(StatusCode::ResourceExhausted);
    } catch (...) {
        fail(StatusCode::Internal);
    }
    outcome.tail_state = RODAQ_STORAGE_TAIL_UNKNOWN;
    return {Status(failure_, {}), outcome};
}

Result<storage::SegmentUsage> StorageBackend::segment_usage() const {
    if (phase_ == Phase::New || phase_ == Phase::Configured) {
        return Status(StatusCode::InvalidState, {});
    }
    storage::SegmentUsage usage{{sizeof(storage::SegmentUsage)}, 0, 0, 0, 0};
    for (const auto* sink : {mcap_.get(), meta_.get()}) {
        if (sink && sink->file_created()) {
            if (!add(usage.physical_bytes, sink->confirmed_bytes())) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            ++usage.file_count;
        }
    }
    return usage;
}

Result<storage::SegmentSealResult> StorageBackend::seal_segment(plugin::Deadline) {
    if (phase_ != Phase::Open || paired_records_ == 0) {
        return Status(StatusCode::InvalidState, {});
    }
    phase_ = Phase::Failed;
    try {
        auto footer = encode_meta_footer({paired_records_, meta_->confirmed_bytes()});
        if (!footer.ok()) {
            fail(footer.status().code());
            return footer.status();
        }
        writer_->close();
        meta_->write(footer.value().data(), footer.value().size());
        meta_->end();
        if (mcap_->status() != StatusCode::Ok || meta_->status() != StatusCode::Ok) {
            fail(mcap_->status() != StatusCode::Ok ? mcap_->status() : meta_->status());
            return Status(failure_, {});
        }
        const auto total = segment_usage();
        if (!total.ok() || total.value().physical_bytes > limits_.max_segment_physical_bytes) {
            fail(StatusCode::DataLoss);
            return Status(failure_, {});
        }
        storage::SegmentSealResult sealed{
            {{mcap_name_, mcap_->confirmed_bytes()}, {meta_name_, meta_->confirmed_bytes()}},
            total.value().physical_bytes};
        phase_ = Phase::Sealed;
        return sealed;
    } catch (const std::bad_alloc&) {
        fail(StatusCode::ResourceExhausted);
    } catch (...) {
        fail(StatusCode::Internal);
    }
    return Status(failure_, {});
}

Result<void> StorageBackend::quiesce(plugin::Deadline) {
    if (writer_) {
        writer_->terminate();
    }
    if (mcap_) {
        mcap_->end();
    }
    if (meta_) {
        meta_->end();
    }
    phase_ = Phase::Stopped;
    return {};
}

}  // namespace rodaq::mcap_storage
