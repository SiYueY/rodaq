#include "runtime_fixture.hpp"

#ifdef RODAQ_MCAP_PACKAGE
#include <map>
#include <set>

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
