#include "storage.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <new>
#include <tuple>
#include <unistd.h>

#include <rodaq/storage/validation.hpp>

#include "config.hpp"
#include "qos.hpp"

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
    return a.name == b.name && a.encoding == b.encoding && a.data == b.data;
}

Result<void> check_deadline(plugin::Deadline deadline) {
    auto remaining = deadline.remaining();
    if (!remaining.ok()) {
        return remaining.status();
    }
    if (remaining.value() <= std::chrono::nanoseconds::zero()) {
        return Status(StatusCode::DeadlineExceeded, {});
    }
    return {};
}

}  // namespace

struct StorageBackend::Descriptor {
    mcap::Schema schema;
    mcap::Channel channel;
    std::uint64_t input_bytes{0};
    std::uint64_t segment{0};
    mcap::SchemaId schema_id{0};
    mcap::ChannelId channel_id{0};
    std::uint32_t next_sequence{0};
};

bool StorageBackend::SchemaLess::operator()(const Descriptor* left, const Descriptor* right) const {
    return std::tie(left->schema.name, left->schema.encoding, left->schema.data) <
           std::tie(right->schema.name, right->schema.encoding, right->schema.data);
}

StorageBackend::StorageBackend(CheckedSink::Write mcap_write)
: mcap_write_(std::move(mcap_write)) {}

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

