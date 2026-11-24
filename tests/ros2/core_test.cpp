#include "runtime_fixture.hpp"

#ifdef RODAQ_MCAP_PACKAGE
#include <map>
#include <set>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>

#include "persistence/file_descriptor.hpp"

#include "rodaqmeta.hpp"

namespace rodaq::test::ros2_backend {

TEST_F(Ros2Runtime, SeparatelyInstalledCustomMessageRecordsWithoutCompiledMessageHeaders) {
    open_core();
    ASSERT_TRUE(core);
    constexpr const char* type = "rodaq_qualification_msgs/msg/Probe";
    auto source = config();
    source["streams"][0]["type"] = type;
    TaskConfig task;
    task.name = "Unknown compiled message with recursive Header schema";
    task.segment_policy.max_duration = 60s;
    PipelineConfig pipeline;
    pipeline.resource_profile = "basic";
    pipeline.output_root = "local";
    pipeline.acquisition = {"com.rodaq.acquisition.ros2", "1.0.0", source.dump()};
    pipeline.storage = {"com.rodaq.storage.mcap", "1.0.0", R"({"format_version":"1.0"})"};
    pipeline.streams = {{0, "s0", {}, ReadinessPolicy::EntryReady}};
    task.pipelines.push_back(std::move(pipeline));
    auto created = core->tasks().create_task(task, {"create"});
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    auto start = core->tasks().start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(completed(id, start.value()));
    auto publisher =
        node->create_generic_publisher("/rodaq_test_0", type, rclcpp::QoS(10).reliable());
    ASSERT_TRUE(connected(publisher));
    // CDR Header {stamp={0,0}, frame_id="probe"}, then data="hello".
    // Neither this application nor the official backend includes or links the
    // separately generated Probe C++ type or typesupport target.
    const std::vector<std::uint8_t> input{0, 1, 0, 0, 0,   0,   0,   0,   0,   0, 0, 0,
                                          6, 0, 0, 0, 'p', 'r', 'o', 'b', 'e', 0, 0, 0,
                                          6, 0, 0, 0, 'h', 'e', 'l', 'l', 'o', 0};
    for (unsigned i = 0; i < 20; ++i) {
        rclcpp::SerializedMessage message(input.size());
        auto& raw = message.get_rcl_serialized_message();
        std::copy(input.begin(), input.end(), raw.buffer);
        raw.buffer_length = input.size();
        publisher->publish(message);
        std::this_thread::sleep_for(10ms);
    }
    auto stop = core->tasks().stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(completed(id, stop.value()));
    ASSERT_TRUE(core->observe().ok());
    auto page = core->artifacts().query_artifacts({});
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().items.size(), 1);
    auto artifact = core->artifacts().get_artifact(page.value().items[0].artifact_id);
    ASSERT_TRUE(artifact.ok());
    ASSERT_EQ(artifact.value().coverage.size(), 1);
    EXPECT_GT(artifact.value().coverage[0].written, 0);
    EXPECT_EQ(artifact.value().coverage[0].accepted, artifact.value().coverage[0].written);
    std::uint64_t count = 0;
    for (const auto& file : artifact.value().files) {
        auto bytes = read_file(artifact.value().summary.artifact_id, file.file_id);
        ASSERT_TRUE(bytes.ok());
        if (const auto* destination = std::getenv("RODAQ_ROS2_EVIDENCE_DIR")) {
            const fs::path directory(destination);
            ASSERT_TRUE(directory.is_absolute() && fs::is_directory(directory));
            ASSERT_EQ(fs::path(file.display_name).filename().string(), file.display_name);
            std::ofstream output(directory / file.display_name, std::ios::binary);
            output.write(
                reinterpret_cast<const char*>(bytes.value().data()),
                static_cast<std::streamsize>(bytes.value().size()));
            ASSERT_TRUE(output.good());
        }
        if (file.display_name.find(".mcap") == std::string::npos) {
            continue;
        }
        const auto path = root / "custom-read.mcap";
        {
            std::ofstream output(path, std::ios::binary);
            output.write(
                reinterpret_cast<const char*>(bytes.value().data()),
                static_cast<std::streamsize>(bytes.value().size()));
            ASSERT_TRUE(output.good());
        }
        mcap::McapReader reader;
        ASSERT_TRUE(reader.open(path.string()).ok());
        for (const auto& message : reader.readMessages()) {
            EXPECT_EQ(message.schema->name, type);
            EXPECT_EQ(message.schema->encoding, "ros2msg");
            const std::string schema(
                reinterpret_cast<const char*>(message.schema->data.data()),
                message.schema->data.size());
            EXPECT_NE(schema.find("MSG: std_msgs/Header\n"), std::string::npos);
            EXPECT_NE(schema.find("MSG: builtin_interfaces/Time\n"), std::string::npos);
            const auto expected = received_cdr(input);
            ASSERT_EQ(message.message.dataSize, expected.size());
            EXPECT_EQ(std::memcmp(message.message.data, expected.data(), expected.size()), 0);
            ++count;
        }
    }
    EXPECT_EQ(count, artifact.value().coverage[0].written);
}

