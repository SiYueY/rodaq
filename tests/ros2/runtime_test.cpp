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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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

namespace {

namespace fs = std::filesystem;
namespace acquisition = rodaq::acquisition;
namespace plugin = rodaq::plugin;
using rodaq::StatusCode;
using namespace std::chrono_literals;

plugin::Deadline soon() { return plugin::Deadline::after(5s).value(); }

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
        init.set_domain_id(173);
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
            {"domain_id", 173},
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

    fs::path root;
    std::shared_ptr<Receiver> receiver;
    plugin::ServiceRegistry services;
    std::optional<plugin::PluginManager> manager;
    std::optional<plugin::PluginLibrary> library;
    std::optional<plugin::PluginInstance> instance;
    std::optional<acquisition::Handle> handle;
    rclcpp::Context::SharedPtr context;
    rclcpp::Node::SharedPtr node;
};

TEST_F(Ros2Runtime, IdleExplicitTypeIsLocallyReadyAndPrivateShutdownPreservesOtherContext) {
    create(config());
    ASSERT_TRUE(handle);
    auto prepared = prepare();
    ASSERT_TRUE(prepared.ok()) << prepared.status().message();
    ASSERT_TRUE(handle->start(soon()).ok());
    ASSERT_TRUE(handle->quiesce(soon()).ok());
    EXPECT_TRUE(context->is_valid());
    std::lock_guard<std::mutex> lock(receiver->mutex);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 1);
    EXPECT_EQ(receiver->sources[RODAQ_SOURCE_EVENT_DISCOVERED], 0);
    EXPECT_TRUE(receiver->payloads.empty());
    ASSERT_EQ(receiver->schemas.size(), 1);
    EXPECT_NE(receiver->schemas[0].find("string data\n"), std::string::npos);
}

TEST_F(Ros2Runtime, MissingDiscoveredTypeFailsWithoutClaimingReady) {
    create(config(1, false));
    ASSERT_TRUE(handle);
    EXPECT_EQ(
        prepare(1, 1, plugin::Deadline::after(50ms).value()).status().code(),
        StatusCode::Unavailable);
    std::lock_guard<std::mutex> lock(receiver->mutex);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 0);
}

TEST_F(Ros2Runtime, DuplicateBindingIdentityIsRejectedBeforeAnyRosResourceReadiness) {
    create(config(2));
    ASSERT_TRUE(handle);
    const std::array<acquisition::StreamBinding, 2> bindings{
        {{{sizeof(acquisition::StreamBinding)}, 1, {"s0", 2}, 4096},
         {{sizeof(acquisition::StreamBinding)}, 1, {"s1", 2}, 4096}}};
    const acquisition::PrepareConfig envelope{{sizeof(envelope)}, bindings.data(), 2, 1};
    EXPECT_EQ(handle->prepare(envelope, soon()).status().code(), StatusCode::InvalidArgument);
    std::lock_guard<std::mutex> lock(receiver->mutex);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_TRANSPORT_READY], 0);
    EXPECT_TRUE(receiver->schemas.empty());
}

TEST_F(Ros2Runtime, RejectedCoreDescriptorCannotBecomeStreamReady) {
    create(config());
    ASSERT_TRUE(handle);
    receiver->reject_descriptor = true;
    EXPECT_EQ(prepare().status().code(), StatusCode::ResourceExhausted);
    EXPECT_TRUE(handle->quiesce(soon()).ok());
    EXPECT_TRUE(context->is_valid());
    std::lock_guard<std::mutex> lock(receiver->mutex);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 0);
    EXPECT_TRUE(receiver->payloads.empty());
}

TEST_F(Ros2Runtime, LatePreparationStaysOwnedAndCannotPublishReadyAfterQuiesce) {
    create(config());
    ASSERT_TRUE(handle);
    receiver->block_descriptor = true;
    auto preparing = std::async(std::launch::async, [&] { return prepare(); });
    const bool entered = receiver->wait([&] { return receiver->descriptor_active; });
    if (!entered) {
        receiver->release();
    }
    ASSERT_TRUE(entered);
    EXPECT_EQ(
        handle->quiesce(plugin::Deadline::after(20ms).value()).status().code(),
        StatusCode::DeadlineExceeded);
    receiver->release();
    EXPECT_FALSE(preparing.get().ok());
    EXPECT_TRUE(handle->quiesce(soon()).ok());
    std::lock_guard<std::mutex> lock(receiver->mutex);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 0);
    EXPECT_TRUE(receiver->payloads.empty());
}

