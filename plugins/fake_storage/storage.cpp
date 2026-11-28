#include <rodaq/plugin/authoring.hpp>
#include <rodaq/storage/interface.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace rodaq::fake_storage {
namespace {

namespace storage = rodaq::storage;
using rodaq::Result;
using rodaq::Status;
using rodaq::StatusCode;
using rodaq::plugin::Deadline;

// Qualification-only backend. Single-file little-endian length-prefixed format.
class FakeStorage {
public:
    FakeStorage() = default;
    FakeStorage(const FakeStorage&) = delete;
    FakeStorage& operator=(const FakeStorage&) = delete;

    ~FakeStorage() noexcept {
        if (file_ >= 0) {
            close(file_);
        }
    }

    Result<void> configure(std::string_view input) {
        try {
            const auto config = nlohmann::json::parse(input);
            if (!config.is_object() || config.size() > 5) {
                return Status(StatusCode::InvalidArgument, {});
            }
            for (const auto& item : config.items()) {
                if (item.key() != "mode" && item.key() != "prefix" && item.key() != "delay_ms" &&
                    item.key() != "descriptor_bytes" && item.key() != "catalog_slots") {
                    return Status(StatusCode::InvalidArgument, {});
                }
            }
            mode_ = config.value("mode", std::string("normal"));
            constexpr std::string_view modes[]{
                "normal",          "prefix_unwritten", "prefix_unknown", "full_failed",
                "invalid_output",  "late_write",       "hang_write",     "fail_prepare",
                "fail_descriptor", "fail_open",        "fail_seal",      "fail_quiesce",
                "fail_shutdown",   "late_seal"};
            if (std::find(std::begin(modes), std::end(modes), mode_) == std::end(modes)) {
                return Status(StatusCode::InvalidArgument, {});
            }
            auto integer = [&](const char* name, std::uint64_t fallback, std::uint64_t max) {
                if (!config.contains(name)) {
                    return fallback;
                }
                if (!config[name].is_number_unsigned()) {
                    throw std::invalid_argument("unsigned fault option required");
                }
                const auto value = config[name].get<std::uint64_t>();
                if (value > max) {
                    throw std::invalid_argument("fault option exceeds bound");
                }
                return value;
            };
            prefix_ = static_cast<std::uint32_t>(integer("prefix", 1, 1024));
            delay_ms_ = static_cast<std::uint32_t>(integer("delay_ms", 100, 5000));
            descriptor_bytes_ = integer("descriptor_bytes", 65536, 1024 * 1024);
            catalog_slots_ = static_cast<std::uint32_t>(integer("catalog_slots", 64, 1024));
            if (descriptor_bytes_ < 128 || catalog_slots_ == 0) {
                return Status(StatusCode::InvalidArgument, {});
            }
            return {};
        } catch (const nlohmann::json::exception&) {
            return Status(StatusCode::InvalidArgument, {});
        } catch (const std::invalid_argument&) {
            return Status(StatusCode::InvalidArgument, {});
        }
    }

    Result<void> prepare(const storage::PrepareConfig& config, Deadline) {
        if (prepared_ || stopped_ || mode_ == "fail_prepare") {
            return Status(StatusCode::InvalidState, {});
        }
        if (config.max_record_bytes > 1024 * 1024 || config.max_batch_bytes > 4 * 1024 * 1024 ||
            config.max_batch_records > 1024 ||
            config.max_descriptors_per_segment > catalog_slots_ ||
            config.max_segment_file_count < 1 || config.max_relative_path_bytes < 12 ||
            config.max_segment_physical_bytes < 80) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        limits_ = config;
        catalog_.resize(catalog_slots_);
        for (auto& entry : catalog_) {
            entry.encoded.reserve(static_cast<std::size_t>(descriptor_bytes_));
        }
        buffer_.reserve(
            static_cast<std::size_t>(std::max(config.max_record_bytes + 512, descriptor_bytes_)));
        prepared_ = true;
        return {};
    }

    Result<void> prepare_descriptor(const storage::StreamDescriptor& descriptor, Deadline) {
        if (!prepared_ || stopped_ || poisoned_ || mode_ == "fail_descriptor") {
            return Status(StatusCode::InvalidState, {});
        }
        std::uint64_t bytes = 0;
        const std::uint64_t sizes[]{descriptor.channel_name.size, descriptor.message_encoding.size,
                                    descriptor.schema_name.size,  descriptor.schema_encoding.size,
                                    descriptor.schema.size,       descriptor.metadata.format.size,
                                    descriptor.metadata.json.size};
        for (auto size : sizes) {
            if (size > descriptor_bytes_ - 128 - bytes) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            bytes += size;
        }
        buffer_.clear();
        append(1, 1);
        append(0, 4);
        append(descriptor.stream_id, 8);
        append(descriptor.descriptor_id, 8);
        text(descriptor.channel_name);
        text(descriptor.message_encoding);
        text(descriptor.schema_name);
        text(descriptor.schema_encoding);
        blob(descriptor.schema.data, static_cast<std::size_t>(descriptor.schema.size));
        text(descriptor.metadata.format);
        text(descriptor.metadata.json);
        frame_size();
        auto found = std::find_if(catalog_.begin(), catalog_.end(), [&](const auto& entry) {
            return entry.id == descriptor.descriptor_id && entry.stream == descriptor.stream_id;
        });
        if (found != catalog_.end()) {
            return found->encoded == buffer_ ? Result<void>{}
                                             : Result<void>{Status(StatusCode::Conflict, {})};
        }
        found = std::find_if(
            catalog_.begin(), catalog_.end(), [](const auto& entry) { return entry.id == 0; });
        if (found == catalog_.end()) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        found->encoded.assign(buffer_.begin(), buffer_.end());
        found->id = descriptor.descriptor_id;
        found->stream = descriptor.stream_id;
        return {};
    }