TEST_F(Ros2Runtime, ActualSubscriptionChangesRemainAuditedAndNewRunsFreezeTheirOwnSettings) {
    auto reliable = node->create_generic_publisher(
        "/rodaq_test_0", "std_msgs/msg/String", rclcpp::QoS(10).reliable());
    open_core();
    ASSERT_TRUE(core);
    TaskConfig task;
    task.name = "Actual installed request audit";
    task.segment_policy.max_duration = 60s;
    PipelineConfig pipeline;
    pipeline.resource_profile = "basic";
    pipeline.output_root = "local";
    pipeline.acquisition = {"com.rodaq.acquisition.ros2", "1.0.0", config(1, false).dump()};
    pipeline.storage = {"com.rodaq.storage.mcap", "1.0.0", R"({"format_version":"1.0"})"};
    pipeline.streams = {{0, "s0", {}, ReadinessPolicy::SourceReady}};
    task.pipelines.push_back(std::move(pipeline));
    const auto created = core->tasks().create_task(task, {"create"});
    ASSERT_TRUE(created.ok()) << created.status().message();
    const auto id = created.value();
    const auto start = core->tasks().start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(completed(id, start.value()));
    ASSERT_TRUE(connected(reliable));
    const auto initial_runtime = core->tasks().get_runtime_snapshot(id).value();
    ASSERT_TRUE(initial_runtime.pipelines[0].facts);
    const auto initial_identity = initial_runtime.pipelines[0].facts->effective_config_id;
    ASSERT_TRUE(initial_identity);
    for (unsigned i = 0; i < 10; ++i) {
        publish(reliable);
        std::this_thread::sleep_for(10ms);
    }
    auto best_effort = node->create_generic_publisher(
        "/rodaq_test_0", "std_msgs/msg/String", rclcpp::QoS(10).best_effort());
    bool rebuilt = false;
    const auto rebuilt_by = std::chrono::steady_clock::now() + 3s;
    do {
        const auto readers = node->get_subscriptions_info_by_topic("/rodaq_test_0");
        rebuilt =
            readers.size() == 1 && readers[0].qos_profile().get_rmw_qos_profile().reliability ==
                                       RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
        if (!rebuilt) {
            std::this_thread::sleep_for(1ms);
        }
    } while (!rebuilt && std::chrono::steady_clock::now() < rebuilt_by);
    ASSERT_TRUE(rebuilt);
    ASSERT_TRUE(connected(best_effort));
    const auto send_best_effort = [&] {
        for (unsigned i = 0; i < 10; ++i) {
            publish(best_effort);
            std::this_thread::sleep_for(10ms);
        }
    };
    send_best_effort();
    ASSERT_TRUE(core->observe().ok());
    EXPECT_EQ(
        core->tasks().get_runtime_snapshot(id).value().pipelines[0].facts->effective_config_id,
        initial_identity);
    const auto stop = core->tasks().stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(completed(id, stop.value()));
    const auto read_manifest = [&](std::uint64_t segment) {
        auto bytes = persistence::read_file(
            root / "volume/.rodaq/artifacts" / std::to_string(segment), "manifest.json", 65536);
        if (!bytes.ok()) {
            return Result<artifact::SegmentManifest>(bytes.status());
        }
        return artifact::decode_manifest(bytes.value(), {});
    };
    const auto first_page = core->artifacts().query_artifacts({});
    ASSERT_TRUE(first_page.ok());
    ASSERT_EQ(first_page.value().items.size(), 1);
    const auto first = read_manifest(first_page.value().items[0].segment_id);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(first.value().identity.effective_configuration);
    EXPECT_EQ(first.value().identity.effective_config_id, *initial_identity);
    const auto& frozen = *first.value().identity.effective_configuration;
    EXPECT_EQ(
        frozen.at("prepared_streams")
            .at(0)
            .at("ros2_subscription")
            .at("installed_requested_qos")
            .at("reliability"),
        "1");
    bool recorded_before = false;
    bool recorded_after = false;
    for (const auto& descriptor : first.value().descriptors) {
        ASSERT_TRUE(descriptor.ros2_subscription);
        const auto& observation = *descriptor.ros2_subscription;
        if (descriptor.accepted_records != 0) {
            recorded_before = recorded_before || (observation.subscription_generation == 1 &&
                                                  observation.installed_requested_qos.reliability ==
                                                      RODAQ_ROS2_QOS_RELIABILITY_RELIABLE);
            recorded_after = recorded_after || (observation.subscription_generation == 2 &&
                                                observation.installed_requested_qos.reliability ==
                                                    RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT);
        }
    }
    EXPECT_TRUE(recorded_before);
    EXPECT_TRUE(recorded_after);
    const auto next = core->tasks().start(id, {"next"}, {true, 3});
    ASSERT_TRUE(next.ok());
    ASSERT_TRUE(completed(id, next.value()));
    const auto next_runtime = core->tasks().get_runtime_snapshot(id).value();
    ASSERT_TRUE(next_runtime.pipelines[0].facts);
    ASSERT_TRUE(next_runtime.pipelines[0].facts->effective_config_id);
    EXPECT_NE(next_runtime.pipelines[0].facts->effective_config_id, initial_identity);
    ASSERT_TRUE(connected(best_effort));
    send_best_effort();
    const auto ended = core->tasks().stop(id, {"ended"}, {true, 4});
    ASSERT_TRUE(ended.ok());
    ASSERT_TRUE(completed(id, ended.value()));
    const auto page = core->artifacts().query_artifacts({}).value();
    ASSERT_EQ(page.items.size(), 2);
    for (const auto& item : page.items) {
        if (item.segment_id != first.value().identity.segment_id) {
            const auto second = read_manifest(item.segment_id);
            ASSERT_TRUE(second.ok());
            EXPECT_EQ(
                second.value().identity.config_version, first.value().identity.config_version);
            EXPECT_NE(
                second.value().identity.effective_config_id,
                first.value().identity.effective_config_id);
            EXPECT_EQ(
                second.value()
                    .identity.effective_configuration->at("prepared_streams")
                    .at(0)
                    .at("ros2_subscription")
                    .at("installed_requested_qos")
                    .at("reliability"),
                "2");
        }
    }
}