TEST_F(Ros2Runtime, LatePublisherDeliversExactBorrowedCdrAndCompatibleRequestStaysBestEffort) {
    create(config());
    ASSERT_TRUE(handle);
    ASSERT_TRUE(prepare().ok());
    ASSERT_TRUE(handle->start(soon()).ok());
    auto publisher = node->create_generic_publisher(
        "/rodaq_test_0", "std_msgs/msg/String", rclcpp::QoS(10).reliable());
    ASSERT_TRUE(connected(publisher));
    ASSERT_TRUE(publish_until(publisher, [&] { return !receiver->payloads.empty(); }));
    ASSERT_TRUE(
        receiver->wait([&] { return receiver->sources[RODAQ_SOURCE_EVENT_DISCOVERED] != 0; }));
    const auto endpoints = node->get_subscriptions_info_by_topic("/rodaq_test_0");
    ASSERT_EQ(endpoints.size(), 1);
    EXPECT_EQ(
        endpoints[0].qos_profile().get_rmw_qos_profile().reliability,
        RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT);
    ASSERT_TRUE(handle->quiesce(soon()).ok());
    std::lock_guard<std::mutex> lock(receiver->mutex);
    ASSERT_FALSE(receiver->payloads.empty());
    for (const auto& payload : receiver->payloads) {
        EXPECT_EQ(payload, bytes());
    }
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE], 0);
}

TEST_F(Ros2Runtime, SaturationIsBoundedAndQuiesceTimeoutRetainsActualCallback) {
    create(config(2));
    ASSERT_TRUE(handle);
    ASSERT_TRUE(prepare(2, 1).ok());
    ASSERT_TRUE(handle->start(soon()).ok());
    auto first = node->create_generic_publisher(
        "/rodaq_test_0", "std_msgs/msg/String", rclcpp::QoS(100).best_effort());
    auto second = node->create_generic_publisher(
        "/rodaq_test_1", "std_msgs/msg/String", rclcpp::QoS(100).best_effort());
    ASSERT_TRUE(connected(first));
    ASSERT_TRUE(connected(second));
    {
        std::lock_guard<std::mutex> lock(receiver->mutex);
        receiver->blocked = true;
    }
    ASSERT_TRUE(publish_until(first, [&] { return receiver->active == 1; }));
    for (unsigned i = 0; i < 20; ++i) {
        publish(second);
        std::this_thread::sleep_for(2ms);
    }
    receiver->release();
    ASSERT_TRUE(receiver->wait([&] { return receiver->overflow != 0; }));
    {
        std::lock_guard<std::mutex> lock(receiver->mutex);
        receiver->blocked = true;
    }
    ASSERT_TRUE(publish_until(first, [&] { return receiver->active == 1; }));
    auto stopped = handle->quiesce(plugin::Deadline::after(20ms).value());
    EXPECT_EQ(stopped.status().code(), StatusCode::DeadlineExceeded);
    receiver->release();
    EXPECT_TRUE(handle->quiesce(soon()).ok());
    EXPECT_TRUE(context->is_valid());
    std::lock_guard<std::mutex> lock(receiver->mutex);
    EXPECT_EQ(receiver->high_water, 1);
    EXPECT_EQ(receiver->active, 0);
}

