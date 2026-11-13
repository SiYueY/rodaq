#include <rodaq/plugins/ros2_acquisition/interface.hpp>

#include <rodaq/plugin/export.hpp>
#include <rodaq/plugin/plugin.hpp>

#include <nlohmann/json.hpp>

#include <rclcpp/generic_subscription.hpp>
#include <rclcpp/node.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/serialized_message.hpp>
#include <rosbag2_transport/qos.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rodaq::plugins::ros2_acquisition {
namespace {

using Json = nlohmann::json;

struct QosOverride {
    bool has_reliability = false;
    bool has_durability = false;
    bool has_history = false;
    bool has_depth = false;
    rclcpp::ReliabilityPolicy reliability = rclcpp::ReliabilityPolicy::SystemDefault;
    rclcpp::DurabilityPolicy durability = rclcpp::DurabilityPolicy::SystemDefault;
    rclcpp::HistoryPolicy history = rclcpp::HistoryPolicy::SystemDefault;
    std::size_t depth = 0u;
};

struct ReferenceConfig {
    std::string topic;
    std::string message_type;
    std::string schema_encoding = "ros2msg";
    std::string schema;
    QosOverride qos;
};

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
            rodaq::StatusCode::InvalidArgument, "ROS reference config is not strict JSON"};
    }
}

rodaq::Result<void> reject_unknown(
    const Json& object, std::initializer_list<std::string_view> allowed) {
    for (const auto& [key, value] : object.items()) {
        (void)value;
        bool found = false;
        for (const auto candidate : allowed) {
            if (key == candidate) {
                found = true;
                break;
            }
        }
        if (!found) {
            return rodaq::Status{
                rodaq::StatusCode::InvalidArgument, "unknown ROS reference config field"};
        }
    }
    return {};
}

rodaq::Result<std::string> required_string(const Json& object, std::string_view key) {
    const auto iterator = object.find(std::string{key});
    if (iterator == object.end() || !iterator->is_string()) {
        return rodaq::Status{
            rodaq::StatusCode::InvalidArgument, "ROS reference config string is required"};
    }
    return iterator->get<std::string>();
}

rodaq::Result<std::string> optional_string(
    const Json& object, std::string_view key, std::string fallback) {
    const auto iterator = object.find(std::string{key});
    if (iterator == object.end()) {
        return fallback;
    }
    if (!iterator->is_string()) {
        return rodaq::Status{
            rodaq::StatusCode::InvalidArgument, "ROS reference config string has invalid type"};
    }
    return iterator->get<std::string>();
}

rodaq::Result<QosOverride> parse_qos(const Json& object) {
    auto unknown = reject_unknown(object, {"reliability", "durability", "history", "depth"});
    if (!unknown.ok()) {
        return unknown.status();
    }
    QosOverride result;
    if (object.contains("reliability")) {
        if (!object.at("reliability").is_string()) {
            return rodaq::Status{rodaq::StatusCode::InvalidArgument, "invalid ROS QoS reliability"};
        }
        const auto value = object.at("reliability").get<std::string>();
        result.has_reliability = true;
        if (value == "reliable") {
            result.reliability = rclcpp::ReliabilityPolicy::Reliable;
        } else if (value == "best_effort") {
            result.reliability = rclcpp::ReliabilityPolicy::BestEffort;
        } else if (value == "system_default") {
            result.reliability = rclcpp::ReliabilityPolicy::SystemDefault;
        } else {
            return rodaq::Status{rodaq::StatusCode::InvalidArgument, "invalid ROS QoS reliability"};
        }
    }
    if (object.contains("durability")) {
        if (!object.at("durability").is_string()) {
            return rodaq::Status{rodaq::StatusCode::InvalidArgument, "invalid ROS QoS durability"};
        }
        const auto value = object.at("durability").get<std::string>();
        result.has_durability = true;
        if (value == "volatile") {
            result.durability = rclcpp::DurabilityPolicy::Volatile;
        } else if (value == "transient_local") {
            result.durability = rclcpp::DurabilityPolicy::TransientLocal;
        } else if (value == "system_default") {
            result.durability = rclcpp::DurabilityPolicy::SystemDefault;
        } else {
            return rodaq::Status{rodaq::StatusCode::InvalidArgument, "invalid ROS QoS durability"};
        }
    }
    if (object.contains("history")) {
        if (!object.at("history").is_string()) {
            return rodaq::Status{rodaq::StatusCode::InvalidArgument, "invalid ROS QoS history"};
        }
        const auto value = object.at("history").get<std::string>();
        result.has_history = true;
        if (value == "keep_last") {
            result.history = rclcpp::HistoryPolicy::KeepLast;
        } else if (value == "keep_all") {
            result.history = rclcpp::HistoryPolicy::KeepAll;
        } else if (value == "system_default") {
            result.history = rclcpp::HistoryPolicy::SystemDefault;
        } else {
            return rodaq::Status{rodaq::StatusCode::InvalidArgument, "invalid ROS QoS history"};
        }
    }
    if (object.contains("depth")) {
        if (!object.at("depth").is_number_unsigned()) {
            return rodaq::Status{rodaq::StatusCode::InvalidArgument, "invalid ROS QoS depth"};
        }
        result.has_depth = true;
        result.depth = object.at("depth").get<std::size_t>();
        if (result.depth == 0u) {
            return rodaq::Status{
                rodaq::StatusCode::InvalidArgument, "ROS QoS depth must be positive"};
        }
    }
    return result;
}

