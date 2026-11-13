#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/service_registry.hpp>
#include <rodaq/plugins/mcap_storage/interface.hpp>
#include <rodaq/plugins/ros2_acquisition/interface.hpp>

#include <mcap/reader.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/int32.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace mcap_api = rodaq::plugins::mcap_storage;

struct OwnedSample {
    std::string topic;
    std::string message_type;
    std::string schema_encoding;
    std::string schema;
    std::string message_encoding;
    std::int64_t timestamp = 0;
    std::vector<std::uint8_t> payload;
};

class BoundedWriter final {
public:
    explicit BoundedWriter(mcap_api::StorageInterface::Handle* storage)
    : storage_(storage), worker_([this] { run(); }) {}

    BoundedWriter(const BoundedWriter&) = delete;
    BoundedWriter& operator=(const BoundedWriter&) = delete;

    ~BoundedWriter() { stop(); }

    rodaq_status_code_t enqueue(const rodaq_reference_ros2_sample_t& sample) noexcept {
        try {
            received.fetch_add(1u, std::memory_order_relaxed);
            last_timestamp.store(sample.timestamp, std::memory_order_relaxed);
            OwnedSample owned{
                std::string{sample.topic.data, sample.topic.size},
                std::string{sample.message_type.data, sample.message_type.size},
                std::string{sample.schema_encoding.data, sample.schema_encoding.size},
                std::string{reinterpret_cast<const char*>(sample.schema.data), sample.schema.size},
                std::string{sample.message_encoding.data, sample.message_encoding.size},
                sample.timestamp,
                std::vector<std::uint8_t>{
                    sample.payload.data, sample.payload.data + sample.payload.size}};
            std::lock_guard<std::mutex> lock{mutex_};
            if (queue_.size() >= kCapacity) {
                dropped.fetch_add(1u, std::memory_order_relaxed);
                return RODAQ_STATUS_RESOURCE_EXHAUSTED;
            }
            queue_.push_back(std::move(owned));
            condition_.notify_one();
            return RODAQ_STATUS_OK;
        } catch (...) {
            return RODAQ_STATUS_INTERNAL;
        }
    }

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            stopping_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::atomic<std::size_t> received{0u};
    std::atomic<std::size_t> written{0u};
    std::atomic<std::size_t> dropped{0u};
    std::atomic<std::int64_t> last_timestamp{0};

private:
    void run() noexcept {
        while (true) {
            OwnedSample sample;
            {
                std::unique_lock<std::mutex> lock{mutex_};
                condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) {
                    return;
                }
                sample = std::move(queue_.front());
                queue_.pop_front();
            }
            if (!channel_ready_) {
                auto channel = mcap_api::register_channel(
                    *storage_, sample.topic, sample.message_type, sample.schema_encoding,
                    sample.schema, sample.message_encoding);
                if (!channel.ok()) {
                    continue;
                }
                channel_id_ = channel.value();
                channel_ready_ = true;
            }
            auto result = mcap_api::write(
                *storage_, channel_id_, sample.timestamp,
                rodaq_bytes_t{
                    sample.payload.data(), static_cast<std::uint32_t>(sample.payload.size())});
            if (result.ok()) {
                written.fetch_add(1u, std::memory_order_relaxed);
            }
        }
    }

    static constexpr std::size_t kCapacity = 64u;
    mcap_api::StorageInterface::Handle* storage_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<OwnedSample> queue_;
    bool stopping_ = false;
    bool channel_ready_ = false;
    std::uint64_t channel_id_ = 0u;
    std::thread worker_;
};

class ForwardingSink final {
public:
    explicit ForwardingSink(std::shared_ptr<BoundedWriter> writer) : writer_(std::move(writer)) {}

    rodaq_status_code_t record(const rodaq_reference_ros2_sample_t& sample) noexcept {
        return writer_->enqueue(sample);
    }

private:
    std::shared_ptr<BoundedWriter> writer_;
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
    const auto output = std::filesystem::temp_directory_path() / "rodaq-reference-integration.mcap";
    std::error_code error;
    std::filesystem::remove(output, error);

    PluginManagerOptions options;
    options.search_paths.push_back(std::filesystem::path{RODAQ_TEST_PLUGIN_ROOT});
    options.shutdown_timeout = 2s;
    auto manager_result = PluginManager::create(options);
    RODAQ_TEST_CHECK(manager_result.ok());
    auto manager = std::move(manager_result).value();
    RODAQ_TEST_CHECK(manager.refresh().ok());