TEST_F(Ros2Runtime, DiscoveryAndIncompatibleOfferRebuildUsePreparedTokensAndReportGap) {
    auto first = node->create_generic_publisher(
        "/rodaq_test_0", "std_msgs/msg/String", rclcpp::QoS(10).reliable());
    create(config(1, false));
    ASSERT_TRUE(handle);
    auto prepared = prepare();
    ASSERT_TRUE(prepared.ok()) << prepared.status().message();
    ASSERT_TRUE(handle->start(soon()).ok());
    ASSERT_TRUE(connected(first));
    auto installed = node->get_subscriptions_info_by_topic("/rodaq_test_0");
    ASSERT_EQ(installed.size(), 1);
    ASSERT_EQ(
        installed[0].qos_profile().get_rmw_qos_profile().reliability,
        RMW_QOS_POLICY_RELIABILITY_RELIABLE);
    ASSERT_TRUE(publish_until(first, [&] { return !receiver->payloads.empty(); }));
    auto second = node->create_generic_publisher(
        "/rodaq_test_0", "std_msgs/msg/String", rclcpp::QoS(10).best_effort());
    ASSERT_TRUE(receiver->wait([&] {
        return receiver->events[RODAQ_ACQUISITION_EVENT_STREAM_UNAVAILABLE] != 0 &&
               receiver->events[RODAQ_ACQUISITION_EVENT_STREAM_READY] >= 2;
    }));
    ASSERT_TRUE(connected(second));
    std::size_t before;
    {
        std::lock_guard<std::mutex> lock(receiver->mutex);
        before = receiver->payloads.size();
    }
    ASSERT_TRUE(publish_until(second, [&] { return receiver->payloads.size() > before; }));
    installed = node->get_subscriptions_info_by_topic("/rodaq_test_0");
    ASSERT_EQ(installed.size(), 1);
    EXPECT_EQ(
        installed[0].qos_profile().get_rmw_qos_profile().reliability,
        RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT);
    ASSERT_TRUE(handle->quiesce(soon()).ok());
    std::lock_guard<std::mutex> lock(receiver->mutex);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE], 0);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_RECOVERABLE_FAILURE], 0);
}

