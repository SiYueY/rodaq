#include <rodaq/plugins/mcap_storage/interface.hpp>

#include <rodaq/plugin/export.hpp>
#include <rodaq/plugin/plugin.hpp>

#include <mcap/writer.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rodaq::plugins::mcap_storage {
namespace {

using Json = nlohmann::json;

struct WriterConfig {
    std::uint64_t chunk_size = mcap::DefaultChunkSize;
    mcap::Compression compression = mcap::Compression::None;
    mcap::CompressionLevel compression_level = mcap::CompressionLevel::Default;
    bool crc = true;
    bool message_index = true;
    bool summary = true;
};

std::string_view abi_string(rodaq_string_t value) noexcept {
    return value.data == nullptr ? std::string_view{} : std::string_view{value.data, value.size};
}

rodaq::Result<Json> parse_strict_json(std::string_view text) {
    std::vector<std::unordered_set<std::string>> object_keys;
    try {
        auto value = Json::parse(
            text.begin(), text.end(),
            [&object_keys](int depth, Json::parse_event_t event, Json& parsed) {
                const auto index = static_cast<std::size_t>(depth);
                if (event == Json::parse_event_t::object_start) {
                    if (object_keys.size() <= index) {
                        object_keys.resize(index + 1u);
                    }
                    object_keys[index].clear();
                } else if (event == Json::parse_event_t::key) {
                    if (object_keys.size() <= index) {
                        object_keys.resize(index + 1u);
                    }
                    const auto key = parsed.get<std::string>();
                    if (!object_keys[index].insert(key).second) {
                        throw std::runtime_error{"duplicate JSON key"};
                    }
                }
                return true;
            },
            true, false);
        return value;
    } catch (const std::exception&) {
        return rodaq::Status{
            rodaq::StatusCode::InvalidArgument, "MCAP reference config is not strict JSON"};
    }
}

rodaq::Result<WriterConfig> parse_config(std::string_view text) {
    auto parsed = parse_strict_json(text);
    if (!parsed.ok()) {
        return parsed.status();
    }
    const auto& root = parsed.value();
    if (!root.is_object()) {
        return rodaq::Status{
            rodaq::StatusCode::InvalidArgument, "MCAP reference config must be an object"};
    }
    for (const auto& [key, value] : root.items()) {
        (void)value;
        if (key != "chunk_size" && key != "compression" && key != "compression_level" &&
            key != "crc" && key != "message_index" && key != "summary") {
            return rodaq::Status{
                rodaq::StatusCode::InvalidArgument, "unknown MCAP reference config field"};
        }
    }

    WriterConfig config;
    if (root.contains("chunk_size")) {
        if (!root.at("chunk_size").is_number_unsigned()) {
            return rodaq::Status{
                rodaq::StatusCode::InvalidArgument, "MCAP chunk_size must be unsigned"};
        }
        config.chunk_size = root.at("chunk_size").get<std::uint64_t>();
        if (config.chunk_size == 0u) {
            return rodaq::Status{
                rodaq::StatusCode::InvalidArgument, "MCAP chunk_size must be positive"};
        }
    }
    if (root.contains("compression")) {
        if (!root.at("compression").is_string()) {
            return rodaq::Status{
                rodaq::StatusCode::InvalidArgument, "MCAP compression must be a string"};
        }
        const auto value = root.at("compression").get<std::string>();
        if (value == "none") {
            config.compression = mcap::Compression::None;
        } else if (value == "lz4") {
            config.compression = mcap::Compression::Lz4;
        } else if (value == "zstd") {
            config.compression = mcap::Compression::Zstd;
        } else {
            return rodaq::Status{rodaq::StatusCode::InvalidArgument, "invalid MCAP compression"};
        }
    }
    if (root.contains("compression_level")) {
        if (!root.at("compression_level").is_string()) {
            return rodaq::Status{
                rodaq::StatusCode::InvalidArgument, "MCAP compression_level must be a string"};
        }
        const auto value = root.at("compression_level").get<std::string>();
        if (value == "fastest") {
            config.compression_level = mcap::CompressionLevel::Fastest;
        } else if (value == "fast") {
            config.compression_level = mcap::CompressionLevel::Fast;
        } else if (value == "default") {
            config.compression_level = mcap::CompressionLevel::Default;
        } else if (value == "slow") {
            config.compression_level = mcap::CompressionLevel::Slow;
        } else if (value == "slowest") {
            config.compression_level = mcap::CompressionLevel::Slowest;
        } else {
            return rodaq::Status{
                rodaq::StatusCode::InvalidArgument, "invalid MCAP compression_level"};
        }
    }
    for (const auto& [key, output] :
         {std::pair{"crc", &config.crc}, std::pair{"message_index", &config.message_index},
          std::pair{"summary", &config.summary}}) {
        if (!root.contains(key)) {
            continue;
        }
        if (!root.at(key).is_boolean()) {
            return rodaq::Status{
                rodaq::StatusCode::InvalidArgument, "MCAP boolean option has invalid type"};
        }
        *output = root.at(key).get<bool>();
    }
    return config;
}

class McapPlugin final : public rodaq::plugin::PluginBase {
public:
    [[nodiscard]] rodaq::Result<void> configure(rodaq::plugin::PluginSetup& setup) override {
        auto parsed = parse_config(setup.config());
        if (!parsed.ok()) {
            return parsed.status();
        }
        config_ = std::move(parsed).value();
        return setup.provide<StorageInterface>(*this);
    }