rodaq::Result<ReferenceConfig> parse_config(std::string_view text) {
    auto parsed = parse_strict_json(text);
    if (!parsed.ok()) {
        return parsed.status();
    }
    const auto& root = parsed.value();
    if (!root.is_object()) {
        return rodaq::Status{
            rodaq::StatusCode::InvalidArgument, "ROS reference config must be an object"};
    }
    auto unknown = reject_unknown(root, {"topic", "type", "definition", "schema_encoding", "qos"});
    if (!unknown.ok()) {
        return unknown.status();
    }
    ReferenceConfig config;
    auto topic = required_string(root, "topic");
    auto type = required_string(root, "type");
    if (!topic.ok()) {
        return topic.status();
    }
    if (!type.ok()) {
        return type.status();
    }
    config.topic = std::move(topic).value();
    config.message_type = std::move(type).value();
    auto encoding = optional_string(root, "schema_encoding", config.schema_encoding);
    auto schema = optional_string(root, "definition", {});
    if (!encoding.ok()) {
        return encoding.status();
    }
    if (!schema.ok()) {
        return schema.status();
    }
    config.schema_encoding = std::move(encoding).value();
    config.schema = std::move(schema).value();
    if (config.topic.empty() || config.message_type.empty() || config.schema_encoding.empty()) {
        return rodaq::Status{
            rodaq::StatusCode::InvalidArgument,
            "ROS reference config requires topic, type and schema encoding"};
    }
    if (root.contains("qos")) {
        if (!root.at("qos").is_object()) {
            return rodaq::Status{
                rodaq::StatusCode::InvalidArgument, "ROS reference qos must be an object"};
        }
        auto qos = parse_qos(root.at("qos"));
        if (!qos.ok()) {
            return qos.status();
        }
        config.qos = std::move(qos).value();
    }
    return config;
}

class Ros2Plugin final : public rodaq::plugin::PluginBase {
public:
    [[nodiscard]] rodaq::Result<void> configure(rodaq::plugin::PluginSetup& setup) override {
        auto parsed = parse_config(setup.config());
        if (!parsed.ok()) {
            return parsed.status();
        }
        config_ = std::move(parsed).value();
        auto sink = setup.service<SinkInterface>();
        if (!sink.ok()) {
            return sink.status();
        }
        sink_ = std::move(sink).value();
        try {
            context_ = std::make_shared<rclcpp::Context>();
            context_->init(0, nullptr);
            rclcpp::NodeOptions options;
            options.context(context_);
            node_ = std::make_shared<rclcpp::Node>("rodaq_reference_ros2_plugin", options);
        } catch (...) {
            context_.reset();
            node_.reset();
            return rodaq::Status{
                rodaq::StatusCode::Unavailable, "ROS reference context initialization failed"};
        }
        return setup.provide<ControlInterface>(*this);
    }