    Result<void> open_segment(const storage::SegmentSpec& segment, Deadline) {
        if (!prepared_ || stopped_ || file_ >= 0 || poisoned_ || mode_ == "fail_open") {
            return Status(StatusCode::InvalidState, {});
        }
        if (segment.staging_directory.size > 4095) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        const std::string path(segment.staging_directory.data, segment.staging_directory.size);
        const int directory = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directory < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        file_ = openat(
            directory, "records.fake", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        close(directory);
        if (file_ < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        file_created_ = true;
        physical_ = 0;
        records_ = 0;
        input_bytes_ = 0;
        segment_descriptors_ = 0;
        for (auto& entry : catalog_) {
            entry.emitted = false;
        }
        buffer_.clear();
        for (const auto byte : std::string_view("RDAQFK2\n")) {
            append(static_cast<std::uint8_t>(byte), 1);
        }
        append(segment.segment_id, 8);
        return write_buffer();
    }

    storage::WriteOutcome write_batch(storage::Batch batch, Deadline) {
        if (file_ < 0 || poisoned_ || stopped_) {
            return Status(StatusCode::InvalidState, {});
        }
        if (mode_ == "hang_write") {
            buffer_.clear();
            append(255, 1);
            auto entered = write_buffer();
            if (!entered.ok()) {
                return entered.status();
            }
            for (;;) {
                pause();
            }
        }
        if (mode_ == "late_write") {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }
        if (batch.count > limits_.max_batch_records) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        std::uint64_t batch_bytes = 0;
        for (std::uint32_t i = 0; i < batch.count; ++i) {
            if (batch.records[i].payload.size > limits_.max_batch_bytes - batch_bytes) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            batch_bytes += batch.records[i].payload.size;
        }
        const bool partial = mode_ == "prefix_unwritten" || mode_ == "prefix_unknown";
        const auto confirmed = partial ? std::min(prefix_, batch.count) : batch.count;
        for (std::uint32_t i = 0; i < confirmed; ++i) {
            auto written = write_record(batch.records[i]);
            if (!written.ok()) {
                return {
                    written.status(),
                    {{sizeof(storage::BatchResult)},
                     i,
                     poisoned_ ? RODAQ_STORAGE_TAIL_UNKNOWN
                               : RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN}};
            }
        }
        if (confirmed < batch.count) {
            if (mode_ == "prefix_unknown") {
                (void)write_record(batch.records[confirmed]);
                poisoned_ = true;
            }
            return {
                Status(StatusCode::Unavailable, {}),
                {{sizeof(storage::BatchResult)},
                 confirmed,
                 mode_ == "prefix_unknown" ? RODAQ_STORAGE_TAIL_UNKNOWN
                                           : RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN}};
        }
        return {
            mode_ == "full_failed" ? Status(StatusCode::Unavailable, {}) : Status{},
            {{sizeof(storage::BatchResult)}, batch.count, RODAQ_STORAGE_TAIL_NONE}};
    }

    Result<storage::SegmentUsage> segment_usage() const {
        if (!prepared_) {
            return Status(StatusCode::InvalidState, {});
        }
        return storage::SegmentUsage{
            {sizeof(storage::SegmentUsage)}, physical_, file_created_ ? 1u : 0u, 0};
    }

    Result<storage::SegmentSealResult> seal_segment(Deadline) {
        if (file_ < 0 || poisoned_ || stopped_ || mode_ == "fail_seal") {
            return Status(StatusCode::InvalidState, {});
        }
        buffer_.clear();
        append(3, 1);
        append(records_, 8);
        auto footer = write_buffer();
        if (!footer.ok()) {
            return footer.status();
        }
        const auto closed = close(std::exchange(file_, -1));
        if (closed != 0) {
            poisoned_ = true;
            return Status(StatusCode::Unavailable, {});
        }
        if (mode_ == "late_seal") {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }
        return storage::SegmentSealResult{{{"records.fake", physical_}}, physical_};
    }

    Result<void> quiesce(Deadline) {
        stopped_ = true;
        if (mode_ == "fail_quiesce") {
            return Status(StatusCode::Unavailable, {});
        }
        if (file_ >= 0 && close(std::exchange(file_, -1)) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        return {};
    }

    bool mode(std::string_view value) const noexcept { return mode_ == value; }

private:
    struct DescriptorEntry {
        std::uint64_t id{0};
        std::uint64_t stream{0};
        std::vector<std::uint8_t> encoded;
        bool emitted{false};
    };

    void append(std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) {
            buffer_.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
        }
    }

    void blob(const std::uint8_t* data, std::size_t size) {
        append(size, 4);
        if (size != 0) {
            buffer_.insert(buffer_.end(), data, data + size);
        }
    }

    void text(rodaq_string_t value) {
        blob(reinterpret_cast<const std::uint8_t*>(value.data), value.size);
    }

    void frame_size() noexcept {
        const auto length = buffer_.size() - 5;
        for (unsigned i = 0; i < 4; ++i) {
            buffer_[i + 1] = static_cast<std::uint8_t>(length >> (8 * i));
        }
    }

    Result<void> write_buffer() {
        if (buffer_.size() > limits_.max_segment_physical_bytes - physical_) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        std::size_t offset = 0;
        while (offset != buffer_.size()) {
            const auto count = write(file_, buffer_.data() + offset, buffer_.size() - offset);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                poisoned_ = true;
                return Status(StatusCode::Unavailable, {});
            }
            offset += static_cast<std::size_t>(count);
            physical_ += static_cast<std::uint64_t>(count);
        }
        return {};
    }

    Result<void> write_record(const storage::Record& record) {
        if (record.payload.size > limits_.max_record_bytes ||
            records_ >= limits_.max_segment_records ||
            record.payload.size > limits_.max_segment_input_bytes - input_bytes_) {
            return Status(StatusCode::InvalidArgument, {});
        }
        auto descriptor = std::find_if(catalog_.begin(), catalog_.end(), [&](const auto& entry) {
            return entry.id == record.descriptor_id && entry.stream == record.stream_id;
        });
        if (descriptor == catalog_.end()) {
            return Status(StatusCode::NotFound, {});
        }
        if (!descriptor->emitted) {
            if (segment_descriptors_ >= limits_.max_descriptors_per_segment) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            buffer_.assign(descriptor->encoded.begin(), descriptor->encoded.end());
            auto emitted = write_buffer();
            if (!emitted.ok()) {
                return emitted.status();
            }
            descriptor->emitted = true;
            ++segment_descriptors_;
        }
        buffer_.clear();
        append(2, 1);
        append(0, 4);
        append(record.stream_id, 8);
        append(record.descriptor_id, 8);
        append(record.recording_time, 8);
        append(record.publish_time, 8);
        append(record.has_publish_time, 1);
        blob(record.payload.data, static_cast<std::size_t>(record.payload.size));
        frame_size();
        auto result = write_buffer();
        if (result.ok()) {
            ++records_;
            input_bytes_ += record.payload.size;
        }
        return result;
    }

    std::string mode_{"normal"};
    std::uint32_t prefix_{1};
    std::uint32_t delay_ms_{100};
    std::uint64_t descriptor_bytes_{65536};
    std::uint32_t catalog_slots_{64};
    storage::PrepareConfig limits_{};
    std::vector<DescriptorEntry> catalog_;
    std::vector<std::uint8_t> buffer_;
    std::uint64_t physical_{0};
    std::uint64_t records_{0};
    std::uint64_t input_bytes_{0};
    std::uint32_t segment_descriptors_{0};
    int file_{-1};
    bool file_created_{false};
    bool prepared_{false};
    bool stopped_{false};
    bool poisoned_{false};
};

template <typename Implementation>
struct FaultProvider : storage::Provider<Implementation> {
    static rodaq_status_code_t write_batch(
        rodaq_interface_context_t* context, const storage::Record* records, std::uint32_t count,
        rodaq_deadline_t deadline, storage::BatchResult* result, rodaq_error_t* error) noexcept {
        const auto code = storage::Provider<Implementation>::write_batch(
            context, records, count, deadline, result, error);
        if (code == RODAQ_STATUS_OK &&
            reinterpret_cast<Implementation*>(context)->mode("invalid_output")) {
            result->confirmed_prefix = count + 1;
        }
        return code;
    }

    static const rodaq_storage_interface_vtable_t* vtable() noexcept {
        static const auto table = [] {
            auto value = *storage::Provider<Implementation>::vtable();
            value.write_batch = write_batch;
            return value;
        }();
        return &table;
    }
};

struct FakeInterface : storage::Interface {
    template <typename Implementation>
    using Provider = FaultProvider<Implementation>;
};

class FakeStoragePlugin final : public rodaq::plugin::PluginBase {
public:
    Result<void> configure(rodaq::plugin::PluginSetup& setup) override {
        auto configured = backend_.configure(setup.config());
        if (!configured.ok()) {
            return configured.status();
        }
        return setup.provide<FakeInterface>(backend_);
    }

    Result<void> shutdown(Deadline deadline) override {
        if (backend_.mode("fail_shutdown")) {
            return Status(StatusCode::Unavailable, {});
        }
        return backend_.quiesce(deadline);
    }

private:
    FakeStorage backend_;
};

}  // namespace
}  // namespace rodaq::fake_storage

RODAQ_DEFINE_PLUGIN(rodaq::fake_storage::FakeStoragePlugin)
