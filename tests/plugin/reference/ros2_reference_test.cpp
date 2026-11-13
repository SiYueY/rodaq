#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>
#include <rodaq/plugins/ros2_acquisition/interface.hpp>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/int32.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

class RosSink final {
public:
    rodaq_status_code_t record(const rodaq_reference_ros2_sample_t& sample) noexcept {
        last_payload.assign(sample.payload.data, sample.payload.data + sample.payload.size);
        last_topic.assign(sample.topic.data, sample.topic.size);
        last_timestamp.store(sample.timestamp, std::memory_order_relaxed);
        ++count;
        return RODAQ_STATUS_OK;
    }

    std::atomic<int> count{0};
    std::string last_topic;
    std::vector<std::uint8_t> last_payload;
    std::atomic<std::int64_t> last_timestamp{0};
};

}  // namespace

int main() {
    using namespace std::chrono_literals;
    using rodaq::plugin::Deadline;
    using rodaq::plugin::PluginManager;
    using rodaq::plugin::PluginManagerOptions;
    using rodaq::plugin::ServiceRegistry;
    namespace ros_api = rodaq::plugins::ros2_acquisition;

    rclcpp::init(0, nullptr);
    auto sink = std::make_shared<RosSink>();
    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.add_shared<ros_api::SinkInterface>(sink).ok());
    RODAQ_TEST_CHECK(services.freeze().ok());

    PluginManagerOptions options;
    options.search_paths.push_back(std::filesystem::path{RODAQ_TEST_PLUGIN_ROOT});
    options.shutdown_timeout = 2s;
    auto manager_result = PluginManager::create(options);
    RODAQ_TEST_CHECK(manager_result.ok());
    auto manager = std::move(manager_result).value();
    RODAQ_TEST_CHECK(manager.refresh().ok());
    auto library_result = manager.load("com.rodaq.reference.ros2", "1.0.0");
    RODAQ_TEST_CHECK(library_result.ok());
    auto library = std::move(library_result).value();

    auto invalid_config = library.create_instance(
        services,
        R"({"topic":"/a","topic":"/b","type":"std_msgs/msg/Int32","definition":"int32 data"})");
    RODAQ_TEST_CHECK(!invalid_config.ok());
    RODAQ_TEST_CHECK(invalid_config.status().code() == rodaq::StatusCode::InvalidArgument);
    const std::string config =
        R"({"topic":"/rodaq_reference","type":"std_msgs/msg/Int32","schema_encoding":"ros2msg","definition":"int32 data\n"})";
    auto instance_result = library.create_instance(services, config);
    RODAQ_TEST_CHECK(instance_result.ok());
    auto instance = std::move(instance_result).value();
    auto operation_result = instance.query<ros_api::ControlInterface>();
    RODAQ_TEST_CHECK(operation_result.ok());
    auto operation = std::move(operation_result).value();

    auto node = std::make_shared<rclcpp::Node>("rodaq_reference_test_publisher");
    auto publisher =
        node->create_publisher<std_msgs::msg::Int32>("/rodaq_reference", rclcpp::SensorDataQoS());
    RODAQ_TEST_CHECK(ros_api::start(operation).ok());

    std_msgs::msg::Int32 message;
    message.data = 42;
    for (int attempt = 0; attempt < 100 && sink->count.load() == 0; ++attempt) {
        publisher->publish(message);
        std::this_thread::sleep_for(20ms);
    }
    RODAQ_TEST_CHECK(sink->count.load() > 0);
    RODAQ_TEST_CHECK(sink->last_topic == "/rodaq_reference");
    RODAQ_TEST_CHECK(!sink->last_payload.empty());
    RODAQ_TEST_CHECK(sink->last_timestamp.load(std::memory_order_relaxed) > 1577836800000000000LL);

    RODAQ_TEST_CHECK(ros_api::stop(operation).ok());
    RODAQ_TEST_CHECK(instance.close(Deadline::after(2s)).ok());
    RODAQ_TEST_CHECK(manager.shutdown(Deadline::after(2s)).ok());
    RODAQ_TEST_CHECK(rclcpp::ok());
    publisher.reset();
    node.reset();
    if (rclcpp::ok()) {
        rclcpp::shutdown();
    }
    return 0;
}