    auto mcap_library_result = manager.load("com.rodaq.reference.mcap", "1.0.0");
    RODAQ_TEST_CHECK(mcap_library_result.ok());
    auto mcap_library = std::move(mcap_library_result).value();
    ServiceRegistry empty_services;
    RODAQ_TEST_CHECK(empty_services.freeze().ok());
    auto mcap_instance_result = mcap_library.create_instance(empty_services, "{}");
    RODAQ_TEST_CHECK(mcap_instance_result.ok());
    auto mcap_instance = std::move(mcap_instance_result).value();
    auto mcap_operation_result = mcap_instance.query<mcap_api::StorageInterface>();
    RODAQ_TEST_CHECK(mcap_operation_result.ok());
    auto mcap_operation = std::move(mcap_operation_result).value();
    RODAQ_TEST_CHECK(mcap_api::open(mcap_operation, output.string()).ok());

    auto bounded_writer = std::make_shared<BoundedWriter>(&mcap_operation);
    auto sink = std::make_shared<ForwardingSink>(bounded_writer);
    ServiceRegistry services;
    RODAQ_TEST_CHECK(services.add_shared<ros_api::SinkInterface>(sink).ok());
    RODAQ_TEST_CHECK(services.freeze().ok());

    auto ros_library_result = manager.load("com.rodaq.reference.ros2", "1.0.0");
    RODAQ_TEST_CHECK(ros_library_result.ok());
    auto ros_library = std::move(ros_library_result).value();
    const std::string config =
        R"({"topic":"/rodaq_integration","type":"std_msgs/msg/Int32","schema_encoding":"ros2msg","definition":"int32 data\n"})";
    auto ros_instance_result = ros_library.create_instance(services, config);
    RODAQ_TEST_CHECK(ros_instance_result.ok());
    auto ros_instance = std::move(ros_instance_result).value();
    auto control_result = ros_instance.query<ros_api::ControlInterface>();
    RODAQ_TEST_CHECK(control_result.ok());
    auto control = std::move(control_result).value();

    auto node = std::make_shared<rclcpp::Node>("rodaq_reference_integration_publisher");
    auto publisher =
        node->create_publisher<std_msgs::msg::Int32>("/rodaq_integration", rclcpp::QoS{10});
    RODAQ_TEST_CHECK(ros_api::start(control).ok());

    std_msgs::msg::Int32 message;
    message.data = 77;
    for (int attempt = 0;
         attempt < 100 && bounded_writer->written.load(std::memory_order_acquire) == 0u;
         ++attempt) {
        publisher->publish(message);
        std::this_thread::sleep_for(20ms);
    }
    RODAQ_TEST_CHECK(bounded_writer->written.load(std::memory_order_acquire) > 0u);
    RODAQ_TEST_CHECK(
        bounded_writer->last_timestamp.load(std::memory_order_relaxed) > 1577836800000000000LL);

    RODAQ_TEST_CHECK(ros_api::stop(control).ok());
    bounded_writer->stop();
    RODAQ_TEST_CHECK(bounded_writer->dropped.load(std::memory_order_relaxed) == 0u);
    RODAQ_TEST_CHECK(
        bounded_writer->written.load(std::memory_order_relaxed) ==
        bounded_writer->received.load(std::memory_order_relaxed));
    RODAQ_TEST_CHECK(ros_instance.close(Deadline::after(2s)).ok());
    RODAQ_TEST_CHECK(mcap_api::close(mcap_operation).ok());
    RODAQ_TEST_CHECK(mcap_instance.close(Deadline::after(2s)).ok());
    RODAQ_TEST_CHECK(manager.shutdown(Deadline::after(2s)).ok());
    RODAQ_TEST_CHECK(rclcpp::ok());

    mcap::McapReader reader;
    RODAQ_TEST_CHECK(reader.open(output.string()).code == mcap::StatusCode::Success);
    std::size_t count = 0u;
    for (const auto& sample : reader.readMessages()) {
        (void)sample;
        ++count;
    }
    RODAQ_TEST_CHECK(count == bounded_writer->written.load(std::memory_order_relaxed));
    reader.close();
    publisher.reset();
    node.reset();
    if (rclcpp::ok()) {
        rclcpp::shutdown();
    }
    return 0;
}
