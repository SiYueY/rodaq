#include "runtime_fixture.hpp"

#ifdef RODAQ_MCAP_PACKAGE
#include <map>
#include <set>

#include "persistence/atomic_file.hpp"

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
    ASSERT_EQ(page.value().items.size(), 1U);
    auto artifact = core->artifacts().get_artifact(page.value().items[0].artifact_id);
    ASSERT_TRUE(artifact.ok());
    ASSERT_EQ(artifact.value().files.size(), 1U);
    ASSERT_EQ(artifact.value().coverage.size(), 1U);
    EXPECT_GT(artifact.value().coverage[0].written, 0ULL);
    EXPECT_EQ(artifact.value().coverage[0].accepted, artifact.value().coverage[0].written);

    auto bytes = read_file(artifact.value().summary.artifact_id, artifact.value().files[0].file_id);
    ASSERT_TRUE(bytes.ok());
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
    std::uint64_t count = 0;
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
    EXPECT_EQ(count, artifact.value().coverage[0].written);
}

TEST_F(Ros2Runtime, FormalTaskRecordsRos2ThroughSingleFileMcapCommitAndArtifactRead) {
    open_core();
    ASSERT_TRUE(core);
    rodaq::TaskConfig task;
    task.name = "ROS2 MCAP actual Core integration";
    task.segment_policy.max_duration = 60s;
    rodaq::PipelineConfig pipeline;
    pipeline.resource_profile = "basic";
    pipeline.output_root = "local";
    pipeline.acquisition = {"com.rodaq.acquisition.ros2", "1.0.0", config().dump()};
    pipeline.storage = {"com.rodaq.storage.mcap", "1.0.0", R"({"format_version":"1.0"})"};
    pipeline.streams = {{0, "s0", {}, rodaq::ReadinessPolicy::EntryReady}};
    task.pipelines.push_back(std::move(pipeline));
    auto created = core->tasks().create_task(task, {"create"});
    ASSERT_TRUE(created.ok()) << created.status().message();
    const auto id = created.value();
    auto start = core->tasks().start(id, {"start"}, {true, 1});
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
    ASSERT_EQ(subscriptions.size(), 1U);
    const auto initial_request = subscriptions.front().qos_profile().get_rmw_qos_profile();

    auto publisher = node->create_generic_publisher(
        "/rodaq_test_0", "std_msgs/msg/String", rclcpp::QoS(100).reliable());
    ASSERT_TRUE(connected(publisher));
    for (unsigned i = 0; i < 30; ++i) {
        publish(publisher);
        std::this_thread::sleep_for(10ms);
    }

    auto stop = core->tasks().stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(completed(id, stop.value()));
    ASSERT_TRUE(core->observe().ok());

    auto artifacts = core->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    ASSERT_EQ(artifacts.value().items.size(), 1U);
    auto artifact = core->artifacts().get_artifact(artifacts.value().items[0].artifact_id);
    ASSERT_TRUE(artifact.ok());
    ASSERT_EQ(artifact.value().files.size(), 1U);
    EXPECT_NE(artifact.value().files[0].display_name.find(".mcap"), std::string::npos);
    ASSERT_EQ(artifact.value().coverage.size(), 1U);
    const auto& coverage = artifact.value().coverage[0];
    EXPECT_GT(coverage.accepted, 0ULL);
    EXPECT_EQ(coverage.accepted, coverage.written);
    EXPECT_EQ(coverage.known_failed, 0ULL);
    EXPECT_EQ(coverage.unknown, 0ULL);

    const auto manifest_path =
        root / "volume/.rodaq/artifacts" / std::to_string(artifact.value().summary.segment_id);
    const auto manifest_bytes = persistence::read_file(manifest_path, "manifest.json", 65536);
    ASSERT_TRUE(manifest_bytes.ok());
    const auto manifest = rodaq::artifact::decode_manifest(manifest_bytes.value(), {});
    ASSERT_TRUE(manifest.ok());
    ASSERT_FALSE(manifest.value().descriptors.empty());
    for (const auto& descriptor : manifest.value().descriptors) {
        ASSERT_EQ(descriptor.metadata_format, "ros2/1.0");
        const auto meta = nlohmann::json::parse(descriptor.metadata_json);
        ASSERT_TRUE(meta.contains("subscription"));
        EXPECT_EQ(
            meta["subscription"]["rmw_implementation"].get<std::string>(),
            rmw_get_implementation_identifier());
        EXPECT_EQ(
            meta["subscription"]["installed_requested_qos"]["reliability"].get<std::uint32_t>(),
            initial_request.reliability);
    }

    auto bytes_res =
        read_file(artifact.value().summary.artifact_id, artifact.value().files[0].file_id);
    ASSERT_TRUE(bytes_res.ok());
    const auto exported = root / "read-back.mcap";
    {
        std::ofstream stream(exported, std::ios::binary);
        stream.write(
            reinterpret_cast<const char*>(bytes_res.value().data()),
            static_cast<std::streamsize>(bytes_res.value().size()));
        ASSERT_TRUE(stream.good());
    }
    mcap::McapReader reader;
    ASSERT_TRUE(reader.open(exported.string()).ok());
    std::uint64_t messages = 0;
    std::uint32_t expected_seq = 1;
    for (const auto& message : reader.readMessages()) {
        EXPECT_EQ(message.channel->topic, "/rodaq_test_0");
        EXPECT_EQ(message.channel->messageEncoding, "cdr");
        EXPECT_EQ(message.schema->name, "std_msgs/msg/String");
        const auto expected = received_cdr(bytes());
        ASSERT_EQ(message.message.dataSize, expected.size());
        EXPECT_EQ(std::memcmp(message.message.data, expected.data(), expected.size()), 0);
        EXPECT_EQ(message.message.sequence, expected_seq++);
        EXPECT_EQ(message.message.publishTime, message.message.logTime);
        ++messages;
    }
    EXPECT_EQ(messages, coverage.written);
}