TEST_F(Ros2Runtime, RealBoundedVolumeProtectionRecoversRos2McapWithoutAnotherUserStart) {
    const auto* selected = std::getenv("RODAQ_ROS2_RECOVERY_VOLUME");
    if (!selected) {
        GTEST_SKIP() << "Requires a dedicated tmpfs <=64MiB via RODAQ_ROS2_RECOVERY_VOLUME";
    }
    const fs::path mount(selected);
    ASSERT_TRUE(mount.is_absolute());
    ASSERT_TRUE(fs::is_directory(mount));
    ASSERT_TRUE(fs::is_empty(mount));
    struct statfs filesystem {};
    ASSERT_EQ(statfs(mount.c_str(), &filesystem), 0);
    ASSERT_EQ(filesystem.f_type, TMPFS_MAGIC);
    struct statvfs available {};
    ASSERT_EQ(statvfs(mount.c_str(), &available), 0);
    ASSERT_GT(available.f_frsize, 0u);
    ASSERT_LE(available.f_blocks, (64u * 1024 * 1024) / available.f_frsize);
    const auto total = static_cast<std::uint64_t>(available.f_blocks) * available.f_frsize;
    ASSERT_LE(total, 64u * 1024 * 1024);
    ASSERT_GE(total, 16u * 1024 * 1024);
    const auto volume = mount / root.filename();
    ASSERT_TRUE(fs::create_directory(volume));
    open_core(false, volume);
    ASSERT_TRUE(core);
    TaskConfig task;
    task.name = "Real bounded-volume automatic recovery";
    task.recovery_policy = RecoveryPolicyMode::Resume;
    task.segment_policy.max_duration = 60s;
    PipelineConfig pipeline;
    pipeline.resource_profile = "basic";
    pipeline.output_root = "local";
    pipeline.acquisition = {"com.rodaq.acquisition.ros2", "1.0.0", config().dump()};
    pipeline.storage = {"com.rodaq.storage.mcap", "1.0.0", R"({"format_version":"1.0"})"};
    pipeline.streams = {{0, "s0", {}, ReadinessPolicy::EntryReady}};
    task.pipelines.push_back(std::move(pipeline));
    const auto id = core->tasks().create_task(task, {"create"}).value();
    const auto start = core->tasks().start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(completed(id, start.value()));
    auto publisher = node->create_generic_publisher(
        "/rodaq_test_0", "std_msgs/msg/String", rclcpp::QoS(100).reliable());
    const auto record = [&] {
        const auto deadline = std::chrono::steady_clock::now() + 4s;
        do {
            publish(publisher);
            if (!core->observe().ok()) {
                return false;
            }
            const auto runtime = core->tasks().get_runtime_snapshot(id);
            if (runtime.ok() && runtime.value().pipelines[0].facts &&
                runtime.value().pipelines[0].facts->accepted_records >= 10) {
                return true;
            }
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    const auto condition = [&](ExecutionCondition expected) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            if (!core->observe().ok()) {
                return false;
            }
            const auto actual = core->tasks().get_task(id);
            if (actual.ok() && actual.value().execution_condition == expected) {
                return true;
            }
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    ASSERT_TRUE(record());
    const auto previous =
        core->tasks().get_runtime_snapshot(id).value().pipelines[0].facts->instance_id;
    ASSERT_EQ(statvfs(volume.c_str(), &available), 0);
    ASSERT_LE(available.f_bavail, available.f_blocks);
    const auto free = static_cast<std::uint64_t>(available.f_bavail) * available.f_frsize;
    constexpr std::uint64_t kRemaining = 512 * 1024;
    ASSERT_GT(free, kRemaining);
    const auto filler = volume / "bounded-test-filler";
    {
        persistence::FileDescriptor file(
            open(filler.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        ASSERT_GE(file.get(), 0);
        ASSERT_EQ(posix_fallocate(file.get(), 0, static_cast<off_t>(free - kRemaining)), 0);
        ASSERT_EQ(fsync(file.get()), 0);
    }
    ASSERT_TRUE(condition(ExecutionCondition::Protected));
    const auto protected_task = core->tasks().get_task(id).value();
    EXPECT_EQ(protected_task.desired_state, DesiredState::Running);
    EXPECT_EQ(protected_task.control_generation, 2);
    EXPECT_EQ(
        core->tasks().get_operation(id, start.value()).value().state, OperationState::Succeeded);
    auto artifacts = core->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    ASSERT_EQ(artifacts.value().items.size(), 1);
    const auto historical =
        core->artifacts().get_artifact(artifacts.value().items[0].artifact_id).value();
    EXPECT_EQ(historical.coverage[0].accepted, historical.coverage[0].written);
    EXPECT_TRUE(read_file(historical.summary.artifact_id, historical.files[0].file_id).ok());
    ASSERT_TRUE(fs::remove(filler));
    ASSERT_TRUE(condition(ExecutionCondition::Active));
    const auto recovered = core->tasks().get_runtime_snapshot(id).value();
    ASSERT_TRUE(recovered.pipelines[0].facts);
    EXPECT_NE(recovered.pipelines[0].facts->instance_id, previous);
    ASSERT_TRUE(record());
    const auto stop = core->tasks().stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(completed(id, stop.value()));
    ASSERT_TRUE(core->observe().ok());
    artifacts = core->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    ASSERT_EQ(artifacts.value().items.size(), 2);
    for (const auto& summary : artifacts.value().items) {
        const auto artifact = core->artifacts().get_artifact(summary.artifact_id).value();
        ASSERT_EQ(artifact.coverage.size(), 1);
        EXPECT_GT(artifact.coverage[0].written, 0);
        EXPECT_EQ(artifact.coverage[0].accepted, artifact.coverage[0].written);
        for (const auto& file : artifact.files) {
            if (file.display_name.find(".mcap") == std::string::npos) {
                continue;
            }
            const auto contents = read_file(summary.artifact_id, file.file_id);
            ASSERT_TRUE(contents.ok());
            const auto exported = root / (std::to_string(summary.segment_id) + "-recovery.mcap");
            {
                std::ofstream output(exported, std::ios::binary);
                output.write(
                    reinterpret_cast<const char*>(contents.value().data()),
                    static_cast<std::streamsize>(contents.value().size()));
                ASSERT_TRUE(output.good());
            }
            mcap::McapReader reader;
            ASSERT_TRUE(reader.open(exported.string()).ok());
            std::uint64_t count = 0;
            const auto expected = received_cdr(bytes());
            for (const auto& message : reader.readMessages()) {
                EXPECT_EQ(message.channel->topic, "/rodaq_test_0");
                EXPECT_EQ(message.schema->name, "std_msgs/msg/String");
                ASSERT_EQ(message.message.dataSize, expected.size());
                EXPECT_EQ(std::memcmp(message.message.data, expected.data(), expected.size()), 0);
                ++count;
            }
            EXPECT_EQ(count, artifact.coverage[0].written);
        }
    }
    EXPECT_EQ(core->tasks().list_operations(id, {}).value().items.size(), 2);
    EXPECT_TRUE(core->shutdown(std::chrono::steady_clock::now() + 5s).ok());
    core.reset();
    fs::remove_all(volume);
}

TEST_F(Ros2Runtime, FormalTaskRecordsRos2ThroughMcapCommitAndArtifactRead) {
    open_core();
    ASSERT_TRUE(core);
    auto& composition = core;
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
    auto start = composition->tasks().start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok()) << start.status().message();
    ASSERT_TRUE(completed(id, start.value()));
    std::vector<rclcpp::TopicEndpointInfo> subscriptions;
    const auto discovered_by = std::chrono::steady_clock::now() + 2s;
    do {
        subscriptions = node->get_subscriptions_info_by_topic("/rodaq_test_0");
        if (subscriptions.size() != 1) {
            std::this_thread::sleep_for(1ms);
        }
    } while (subscriptions.size() != 1 && std::chrono::steady_clock::now() < discovered_by);
    ASSERT_EQ(subscriptions.size(), 1);
    const auto initial_request = subscriptions.front().qos_profile().get_rmw_qos_profile();
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
    ASSERT_TRUE(completed(id, stop.value()));
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
    const auto manifest_path =
        root / "volume/.rodaq/artifacts" / std::to_string(artifact.value().summary.segment_id);
    const auto manifest_bytes = persistence::read_file(manifest_path, "manifest.json", 65536);
    ASSERT_TRUE(manifest_bytes.ok());
    const auto manifest = rodaq::artifact::decode_manifest(manifest_bytes.value(), {});
    ASSERT_TRUE(manifest.ok());
    ASSERT_TRUE(manifest.value().identity.effective_configuration);
    const auto& effective = *manifest.value().identity.effective_configuration;
    EXPECT_EQ(
        persistence::semantic_digest(effective, "rodaq.effective-config.v1").value(),
        manifest.value().identity.effective_config_id);
    const auto& initial = effective.at("prepared_streams").at(0);
    EXPECT_EQ(initial.at("schema_name"), "std_msgs/msg/String");
    EXPECT_EQ(initial.at("schema_encoding"), "ros2msg");
    EXPECT_EQ(
        initial.at("ros2_subscription").at("rmw_implementation"),
        rmw_get_implementation_identifier());
    EXPECT_EQ(
        initial.at("ros2_subscription").at("installed_requested_qos").at("reliability"),
        std::to_string(initial_request.reliability));
    ASSERT_FALSE(manifest.value().descriptors.empty());
    for (const auto& descriptor : manifest.value().descriptors) {
        ASSERT_TRUE(descriptor.ros2_subscription);
        EXPECT_EQ(
            descriptor.ros2_subscription->rmw_implementation, rmw_get_implementation_identifier());
        EXPECT_EQ(
            descriptor.ros2_subscription->installed_requested_qos.reliability,
            initial_request.reliability);
    }
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
                expected = received_cdr(std::move(expected));
                ASSERT_EQ(message.message.dataSize, expected.size());
                EXPECT_EQ(std::memcmp(message.message.data, expected.data(), expected.size()), 0);
                EXPECT_EQ(message.message.publishTime, message.message.logTime);
                ++messages;
            }
        }
    }
    EXPECT_EQ(messages, coverage.written);
}

