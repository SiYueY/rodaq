#pragma once

#include <rodaq/acquisition/interface.hpp>
#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>

#include "ros2/ros2.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/version.h>
#include <rmw/rmw.h>

#include "native_resources.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#ifdef RODAQ_MCAP_PACKAGE
#include <mcap/reader.hpp>

#include "daq/startup.hpp"
#endif

namespace rodaq::test::ros2_backend {

namespace fs = std::filesystem;
namespace acquisition = rodaq::acquisition;
namespace plugin = rodaq::plugin;
using rodaq::StatusCode;
using namespace std::chrono_literals;

inline plugin::Deadline soon() { return plugin::Deadline::after(5s).value(); }

class Receiver {
public:
    struct Subscription {
        std::string distribution;
        std::string rmw;
        rodaq_ros2_qos_profile_t requested;
        std::uint64_t generation;
        std::uint32_t threads;
    };

    struct Call {
        explicit Call(std::atomic<unsigned>& count) : count_(count) {
            EXPECT_EQ(count_.fetch_add(1), 0u);
        }
        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;
        ~Call() { count_.fetch_sub(1); }
        std::atomic<unsigned>& count_;
    };

    static rodaq_status_code_t c_prepare_descriptor(
        void* ctx, rodaq_stream_id_t stream_id, const rodaq_acquisition_descriptor_t* descriptor,
        rodaq_descriptor_id_t* descriptor_id, rodaq_error_t*) noexcept {
        return reinterpret_cast<Receiver*>(ctx)->prepare_descriptor(
            stream_id, *descriptor, descriptor_id);
    }

    static rodaq_record_disposition_t c_submit_record(
        void* ctx, const rodaq_acquisition_record_attempt_t* record) noexcept {
        return reinterpret_cast<Receiver*>(ctx)->submit_record(*record);
    }

    static void c_report_event(void* ctx, const rodaq_acquisition_event_t* event) noexcept {
        reinterpret_cast<Receiver*>(ctx)->report_event(*event);
    }

    acquisition::CallbacksTable table() noexcept {
        return {
            {sizeof(acquisition::CallbacksTable)},
            this,
            c_prepare_descriptor,
            c_submit_record,
            c_report_event};
    }

    rodaq_status_code_t prepare_descriptor(
        rodaq_stream_id_t stream_id, const acquisition::Descriptor& descriptor,
        rodaq_descriptor_id_t* out_id) {
        Call call(calls);
        std::unique_lock<std::mutex> lock(mutex);
        descriptor_active = true;
        changed.notify_all();
        changed.wait(lock, [&] { return !block_descriptor; });
        descriptor_active = false;
        if (reject_descriptor) {
            return RODAQ_STATUS_RESOURCE_EXHAUSTED;
        }
        if (descriptor.metadata.format.size != 8 ||
            std::string_view(descriptor.metadata.format.data, descriptor.metadata.format.size) !=
                "ros2/1.0" ||
            descriptor.metadata.json.size == 0) {
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
        const auto meta = nlohmann::json::parse(
            descriptor.metadata.json.data,
            descriptor.metadata.json.data + descriptor.metadata.json.size);
        if (!meta.is_object() || !meta.contains("subscription")) {
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
        const auto& sub = meta.at("subscription");
        const auto& req = sub.at("installed_requested_qos");
        rodaq_ros2_qos_profile_t requested{};
        requested.history = req.at("history").get<std::uint32_t>();
        requested.reliability = req.at("reliability").get<std::uint32_t>();
        requested.durability = req.at("durability").get<std::uint32_t>();
        requested.liveliness = req.at("liveliness").get<std::uint32_t>();
        requested.depth = req.at("depth").get<std::uint64_t>();
        requested.deadline = {
            req.at("deadline_sec").get<std::uint64_t>(),
            req.at("deadline_nsec").get<std::uint64_t>()};
        requested.lifespan = {
            req.at("lifespan_sec").get<std::uint64_t>(),
            req.at("lifespan_nsec").get<std::uint64_t>()};
        requested.liveliness_lease_duration = {
            req.at("lease_sec").get<std::uint64_t>(), req.at("lease_nsec").get<std::uint64_t>()};
        requested.avoid_ros_namespace_conventions =
            req.at("avoid_ros_namespace_conventions").get<bool>() ? 1 : 0;
        subscriptions.push_back(
            {sub.at("ros_distribution").get<std::string>(),
             sub.at("rmw_implementation").get<std::string>(), requested,
             sub.at("subscription_generation").get<std::uint64_t>(),
             sub.at("executor_threads").get<std::uint32_t>()});
        EXPECT_EQ(
            std::string_view(descriptor.message_encoding.data, descriptor.message_encoding.size),
            "cdr");
        schemas.emplace_back(
            reinterpret_cast<const char*>(descriptor.schema.data), descriptor.schema.size);
        EXPECT_LE(stream_id, descriptor_ids.size());
        const rodaq_descriptor_id_t id = schemas.size();
        descriptor_ids.at(stream_id - 1) = id;
        *out_id = id;
        changed.notify_all();
        return RODAQ_STATUS_OK;
    }

    rodaq_record_disposition_t submit_record(const acquisition::RecordAttempt& record) {
        Call call(calls);
        std::unique_lock<std::mutex> lock(mutex);
        ++active;
        high_water = std::max(high_water, active);
        changed.notify_all();
        changed.wait(lock, [&] { return !blocked; });
        EXPECT_LE(record.stream_id, descriptor_ids.size());
        EXPECT_EQ(record.descriptor_id, descriptor_ids.at(record.stream_id - 1));
        if (payloads.size() < 256) {
            payloads.emplace_back(record.payload.data, record.payload.data + record.payload.size);
        }
        --active;
        changed.notify_all();
        return RODAQ_SUBMIT_ACCEPTED;
    }

    void report_event(const acquisition::AcquisitionEvent& event) {
        Call call(calls);
        std::lock_guard<std::mutex> lock(mutex);
        if (event.kind < events.size()) {
            ++events[event.kind];
        }
        if (event.kind == RODAQ_ACQUISITION_EVENT_OVERFLOW) {
            overflow += event.count;
        } else if (event.kind == RODAQ_ACQUISITION_EVENT_DROPPED) {
            dropped += event.count;
        }
        changed.notify_all();
    }

    template <typename Predicate>
    bool wait(Predicate ready, std::chrono::milliseconds duration = 5s) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, duration, ready);
    }

    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        blocked = false;
        block_descriptor = false;
        changed.notify_all();
    }