class Ros2SegmentRuntime : public Ros2Runtime, public testing::WithParamInterface<bool> {};

TEST_P(Ros2SegmentRuntime, LateStreamPauseResumeAndRotationPreserveAcceptedRecordsInSingleMcap) {
    open_core();
    ASSERT_TRUE(core);
    TaskConfig task;
    task.name = GetParam() ? "duration rotation" : "physical rotation";
    if (GetParam()) {
        task.segment_policy.max_duration = 80ms;
    } else {
        task.segment_policy.max_physical_bytes = 1024;
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
    ASSERT_GE(before.value().items.size(), 2U);
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
    EXPECT_EQ(core->tasks().get_task(id).value().control_generation, 5ULL);

    auto page = core->artifacts().query_artifacts({});
    ASSERT_TRUE(page.ok());
    ASSERT_GT(page.value().items.size(), before.value().items.size());
    std::set<std::string> received;
    bool resumed_data = false;
    for (const auto& item : page.value().items) {
        auto artifact = core->artifacts().get_artifact(item.artifact_id);
        ASSERT_TRUE(artifact.ok());
        ASSERT_EQ(artifact.value().files.size(), 1U);
        std::uint64_t accepted = 0;
        for (const auto& coverage : artifact.value().coverage) {
            EXPECT_EQ(coverage.accepted, coverage.written);
            EXPECT_EQ(coverage.known_failed, 0ULL);
            EXPECT_EQ(coverage.unknown, 0ULL);
            accepted += coverage.accepted;
        }
        auto bytes_res = read_file(item.artifact_id, artifact.value().files[0].file_id);
        ASSERT_TRUE(bytes_res.ok());
        const auto path = root / "segment-read.mcap";
        {
            std::ofstream output(path, std::ios::binary);
            output.write(
                reinterpret_cast<const char*>(bytes_res.value().data()),
                static_cast<std::streamsize>(bytes_res.value().size()));
            ASSERT_TRUE(output.good());
        }
        mcap::McapReader reader;
        ASSERT_TRUE(reader.open(path.string()).ok());
        std::uint64_t decoded_count = 0;
        std::map<mcap::ChannelId, std::uint32_t> channel_seq;
        for (const auto& message : reader.readMessages()) {
            ASSERT_GE(message.message.dataSize, 9U);
            const auto* payload = reinterpret_cast<const std::uint8_t*>(message.message.data);
            const auto length = static_cast<std::uint32_t>(payload[4]) |
                                (static_cast<std::uint32_t>(payload[5]) << 8) |
                                (static_cast<std::uint32_t>(payload[6]) << 16) |
                                (static_cast<std::uint32_t>(payload[7]) << 24);
            ASSERT_GT(length, 0U);
            ASSERT_LE(length, message.message.dataSize - 8);
            const std::string text(reinterpret_cast<const char*>(payload + 8), length - 1);
            const auto expected = sent.find(text);
            ASSERT_NE(expected, sent.end()) << text;
            EXPECT_EQ(message.channel->topic, expected->second.topic);
            EXPECT_TRUE(received.insert(text).second) << text;
            resumed_data = resumed_data || text.rfind("2/", 0) == 0;
            EXPECT_EQ(message.message.sequence, ++channel_seq[message.message.channelId]);
            EXPECT_EQ(message.message.publishTime, message.message.logTime);
            ++decoded_count;
        }
        EXPECT_EQ(decoded_count, accepted);
    }
    EXPECT_TRUE(resumed_data);
    EXPECT_FALSE(received.empty());
}

INSTANTIATE_TEST_SUITE_P(DurationAndPhysical, Ros2SegmentRuntime, testing::Bool());

}  // namespace rodaq::test::ros2_backend
#endif