class Ros2SegmentRuntime : public Ros2Runtime, public testing::WithParamInterface<bool> {};

TEST_P(Ros2SegmentRuntime, LateStreamPauseResumeAndRotationPreserveAcceptedRecordsAndSidecar) {
    open_core();
    ASSERT_TRUE(core);
    TaskConfig task;
    task.name = GetParam() ? "duration rotation" : "physical rotation";
    if (GetParam()) {
        task.segment_policy.max_duration = 80ms;
    } else {
        task.segment_policy.max_physical_bytes = 4096;
    }
    PipelineConfig pipeline;
    pipeline.resource_profile = "basic";
    pipeline.output_root = "local";
    pipeline.acquisition = {"com.rodaq.acquisition.ros2", "1.0.0", config(2).dump()};
    pipeline.storage = {"com.rodaq.storage.mcap", "1.0.0", R"({"format_version":"1.0"})"};
    pipeline.streams = {
        {0, "s0", {}, ReadinessPolicy::EntryReady}, {0, "s1", {}, ReadinessPolicy::EntryReady}};
    task.pipelines.push_back(std::move(pipeline));
    auto created = core->tasks().create_task(task, {"create"});
    ASSERT_TRUE(created.ok()) << created.status().message();
    const auto id = created.value();
    auto started = core->tasks().start(id, {"start"}, {true, 1});
    ASSERT_TRUE(started.ok());
    ASSERT_TRUE(completed(id, started.value()));

    std::array<rclcpp::GenericPublisher::SharedPtr, 2> publishers;
    publishers[0] = node->create_generic_publisher(
        "/rodaq_test_0", "std_msgs/msg/String", rclcpp::QoS(100).reliable());
    ASSERT_TRUE(connected(publishers[0]));
    struct SentPayload {
        std::string topic;
        std::vector<std::uint8_t> bytes;
    };
    std::map<std::string, SentPayload> sent;
    const auto send = [&](unsigned stream, std::string text, bool recordable) {
        std::vector<std::uint8_t> payload{0, 1, 0, 0};
        const auto length = static_cast<std::uint32_t>(text.size() + 1);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            payload.push_back(static_cast<std::uint8_t>(length >> shift));
        }
        payload.insert(payload.end(), text.begin(), text.end());
        payload.push_back(0);
        rclcpp::SerializedMessage message(payload.size());
        auto& raw = message.get_rcl_serialized_message();
        std::copy(payload.begin(), payload.end(), raw.buffer);
        raw.buffer_length = payload.size();
        publishers[stream]->publish(message);
        if (recordable) {
            sent.emplace(
                std::move(text),
                SentPayload{"/rodaq_test_" + std::to_string(stream), std::move(payload)});
        }
    };
    const auto pump = [&](unsigned phase, unsigned count, bool late_stream) {
        for (unsigned i = 0; i < count; ++i) {
            if (late_stream && i == 8) {
                publishers[1] = node->create_generic_publisher(
                    "/rodaq_test_1", "std_msgs/msg/String", rclcpp::QoS(100).reliable());
                EXPECT_TRUE(connected(publishers[1]));
            }
            for (unsigned stream = 0; stream < publishers.size(); ++stream) {
                if (publishers[stream]) {
                    send(
                        stream,
                        std::to_string(phase) + "/" + std::to_string(stream) + "/" +
                            std::to_string(i),
                        true);
                }
            }
            EXPECT_TRUE(core->observe().ok());
            std::this_thread::sleep_for(10ms);
        }
    };
    pump(1, 28, true);
    auto paused = core->tasks().pause(id, {"pause"}, {true, 2});
    ASSERT_TRUE(paused.ok());
    ASSERT_TRUE(completed(id, paused.value()));
    ASSERT_TRUE(core->observe().ok());
    auto before = core->artifacts().query_artifacts({});
    ASSERT_TRUE(before.ok());
    ASSERT_GE(before.value().items.size(), 2);
    for (unsigned i = 0; i < 5; ++i) {
        send(0, "paused/0/" + std::to_string(i), false);
        send(1, "paused/1/" + std::to_string(i), false);
    }
    auto resumed = core->tasks().resume(id, {"resume"}, {true, 3});
    ASSERT_TRUE(resumed.ok());
    ASSERT_TRUE(completed(id, resumed.value()));
    ASSERT_TRUE(connected(publishers[0]));
    ASSERT_TRUE(connected(publishers[1]));
    pump(2, 16, false);
    auto stopped = core->tasks().stop(id, {"stop"}, {true, 4});
    ASSERT_TRUE(stopped.ok());
    ASSERT_TRUE(completed(id, stopped.value()));
    ASSERT_TRUE(core->observe().ok());
    EXPECT_EQ(core->tasks().get_task(id).value().control_generation, 5);

    auto page = core->artifacts().query_artifacts({});
    ASSERT_TRUE(page.ok());
    ASSERT_GT(page.value().items.size(), before.value().items.size());
    std::sort(
        page.value().items.begin(), page.value().items.end(),
        [](const auto& a, const auto& b) { return a.segment_id < b.segment_id; });
    std::set<std::string> received;
    std::map<StreamId, std::uint64_t> last_sequence;
    std::optional<std::uint64_t> boot;
    std::optional<std::uint64_t> playback_offset;
    std::set<StreamId> received_streams;
    bool resumed_data = false;
    for (const auto& item : page.value().items) {
        auto artifact = core->artifacts().get_artifact(item.artifact_id);
        ASSERT_TRUE(artifact.ok());
        ASSERT_EQ(artifact.value().files.size(), 2);
        std::uint64_t accepted = 0;
        for (const auto& coverage : artifact.value().coverage) {
            EXPECT_EQ(coverage.accepted, coverage.written);
            EXPECT_EQ(coverage.known_failed, 0);
            EXPECT_EQ(coverage.unknown, 0);
            accepted += coverage.accepted;
        }
        std::vector<std::uint64_t> playback;
        std::string meta;
        for (const auto& file : artifact.value().files) {
            auto bytes = read_file(item.artifact_id, file.file_id);
            ASSERT_TRUE(bytes.ok());
            const auto* data = reinterpret_cast<const char*>(bytes.value().data());
            if (file.display_name.find(".rodaqmeta") != std::string::npos) {
                meta.assign(data, bytes.value().size());
                continue;
            }
            const auto path = root / "segment-read.mcap";
            {
                std::ofstream output(path, std::ios::binary);
                output.write(data, static_cast<std::streamsize>(bytes.value().size()));
                ASSERT_TRUE(output.good());
            }
            mcap::McapReader reader;
            ASSERT_TRUE(reader.open(path.string()).ok());
            for (const auto& message : reader.readMessages()) {
                ASSERT_GE(message.message.dataSize, 9);
                const auto* payload = reinterpret_cast<const std::uint8_t*>(message.message.data);
                const auto length = static_cast<std::uint32_t>(payload[4]) |
                                    (static_cast<std::uint32_t>(payload[5]) << 8) |
                                    (static_cast<std::uint32_t>(payload[6]) << 16) |
                                    (static_cast<std::uint32_t>(payload[7]) << 24);
                ASSERT_GT(length, 0);
                ASSERT_LE(length, message.message.dataSize - 8);
                EXPECT_EQ(payload[8 + length - 1], 0);
                const std::string text(reinterpret_cast<const char*>(payload + 8), length - 1);
                const auto expected = sent.find(text);
                ASSERT_NE(expected, sent.end()) << text;
                EXPECT_EQ(message.channel->topic, expected->second.topic);
                const auto expected_bytes = received_cdr(expected->second.bytes);
                ASSERT_EQ(message.message.dataSize, expected_bytes.size());
                EXPECT_EQ(std::memcmp(payload, expected_bytes.data(), expected_bytes.size()), 0);
                EXPECT_EQ(message.schema->name, "std_msgs/msg/String");
                EXPECT_TRUE(received.insert(text).second) << text;
                resumed_data = resumed_data || text.rfind("2/", 0) == 0;
                EXPECT_EQ(message.message.sequence, 0);
                EXPECT_EQ(message.message.publishTime, message.message.logTime);
                playback.push_back(message.message.logTime);
            }
        }
        ASSERT_EQ(playback.size(), accepted);
        ASSERT_GE(meta.size(), mcap_storage::kMetaHeaderBytes + mcap_storage::kMetaFooterBytes);
        auto header = mcap_storage::decode_meta_header(meta);
        auto footer = mcap_storage::decode_meta_footer(
            std::string_view(meta).substr(meta.size() - mcap_storage::kMetaFooterBytes));
        ASSERT_TRUE(header.ok());
        ASSERT_TRUE(footer.ok());
        EXPECT_EQ(footer.value().entry_count, accepted);
        EXPECT_EQ(footer.value().bytes_before_footer, meta.size() - mcap_storage::kMetaFooterBytes);
        std::size_t offset = header.value().size;
        for (std::size_t i = 0; i < playback.size(); ++i) {
            auto entry = mcap_storage::decode_meta_entry(
                std::string_view(meta).substr(offset), header.value().minor);
            ASSERT_TRUE(entry.ok());
            EXPECT_EQ(entry.value().ordinal, i);
            const auto& record = entry.value().record;
            EXPECT_GT(record.acceptance_sequence, last_sequence[record.stream_id]);
            last_sequence[record.stream_id] = record.acceptance_sequence;
            received_streams.insert(record.stream_id);
            if (!boot) {
                boot = record.system_boot_id;
            }
            EXPECT_EQ(record.system_boot_id, *boot);
            ASSERT_GE(playback[i], record.receive_monotonic);
            const auto delta = playback[i] - record.receive_monotonic;
            if (!playback_offset) {
                playback_offset = delta;
            }
            EXPECT_EQ(delta, *playback_offset);
            offset += entry.value().size;
        }
        EXPECT_EQ(offset, footer.value().bytes_before_footer);
    }
    EXPECT_EQ(received_streams.size(), 2);
    EXPECT_TRUE(resumed_data);
    EXPECT_FALSE(received.empty());
}

INSTANTIATE_TEST_SUITE_P(DurationAndPhysical, Ros2SegmentRuntime, testing::Bool());

}  // namespace rodaq::test::ros2_backend
#endif