#ifdef RODAQ_MCAP_PACKAGE
TEST_F(Ros2Runtime, FormalTaskRecordsRos2ThroughMcapCommitAndArtifactRead) {
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
        2,
        128,
        1,
        1,
        1};
    profile.max_prepared_descriptors = 16;
    profile.manifest.max_bytes = 65536;
    options.settings.profiles.push_back(profile);
    auto opened = rodaq::daq::Composition::open(std::move(options));
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto composition = std::move(opened.value());
    // An assertion failure must still stop actual Core/Native work before the
    // composition's strict destructor and temporary namespace removal.
    struct Shutdown {
        rodaq::daq::Composition& composition;
        ~Shutdown() {
            EXPECT_TRUE(composition.shutdown(std::chrono::steady_clock::now() + 10s).ok());
        }
    } shutdown{*composition};
    rodaq::TaskConfig task;
    task.name = "ROS2 MCAP actual Core integration";
    task.segment_policy.max_duration = 60s;
    rodaq::PipelineConfig pipeline;
    pipeline.resource_profile = "basic";
    pipeline.output_root = "local";
    pipeline.acquisition = {"com.rodaq.acquisition.ros2", "1.0.0", config().dump()};
    pipeline.storage = {"com.rodaq.storage.mcap", "1.0.0", R"({"format_version":"1.0"})"};
    pipeline.streams = {{0, "s0", {}, rodaq::ReadinessPolicy::EntryReady}};
    if (std::getenv("RODAQ_ROS2_EXTERNAL_PUBLISHER") != nullptr) {
        pipeline.streams[0].readiness_policy = rodaq::ReadinessPolicy::SourceReady;
    }
    task.pipelines.push_back(std::move(pipeline));
    auto created = composition->tasks().create_task(task, {"create"});
    ASSERT_TRUE(created.ok()) << created.status().message();
    const auto id = created.value();
    const auto completed = [&](rodaq::OperationId operation) {
        const auto end = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < end) {
            auto result = composition->tasks().get_operation(id, operation);
            if (!result.ok()) {
                return false;
            }
            if (result.value().state == rodaq::OperationState::Succeeded) {
                return true;
            }
            if (result.value().state == rodaq::OperationState::Failed) {
                ADD_FAILURE() << "Core operation failed: "
                              << static_cast<int>(result.value().status_code) << " "
                              << result.value().diagnostic;
                return false;
            }
            std::this_thread::sleep_for(2ms);
        }
        return false;
    };
    auto start = composition->tasks().start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok()) << start.status().message();
    ASSERT_TRUE(completed(start.value()));
    if (std::getenv("RODAQ_ROS2_EXTERNAL_PUBLISHER") != nullptr) {
        // Qualification harness supplies a real publisher from another ROS
        // distro/process. No local producer may mask that transport path.
        std::this_thread::sleep_for(3s);
    } else {
        auto publisher = node->create_generic_publisher(
            "/rodaq_test_0", "std_msgs/msg/String", rclcpp::QoS(100).reliable());
        ASSERT_TRUE(connected(publisher));
        for (unsigned i = 0; i < 30; ++i) {
            publish(publisher);
            std::this_thread::sleep_for(10ms);
        }
    }
    auto stop = composition->tasks().stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(completed(stop.value()));
    ASSERT_TRUE(composition->observe().ok());
    auto artifacts = composition->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    ASSERT_EQ(artifacts.value().items.size(), 1);
    auto artifact = composition->artifacts().get_artifact(artifacts.value().items[0].artifact_id);
    ASSERT_TRUE(artifact.ok());
    ASSERT_EQ(artifact.value().files.size(), 2);
    ASSERT_EQ(artifact.value().coverage.size(), 1);
    const auto& coverage = artifact.value().coverage[0];
    EXPECT_GT(coverage.accepted, 0);
    EXPECT_EQ(coverage.accepted, coverage.written);
    EXPECT_EQ(coverage.known_failed, 0);
    EXPECT_EQ(coverage.unknown, 0);
    std::uint64_t messages = 0;
    for (const auto& file : artifact.value().files) {
        auto read = composition->artifacts().acquire_read(
            artifact.value().summary.artifact_id, file.file_id);
        ASSERT_TRUE(read.ok());
        ASSERT_LT(read.value().size(), 1048576);
        std::vector<std::byte> output(static_cast<std::size_t>(read.value().size()));
        auto count = read.value().read_at(0, {output.data(), output.size()});
        ASSERT_TRUE(count.ok());
        ASSERT_EQ(count.value(), output.size());
        if (const auto* destination = std::getenv("RODAQ_ROS2_EVIDENCE_DIR")) {
            const fs::path directory(destination);
            ASSERT_TRUE(directory.is_absolute() && fs::is_directory(directory));
            ASSERT_EQ(fs::path(file.display_name).filename().string(), file.display_name);
            std::ofstream evidence(directory / file.display_name, std::ios::binary);
            evidence.write(
                reinterpret_cast<const char*>(output.data()),
                static_cast<std::streamsize>(output.size()));
            ASSERT_TRUE(evidence.good());
        }
        if (file.display_name.size() >= 5 &&
            file.display_name.substr(file.display_name.size() - 5) == ".mcap") {
            const auto exported = root / "read-back.mcap";
            {
                std::ofstream stream(exported, std::ios::binary);
                stream.write(
                    reinterpret_cast<const char*>(output.data()),
                    static_cast<std::streamsize>(output.size()));
                ASSERT_TRUE(stream.good());
            }
            mcap::McapReader reader;
            ASSERT_TRUE(reader.open(exported.string()).ok());
            for (const auto& message : reader.readMessages()) {
                EXPECT_EQ(message.channel->topic, "/rodaq_test_0");
                EXPECT_EQ(message.channel->messageEncoding, "cdr");
                EXPECT_EQ(message.schema->name, "std_msgs/msg/String");
                auto expected = bytes();
                if (std::getenv("RODAQ_ROS2_EXTERNAL_PUBLISHER") != nullptr) {
                    // The qualified rclpy/Fast DDS String serializer includes
                    // two trailing alignment bytes. Preserve those bytes too;
                    // the local raw-CDR publisher deliberately sends 14 bytes.
                    expected.resize(16, 0);
                }
                ASSERT_EQ(message.message.dataSize, expected.size());
                EXPECT_EQ(std::memcmp(message.message.data, expected.data(), expected.size()), 0);
                EXPECT_EQ(message.message.publishTime, message.message.logTime);
                ++messages;
            }
        }
    }
    EXPECT_EQ(messages, coverage.written);
}
#endif

}  // namespace