Result<void> StorageBackend::prepare(
    const storage::PrepareConfig& config, plugin::Deadline deadline) {
    if (phase_ != Phase::Configured) {
        return Status(StatusCode::InvalidState, {});
    }
    if (!storage::abi::valid_prepare(&config)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (auto ok = check_deadline(deadline); !ok.ok()) {
        return ok.status();
    }
    if (config.max_descriptors_per_segment > kMaxDescriptors || config.max_segment_file_count < 1 ||
        config.max_relative_path_bytes < 25) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    limits_ = config;
    phase_ = Phase::Prepared;
    return {};
}

Result<void> StorageBackend::prepare_descriptor(
    const storage::StreamDescriptor& input, plugin::Deadline deadline) {
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
    if (auto ok = check_deadline(deadline); !ok.ok()) {
        return ok.status();
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
    if (descriptor.input_bytes > kMaxCatalogBytes - catalog_bytes_) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    catalog_bytes_ += descriptor.input_bytes;
    descriptors_.emplace(key, std::move(descriptor));
    return {};
}

Result<void> StorageBackend::open_segment(
    const storage::SegmentSpec& segment, plugin::Deadline deadline) {
    if (phase_ != Phase::Prepared && phase_ != Phase::Sealed) {
        return Status(StatusCode::InvalidState, {});
    }
    if (!storage::abi::valid_segment(&segment) || segment.staging_directory.size > 4095 ||
        segment.staging_directory.data[0] != '/' || segment.segment_id == segment_id_) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (auto ok = check_deadline(deadline); !ok.ok()) {
        return ok.status();
    }
    // Once any file effect can happen, every exceptional/failed exit is a
    // failed Segment until actual quiesce. No retry or ordinary Seal.
    phase_ = Phase::Failed;
    try {
        if (writer_) {
            writer_->terminate();
            writer_.reset();
        }
        mcap_ = std::make_unique<CheckedSink>(mcap_write_);
        writer_ = std::make_unique<mcap::McapWriter>();
        mcap_name_ = std::to_string(segment.segment_id) + ".mcap";
        const auto directory_name = text(segment.staging_directory);
        const int directory =
            ::open(directory_name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directory < 0) {
            fail(StatusCode::Unavailable);
            return Status(failure_, {});
        }
        auto opened = mcap_->open(directory, mcap_name_.c_str());
        ::close(directory);
        if (!opened.ok()) {
            fail(opened.status().code());
            return opened.status();
        }
        if (auto ok = check_deadline(deadline); !ok.ok()) {
            fail(ok.status().code());
            return ok.status();
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
        if (!written.ok() || mcap_->status() != StatusCode::Ok) {
            fail(mcap_->status() != StatusCode::Ok ? mcap_->status() : StatusCode::DataLoss);
            return Status(failure_, {});
        }
        segment_id_ = segment.segment_id;
        written_records_ = 0;
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
    descriptor.next_sequence = 0;
    ++channel_count_;
    return {};
}

storage::WriteOutcome StorageBackend::write_batch(storage::Batch batch, plugin::Deadline deadline) {
    if (phase_ != Phase::Open) {
        return Status(StatusCode::InvalidState, {});
    }
    if (batch.count > limits_.max_batch_records ||
        batch.count > limits_.max_segment_records - written_records_) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    if (!storage::abi::valid_batch(batch)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::uint64_t bytes = 0;
    for (std::uint32_t i = 0; i < batch.count; ++i) {
        const auto& record = batch.records[i];
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
    if (auto ok = check_deadline(deadline); !ok.ok()) {
        return {ok.status(), outcome};
    }
    try {
        for (std::uint32_t i = 0; i < batch.count; ++i) {
            if (auto ok = check_deadline(deadline); !ok.ok()) {
                return {ok.status(), outcome};
            }
            const auto& record = batch.records[i];
            auto& descriptor = descriptors_.at({record.stream_id, record.descriptor_id});
            if (descriptor.segment == segment_id_ &&
                descriptor.next_sequence == std::numeric_limits<std::uint32_t>::max()) {
                return {Status(StatusCode::ResourceExhausted, "MCAP sequence limit"), outcome};
            }
            outcome.tail_state = RODAQ_STORAGE_TAIL_UNKNOWN;
            auto ready = materialize(descriptor);
            if (!ready.ok()) {
                fail(ready.status().code());
                return {ready.status(), outcome};
            }
            const std::uint32_t sequence = ++descriptor.next_sequence;
            mcap::Message message;
            message.channelId = descriptor.channel_id;
            message.sequence = sequence;
            message.logTime = record.recording_time;
            message.publishTime =
                record.has_publish_time != 0 ? record.publish_time : record.recording_time;
            message.data = reinterpret_cast<const std::byte*>(record.payload.data);
            message.dataSize = record.payload.size;
            const auto written = writer_->write(message);
            if (!written.ok() || mcap_->status() != StatusCode::Ok) {
                fail(mcap_->status() != StatusCode::Ok ? mcap_->status() : StatusCode::DataLoss);
                return {Status(failure_, {}), outcome};
            }
            ++written_records_;
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
    storage::SegmentUsage usage{{sizeof(storage::SegmentUsage)}, 0, 0, 0};
    if (mcap_ && mcap_->file_created()) {
        usage.physical_bytes = mcap_->confirmed_bytes();
        usage.file_count = 1;
    }
    return usage;
}

Result<storage::SegmentSealResult> StorageBackend::seal_segment(plugin::Deadline deadline) {
    if (phase_ != Phase::Open || written_records_ == 0) {
        return Status(StatusCode::InvalidState, {});
    }
    if (auto ok = check_deadline(deadline); !ok.ok()) {
        return ok.status();
    }
    phase_ = Phase::Failed;
    try {
        writer_->close();
        if (mcap_->status() != StatusCode::Ok) {
            fail(mcap_->status());
            return Status(failure_, {});
        }
        mcap_->end();
        if (mcap_->status() != StatusCode::Ok) {
            fail(mcap_->status());
            return Status(failure_, {});
        }
        const auto bytes = mcap_->confirmed_bytes();
        storage::SegmentSealResult sealed{{{mcap_name_, bytes}}, bytes};
        phase_ = Phase::Sealed;
        return sealed;
    } catch (const std::bad_alloc&) {
        fail(StatusCode::ResourceExhausted);
    } catch (...) {
        fail(StatusCode::Internal);
    }
    return Status(failure_, {});
}

Result<void> StorageBackend::quiesce(plugin::Deadline deadline) {
    if (auto ok = check_deadline(deadline); !ok.ok()) {
        return ok.status();
    }
    if (writer_) {
        writer_->terminate();
    }
    if (mcap_) {
        mcap_->end();
    }
    phase_ = Phase::Stopped;
    return {};
}

}  // namespace rodaq::mcap_storage