    std::atomic<unsigned> calls{0};
    std::array<rodaq_descriptor_id_t, 2> descriptor_ids{};
    std::mutex mutex;
    std::condition_variable changed;
    bool blocked{false};
    bool block_descriptor{false};
    bool descriptor_active{false};
    bool reject_descriptor{false};
    unsigned active{0};
    unsigned high_water{0};
    std::uint64_t overflow{0};
    std::uint64_t dropped{0};
    std::array<unsigned, 11> events{};
    std::vector<std::string> schemas;
    std::vector<Subscription> subscriptions;
    std::vector<std::vector<std::uint8_t>> payloads;
};

class Ros2Runtime : public testing::Test {
protected:
    void SetUp() override {
        if (const auto* selected = std::getenv("RODAQ_ROS2_TEST_DOMAIN_ID")) {
            const std::string_view input(selected);
            const auto parsed =
                std::from_chars(input.data(), input.data() + input.size(), domain_id);
            ASSERT_EQ(parsed.ec, std::errc{});
            ASSERT_EQ(parsed.ptr, input.data() + input.size());
            ASSERT_LE(domain_id, 232U);
        }
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-ros2-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern.data();
        fs::copy(RODAQ_ROS2_PACKAGE, root / "packages", fs::copy_options::recursive);
        plugin::PluginManagerOptions options;
        options.search_paths = {root / "packages"};
        options.max_instances = 1;
        options.max_loaded_plugins = 1;
        options.max_config_size = 16384;
        options.shutdown_timeout = 5s;
        auto created = plugin::PluginManager::create(options);
        ASSERT_TRUE(created.ok());
        manager.emplace(std::move(created.value()));
        auto loaded = manager->load("com.rodaq.acquisition.ros2", "1.0.0");
        ASSERT_TRUE(loaded.ok()) << loaded.status().message();
        library.emplace(std::move(loaded.value()));
        receiver = std::make_shared<Receiver>();
        context = std::make_shared<ros2_acquisition::Context>();
        rclcpp::InitOptions init;
        init.set_domain_id(domain_id);
        init.auto_initialize_logging(false);
        context->init(0, nullptr, init);
        rclcpp::NodeOptions node_options;
        node_options.context(context)
            .start_parameter_services(false)
            .start_parameter_event_publisher(false)
            .enable_rosout(false);
        node = std::make_shared<rclcpp::Node>("rodaq_test_publisher", node_options);
    }