    [[nodiscard]] rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override {
        stop();
        node_.reset();
        if (context_ != nullptr) {
            if (context_->is_valid()) {
                context_->shutdown("RoDAQ ROS reference plugin shutdown");
            }
            context_.reset();
        }
        return {};
    }

    rodaq_status_code_t start() {
        std::lock_guard<std::mutex> lock{mutex_};
        if (subscription_ != nullptr || node_ == nullptr) {
            return RODAQ_STATUS_INVALID_STATE;
        }
        auto qos = rosbag2_transport::Rosbag2QoS::adapt_request_to_offers(
            config_.topic, node_->get_publishers_info_by_topic(config_.topic));
        if (config_.qos.has_reliability) {
            qos.reliability(config_.qos.reliability);
        }
        if (config_.qos.has_durability) {
            qos.durability(config_.qos.durability);
        }
        if (config_.qos.has_history) {
            qos.history(config_.qos.history);
        }
        if (config_.qos.has_depth) {
            qos.keep_last(config_.qos.depth);
        }
        subscription_ = node_->create_generic_subscription(
            config_.topic, config_.message_type, qos,
            [this](std::shared_ptr<rclcpp::SerializedMessage> message) {
                on_message(std::move(message));
            });
        auto executor = std::make_shared<rclcpp::executors::SingleThreadedExecutor>();
        executor->add_node(node_);
        executor_ = executor;
        executor_thread_ = std::thread([executor] { executor->spin(); });
        return RODAQ_STATUS_OK;
    }

    void stop() {
        std::shared_ptr<rclcpp::GenericSubscription> subscription;
        std::shared_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            subscription = std::move(subscription_);
            executor = std::move(executor_);
            thread = std::move(executor_thread_);
        }
        subscription.reset();
        if (executor != nullptr) {
            executor->cancel();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

private:
    void on_message(std::shared_ptr<rclcpp::SerializedMessage> message) noexcept {
        try {
            const auto& serialized = message->get_rcl_serialized_message();
            rodaq_reference_ros2_sample_t sample{};
            sample.header.struct_size = sizeof(sample);
            sample.topic = rodaq_string_t{
                config_.topic.data(), static_cast<std::uint32_t>(config_.topic.size())};
            sample.message_type = rodaq_string_t{
                config_.message_type.data(),
                static_cast<std::uint32_t>(config_.message_type.size())};
            sample.message_encoding = rodaq_string_t{"cdr", 3u};
            sample.schema_encoding = rodaq_string_t{
                config_.schema_encoding.data(),
                static_cast<std::uint32_t>(config_.schema_encoding.size())};
            sample.schema = rodaq_bytes_t{
                reinterpret_cast<const std::uint8_t*>(config_.schema.data()),
                static_cast<std::uint32_t>(config_.schema.size())};
            sample.timestamp = rclcpp::Clock{RCL_SYSTEM_TIME}.now().nanoseconds();
            sample.payload = rodaq_bytes_t{
                serialized.buffer, static_cast<std::uint32_t>(serialized.buffer_length)};
            (void)sink_.invoke([&](rodaq_interface_context_t* context,
                                   const SinkInterface::Vtable* vtable, rodaq_error_t* error) {
                return vtable->record(
                    reinterpret_cast<rodaq_reference_ros2_sink_context_t*>(context), &sample,
                    error);
            });
        } catch (...) {
        }
    }

    ReferenceConfig config_;
    SinkInterface::Reference sink_;
    rclcpp::Context::SharedPtr context_;
    std::shared_ptr<rclcpp::Node> node_;
    std::shared_ptr<rclcpp::GenericSubscription> subscription_;
    std::shared_ptr<rclcpp::executors::SingleThreadedExecutor> executor_;
    std::thread executor_thread_;
    std::mutex mutex_;
};

}  // namespace
}  // namespace rodaq::plugins::ros2_acquisition

RODAQ_DEFINE_PLUGIN(rodaq::plugins::ros2_acquisition::Ros2Plugin)
