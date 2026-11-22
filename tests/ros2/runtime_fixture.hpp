#pragma once

#include <rodaq/acquisition/interface.hpp>
#include <rodaq/acquisition/service.hpp>
#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/registry.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/version.h>
#include <rmw/rmw.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <fstream>
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
    struct Call {
        explicit Call(std::atomic<unsigned>& count) : count_(count) {
            EXPECT_EQ(count_.fetch_add(1), 0u);
        }

        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;

        ~Call() { count_.fetch_sub(1); }

        std::atomic<unsigned>& count_;
    };

    rodaq::Result<acquisition::DescriptorToken> propose_descriptor(
        const acquisition::DescriptorProposal& descriptor, plugin::Deadline) {
        Call call(calls);
        std::unique_lock<std::mutex> lock(mutex);
        descriptor_active = true;
        changed.notify_all();
        changed.wait(lock, [&] { return !block_descriptor; });
        descriptor_active = false;
        if (reject_descriptor) {
            return rodaq::Status(StatusCode::ResourceExhausted, "Test Core rejected descriptor");
        }
        EXPECT_EQ(descriptor.data_profile, RODAQ_DATA_PROFILE_ROS2);
        EXPECT_EQ(
            std::string_view(descriptor.message_encoding.data, descriptor.message_encoding.size),
            "cdr");
        schemas.emplace_back(
            reinterpret_cast<const char*>(descriptor.schema.data), descriptor.schema.size);
        EXPECT_LE(descriptor.stream_id, tokens.size());
        tokens.at(descriptor.stream_id - 1) = {schemas.size(), 1};
        changed.notify_all();
        return acquisition::DescriptorToken{schemas.size(), 1};
    }

    rodaq::Result<acquisition::SubmitResult> submit_record(
        const acquisition::RecordAttempt& record) {
        Call call(calls);
        std::unique_lock<std::mutex> lock(mutex);
        ++active;
        high_water = std::max(high_water, active);
        changed.notify_all();
        changed.wait(lock, [&] { return !blocked; });
        assert_token(record);
        if (payloads.size() < 256) {
            payloads.emplace_back(
                record.payload.data, record.payload.data + record.payload.logical_size);
        }
        EXPECT_EQ(record.timestamp_count, 0);
        EXPECT_EQ(record.has_source_session_id, 0);
#if RCLCPP_VERSION_MAJOR < 28
        EXPECT_EQ(record.has_source_id, 0);
        EXPECT_EQ(record.has_source_sequence, 0);
        EXPECT_EQ(record.sequence_scope, RODAQ_SEQUENCE_SCOPE_INVALID);
#else
        if (record.has_source_id) {
            EXPECT_EQ(record.source_id.size, RMW_GID_STORAGE_SIZE);
            EXPECT_EQ(record.source_id.kind, RODAQ_SOURCE_ID_ROS2_GID);
        }
#endif
        --active;
        changed.notify_all();
        return acquisition::SubmitResult{
            {sizeof(acquisition::SubmitResult)}, RODAQ_SUBMIT_ACCEPTED, 0};
    }

    rodaq::Result<void> report_source_event(const acquisition::SourceEvent& event) {
        Call call(calls);
        std::lock_guard<std::mutex> lock(mutex);
        ++sources[event.kind];
        changed.notify_all();
        return {};
    }

    rodaq::Result<void> report_acquisition_event(const acquisition::AcquisitionEvent& event) {
        Call call(calls);
        std::lock_guard<std::mutex> lock(mutex);
        ++events[event.kind];
        if (event.kind == RODAQ_ACQUISITION_EVENT_OVERFLOW) {
            overflow += event.count;
        }
        changed.notify_all();
        return {};
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

    void assert_token(const acquisition::RecordAttempt& record) {
        EXPECT_LE(record.stream_id, tokens.size());
        const auto token = tokens.at(record.stream_id - 1);
        EXPECT_EQ(record.descriptor_token.value, token.value);
        EXPECT_EQ(record.descriptor_token.generation, token.generation);
    }

    std::atomic<unsigned> calls{0};
    std::array<acquisition::DescriptorToken, 2> tokens{};
    std::mutex mutex;
    std::condition_variable changed;
    bool blocked{false};
    bool block_descriptor{false};
    bool descriptor_active{false};
    bool reject_descriptor{false};
    unsigned active{0};
    unsigned high_water{0};
    std::uint64_t overflow{0};
    std::array<unsigned, 11> events{};
    std::array<unsigned, 7> sources{};
    std::vector<std::string> schemas;
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
            ASSERT_LE(domain_id, 232);
        }
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-ros2-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern.data();
        fs::copy(RODAQ_ROS2_PACKAGE, root / "packages", fs::copy_options::recursive);
        for (const auto& entry : fs::recursive_directory_iterator(root / "packages")) {
            fs::permissions(entry.path(), fs::perms::owner_read | fs::perms::owner_exec);
        }
        fs::permissions(root / "packages", fs::perms::owner_read | fs::perms::owner_exec);
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
        ASSERT_TRUE(services.add_shared<acquisition::Service>(receiver).ok());
        ASSERT_TRUE(services.freeze().ok());
        context = std::make_shared<rclcpp::Context>();
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
        }
        context.reset();
        std::error_code error;
        fs::permissions(root / "packages", fs::perms::owner_all, error);
        for (const auto& entry : fs::recursive_directory_iterator(root / "packages", error)) {
            fs::permissions(entry.path(), fs::perms::owner_all, error);
        }
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

    void create(const nlohmann::json& config) {
        auto created = library->create_instance(services, config.dump());
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
        return handle->prepare(envelope, deadline);
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
        // Qualified Cyclone DDS 0.10.5 aligns serialized transport payloads to
        // four bytes. This is the independent publisher oracle; RoDAQ itself
        // must preserve every received byte and never normalize that padding.
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
    void open_core(bool robot = false) {
        library.reset();
        ASSERT_TRUE(manager->shutdown(soon()).ok());
        manager.reset();
        fs::permissions(root / "packages", fs::perms::owner_all);
        fs::copy(RODAQ_MCAP_PACKAGE, root / "packages/mcap", fs::copy_options::recursive);
        for (const auto& entry : fs::recursive_directory_iterator(root / "packages/mcap")) {
            fs::permissions(entry.path(), fs::perms::owner_read | fs::perms::owner_exec);
        }
        fs::permissions(root / "packages/mcap", fs::perms::owner_read | fs::perms::owner_exec);
        fs::permissions(root / "packages", fs::perms::owner_read | fs::perms::owner_exec);
        fs::create_directory(root / "state");
        fs::create_directory(root / "volume");
        rodaq::daq::StartupOptions options;
        options.state_root = root / "state";
        options.volume_root = root / "volume";
        options.mode = rodaq::daq::DeploymentOpenMode::Initialize;
        options.plugins.search_paths = {root / "packages"};
        options.plugins.max_instances = 4;
        options.plugins.max_loaded_plugins = 2;
        options.plugins.max_config_size = 16384;
        options.plugins.shutdown_timeout = 5s;
        options.capacity = {256 * 1024 * 1024, 8 * 1024 * 1024, 1024 * 1024, 5000000000, 8, 8, 16};
        options.ledger_records = 8;
        options.settings.deployment_generation = 1;
        options.settings.qualified_runtime_profile_id = "s3.integration.evidence-only";
        options.settings.output_root = "local";
        options.settings.tasks.config.max_pipelines = 1;
        options.settings.allowed_plugins = {
            {"com.rodaq.acquisition.ros2", "1.0.0", rodaq::task::AdmissionRole::Acquisition},
            { "com.rodaq.storage.mcap",
              "1.0.0",
              rodaq::task::AdmissionRole::Storage }};
        options.settings.storage = {{ "com.rodaq.storage.mcap", "1.0.0", "mcap" }};
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
            2,
            128,
            1,
            1,
            1};
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
    plugin::ServiceRegistry services;
    std::optional<plugin::PluginManager> manager;
    std::optional<plugin::PluginLibrary> library;
    std::optional<plugin::PluginInstance> instance;
    std::optional<acquisition::Handle> handle;
    rclcpp::Context::SharedPtr context;
    rclcpp::Node::SharedPtr node;
};

}  // namespace rodaq::test::ros2_backend