    void TearDown() override {
#ifdef RODAQ_MCAP_PACKAGE
        if (core) {
            EXPECT_TRUE(core->shutdown(std::chrono::steady_clock::now() + 10s).ok());
            core.reset();
        }
#endif
        if (receiver) {
            receiver->release();
        }
        if (handle) {
            EXPECT_TRUE(handle->quiesce(soon()).ok());
        }
        handle.reset();
        if (instance) {
            EXPECT_TRUE(instance->close(soon()).ok());
        }
        instance.reset();
        library.reset();
        if (manager) {
            EXPECT_TRUE(manager->shutdown(soon()).ok());
        }
        manager.reset();
        node.reset();
        if (context) {
            context->shutdown("test complete");
            EXPECT_TRUE(context->release_after_join().ok());
        }
        context.reset();
        std::error_code error;
        fs::remove_all(root, error);
    }

    nlohmann::json config(unsigned count = 1, bool type = true) {
        auto streams = nlohmann::json::array();
        for (unsigned i = 0; i < count; ++i) {
            nlohmann::json stream{
                {"stream_key", "s" + std::to_string(i)},
                {"topic", "/rodaq_test_" + std::to_string(i)},
                {"qos", {{"mode", "adapt"}, {"depth", "100"}}}};
            if (type) {
                stream["type"] = "std_msgs/msg/String";
            }
            streams.push_back(std::move(stream));
        }
        return {
            {"format_version", "1.0"},
            {"domain_id", domain_id},
            {"executor_threads", 4},
            {"graph_poll_interval", {{"value", "20"}, {"unit", "ms"}}},
            {"qos_change_debounce", {{"value", "0"}, {"unit", "ms"}}},
            {"streams", streams}};
    }

    void create(const nlohmann::json& cfg) {
        auto created = library->create_instance(cfg.dump());
        ASSERT_TRUE(created.ok()) << created.status().message();
        instance.emplace(std::move(created.value()));
        auto queried = instance->query<acquisition::Interface>();
        ASSERT_TRUE(queried.ok());
        handle.emplace(std::move(queried.value()));
    }

    rodaq::Result<void> prepare(
        unsigned count = 1, unsigned capacity = 1, plugin::Deadline deadline = soon()) {
        const std::array<acquisition::StreamBinding, 2> bindings{
            {{{sizeof(acquisition::StreamBinding)}, 1, {"s0", 2}, 1048576},
             {{sizeof(acquisition::StreamBinding)}, 2, {"s1", 2}, 1048576}}};
        const acquisition::PrepareConfig envelope{
            {sizeof(envelope)}, bindings.data(), count, capacity};
        return handle->prepare(envelope, receiver->table(), deadline);
    }

    static bool connected(const rclcpp::GenericPublisher::SharedPtr& publisher) {
        const auto end = std::chrono::steady_clock::now() + 5s;
        while (publisher->get_subscription_count() == 0 && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(10ms);
        }
        return publisher->get_subscription_count() != 0;
    }

    static std::vector<std::uint8_t> bytes() {
        return {0, 1, 0, 0, 6, 0, 0, 0, 'h', 'e', 'l', 'l', 'o', 0};
    }

    static std::vector<std::uint8_t> received_cdr(std::vector<std::uint8_t> input) {
        if (std::string_view(rmw_get_implementation_identifier()) == "rmw_cyclonedds_cpp") {
            input.resize((input.size() + 3) / 4 * 4, 0);
        }
        return input;
    }

    static void publish(const rclcpp::GenericPublisher::SharedPtr& publisher) {
        const auto input = bytes();
        rclcpp::SerializedMessage message(input.size());
        auto& raw = message.get_rcl_serialized_message();
        std::copy(input.begin(), input.end(), raw.buffer);
        raw.buffer_length = input.size();
        publisher->publish(message);
    }