    [[nodiscard]] rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override {
        std::lock_guard<std::mutex> lock{mutex_};
        if (open_) {
            writer_.terminate();
            open_ = false;
        }
        return {};
    }

    rodaq_status_code_t open_impl(rodaq_string_t path) {
        std::lock_guard<std::mutex> lock{mutex_};
        if (open_) {
            return RODAQ_STATUS_INVALID_STATE;
        }
        if (path.data == nullptr || !valid_utf8(path)) {
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
        mcap::McapWriterOptions options{"ros2"};
        options.chunkSize = config_.chunk_size;
        options.compression = config_.compression;
        options.compressionLevel = config_.compression_level;
        options.noChunkCRC = !config_.crc;
        options.noSummaryCRC = !config_.crc;
        options.noMessageIndex = !config_.message_index;
        options.noSummary = !config_.summary;
        options.noChunking = false;

        const auto status = writer_.open(abi_string(path), options);
        if (status.code != mcap::StatusCode::Success) {
            return RODAQ_STATUS_UNAVAILABLE;
        }
        open_ = true;
        channels_.clear();
        registered_channels_.clear();
        schema_ids_.clear();
        sequence_ids_.clear();
        return RODAQ_STATUS_OK;
    }

    rodaq_status_code_t register_channel_impl(
        rodaq_string_t topic, rodaq_string_t message_type, rodaq_string_t schema_encoding,
        rodaq_bytes_t schema, rodaq_string_t message_encoding, std::uint64_t* channel_id) {
        std::lock_guard<std::mutex> lock{mutex_};
        if (!open_ || channel_id == nullptr || topic.data == nullptr ||
            message_type.data == nullptr || schema_encoding.data == nullptr ||
            message_encoding.data == nullptr || (schema.size != 0u && schema.data == nullptr)) {
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }

        const auto topic_text = abi_string(topic);
        const auto type_text = abi_string(message_type);
        const auto schema_encoding_text = abi_string(schema_encoding);
        const auto schema_text =
            schema.data == nullptr
                ? std::string_view{}
                : std::string_view{reinterpret_cast<const char*>(schema.data), schema.size};
        const auto key = std::string{topic_text} + '\n' + std::string{type_text} + '\n' +
                         std::string{schema_encoding_text} + '\n' + std::string{schema_text} +
                         '\n' + std::string{abi_string(message_encoding)};
        const auto existing = channels_.find(key);
        if (existing != channels_.end()) {
            *channel_id = existing->second;
            return RODAQ_STATUS_OK;
        }

        mcap::SchemaId assigned_schema_id = 0u;
        const auto schema_id = schema_ids_.find(key);
        if (schema_id == schema_ids_.end()) {
            mcap::Schema mcap_schema{type_text, schema_encoding_text, schema_text};
            writer_.addSchema(mcap_schema);
            assigned_schema_id = mcap_schema.id;
            schema_ids_.emplace(key, assigned_schema_id);
        } else {
            assigned_schema_id = schema_id->second;
        }

        mcap::Channel channel{topic_text, abi_string(message_encoding), assigned_schema_id};
        writer_.addChannel(channel);
        channels_.emplace(key, channel.id);
        registered_channels_.insert(channel.id);
        *channel_id = channel.id;
        return RODAQ_STATUS_OK;
    }

    rodaq_status_code_t write_impl(
        std::uint64_t channel_id, std::int64_t timestamp, rodaq_bytes_t payload) {
        std::lock_guard<std::mutex> lock{mutex_};
        if (!open_ || timestamp < 0 || (payload.size != 0u && payload.data == nullptr)) {
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
        if (channel_id > std::numeric_limits<mcap::ChannelId>::max()) {
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
        if (registered_channels_.find(channel_id) == registered_channels_.end()) {
            return RODAQ_STATUS_NOT_FOUND;
        }

        mcap::Message message;
        message.channelId = static_cast<mcap::ChannelId>(channel_id);
        message.sequence = sequence_ids_[channel_id]++;
        message.logTime = static_cast<mcap::Timestamp>(timestamp);
        message.publishTime = static_cast<mcap::Timestamp>(timestamp);
        message.dataSize = payload.size;
        message.data = reinterpret_cast<const std::byte*>(payload.data);
        const auto status = writer_.write(message);
        return status.code == mcap::StatusCode::Success ? RODAQ_STATUS_OK : RODAQ_STATUS_DATA_LOSS;
    }

    rodaq_status_code_t close_impl() {
        std::lock_guard<std::mutex> lock{mutex_};
        if (!open_) {
            return RODAQ_STATUS_INVALID_STATE;
        }
        writer_.close();
        open_ = false;
        return RODAQ_STATUS_OK;
    }

private:
    static bool valid_utf8(rodaq_string_t value) noexcept {
        return value.size == 0u ||
               (value.data != nullptr && rodaq::plugin::interface_support::valid_utf8(
                                             std::string_view{value.data, value.size}));
    }

    std::mutex mutex_;
    mcap::McapWriter writer_;
    WriterConfig config_;
    bool open_ = false;
    std::unordered_map<std::string, std::uint64_t> schema_ids_;
    std::unordered_map<std::string, std::uint64_t> channels_;
    std::unordered_set<std::uint64_t> registered_channels_;
    std::unordered_map<std::uint64_t, std::uint32_t> sequence_ids_;
};

}  // namespace
}  // namespace rodaq::plugins::mcap_storage

RODAQ_DEFINE_PLUGIN(rodaq::plugins::mcap_storage::McapPlugin)