    template <typename Predicate>
    bool publish_until(const rclcpp::GenericPublisher::SharedPtr& publisher, Predicate ready) {
        for (unsigned i = 0; i < 50; ++i) {
            publish(publisher);
            if (receiver->wait(ready, 100ms)) {
                return true;
            }
        }
        std::lock_guard<std::mutex> lock(receiver->mutex);
        ADD_FAILURE() << "No delivery: terminal="
                      << receiver->events[RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE]
                      << " overflow=" << receiver->overflow
                      << " schemas=" << receiver->schemas.size();
        return false;
    }

#ifdef RODAQ_MCAP_PACKAGE
    void open_core(bool robot = false, const fs::path& recovery_volume = {}) {
        library.reset();
        ASSERT_TRUE(manager->shutdown(soon()).ok());
        manager.reset();
        fs::copy(RODAQ_MCAP_PACKAGE, root / "packages/mcap", fs::copy_options::recursive);
        fs::create_directory(root / "state");
        fs::create_directory(root / "volume");
        rodaq::daq::StartupOptions options;
        options.state_root = root / "state";
        options.volume_root = recovery_volume.empty() ? root / "volume" : recovery_volume;
        options.mode = rodaq::daq::DeploymentOpenMode::Initialize;
        options.plugins.search_paths = {root / "packages"};
        options.plugins.max_instances = 4;
        options.plugins.max_loaded_plugins = 2;
        options.plugins.max_config_size = 16384;
        options.plugins.shutdown_timeout = 5s;
        options.capacity = {256 * 1024 * 1024, 64 * 1024 * 1024, 1024 * 1024, 5000000000, 8, 8, 16,
                            2 * 1024 * 1024,   4 * 1024 * 1024,  1000000000};
        options.settings.deployment_generation = 1;
        options.settings.qualified_runtime_profile_id = "s3.integration.evidence-only";
        options.settings.output_root = "local";
        options.settings.tasks.config.max_pipelines = 1;
        options.settings.allowed_plugins = {
            {"com.rodaq.acquisition.ros2", "1.0.0", rodaq::task::AdmissionRole::Acquisition},
            {"com.rodaq.storage.mcap", "1.0.0", rodaq::task::AdmissionRole::Storage}};
        options.settings.storage = {{"com.rodaq.storage.mcap", "1.0.0", "mcap"}};
        rodaq::daq::ResourceProfile profile;
        profile.name = "basic";
        profile.instance.records = 256;
        profile.instance.logical_bytes = 1048576;
        profile.instance.payloads = {1048576, 4096, 4096, 256};
        profile.instance.queue_capacity = 256;
        profile.instance.callback_concurrency = 2;
        profile.storage = {
            {sizeof(rodaq::storage::PrepareConfig)},
            4096,
            65536,
            16,
            16,
            10000,
            1048576,
            32 * 1024 * 1024,
            1,
            128};
        profile.segment_reservation_bytes = 32 * 1024 * 1024;
        profile.max_prepared_descriptors = 16;
        profile.manifest.max_bytes = 65536;
        if (robot) {
            profile.instance.logical_bytes = 16 * 1024 * 1024;
            profile.instance.payloads = {16 * 1024 * 1024, 4096, 1024 * 1024, 32};
            profile.storage.max_record_bytes = 1024 * 1024;
            profile.storage.max_batch_bytes = 4 * 1024 * 1024;
            profile.storage.max_batch_records = 4;
            profile.storage.max_segment_input_bytes = 64 * 1024 * 1024;
        }
        options.settings.profiles.push_back(profile);
        auto opened = rodaq::daq::Composition::open(std::move(options));
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        core = std::move(opened.value());
    }

    bool completed(TaskId id, OperationId operation) {
        const auto end = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < end) {
            auto result = core->tasks().get_operation(id, operation);
            if (!result.ok()) {
                return false;
            }
            if (result.value().state == OperationState::Succeeded) {
                return true;
            }
            if (result.value().state == OperationState::Failed) {
                ADD_FAILURE() << "Core operation failed: "
                              << static_cast<int>(result.value().status_code) << " "
                              << result.value().diagnostic;
                return false;
            }
            std::this_thread::sleep_for(2ms);
        }
        return false;
    }

    Result<std::vector<std::byte>> read_file(ArtifactId artifact, ArtifactFileId file) {
        auto opened = core->artifacts().acquire_read(artifact, file);
        if (!opened.ok()) {
            return opened.status();
        }
        if (opened.value().size() > 64 * 1024 * 1024) {
            return Status(StatusCode::ResourceExhausted, "Qualification read budget exceeded");
        }
        std::vector<std::byte> output(static_cast<std::size_t>(opened.value().size()));
        std::size_t offset = 0;
        while (offset < output.size()) {
            const auto chunk = std::min<std::size_t>(1024 * 1024, output.size() - offset);
            auto read = opened.value().read_at(offset, {output.data() + offset, chunk});
            if (!read.ok()) {
                return read.status();
            }
            if (read.value() == 0) {
                return Status(StatusCode::DataLoss, "Unexpected Artifact EOF");
            }
            offset += read.value();
        }
        return output;
    }

    std::unique_ptr<daq::Composition> core;
#endif

    fs::path root;
    std::uint32_t domain_id{173};
    std::shared_ptr<Receiver> receiver;
    std::optional<plugin::PluginManager> manager;
    std::optional<plugin::PluginLibrary> library;
    std::optional<plugin::PluginInstance> instance;
    std::optional<acquisition::Handle> handle;
    std::shared_ptr<ros2_acquisition::Context> context;
    rclcpp::Node::SharedPtr node;
};

}  // namespace rodaq::test::ros2_backend
