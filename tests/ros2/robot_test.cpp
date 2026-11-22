#include "runtime_fixture.hpp"

#ifdef RODAQ_MCAP_PACKAGE
#include <map>
#include <set>
#include <stdexcept>

#include <openssl/sha.h>
#include <rclcpp/serialization.hpp>
#include <rmw/rmw.h>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

namespace rodaq::test::ros2_backend {
namespace {

using Digest = std::array<unsigned char, SHA256_DIGEST_LENGTH>;

Digest digest(const void* data, std::size_t size) {
    Digest result{};
    if (SHA256(static_cast<const unsigned char*>(data), size, result.data()) == nullptr) {
        throw std::runtime_error("Robot payload digest failed");
    }
    return result;
}

// Test producer owns exactly two native execution frames and at most 1024
// digest receipts. It retains no camera payload after synchronous publication.
class RobotPublisher {
public:
    explicit RobotPublisher(rclcpp::Node::SharedPtr node) : node_(std::move(node)) {}
    RobotPublisher(const RobotPublisher&) = delete;
    RobotPublisher& operator=(const RobotPublisher&) = delete;

    ~RobotPublisher() { stop(); }

    void start() {
        camera_ = std::thread([this] { run(true); });
        joints_ = std::thread([this] { run(false); });
    }

    void stop() {
        stopping_.store(true);
        if (camera_.joinable()) {
            camera_.join();
        }
        if (joints_.joinable()) {
            joints_.join();
        }
    }

    // Both inspection methods require stop() to have joined the publishers.
    void rethrow_error() const {
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

    const std::map<std::pair<bool, std::int32_t>, Digest>& receipts() const { return receipts_; }

private:
    template <typename Message>
    void publish(
        const rclcpp::GenericPublisher::SharedPtr& publisher, const Message& message, bool camera) {
        rclcpp::Serialization<Message> serializer;
        rclcpp::SerializedMessage serialized;
        serializer.serialize_message(&message, &serialized);
        const auto& raw = serialized.get_rcl_serialized_message();
        const auto hash = digest(raw.buffer, raw.buffer_length);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            receipts_.emplace(std::make_pair(camera, message.header.stamp.sec), hash);
        }
        publisher->publish(serialized);
    }

    void run(bool camera) noexcept {
        try {
            const auto period = camera ? 66666667ns : 10ms;
            if (!camera) {
                std::this_thread::sleep_for(200ms);
            }
            if (stopping_.load()) {
                return;
            }
            auto publisher = node_->create_generic_publisher(
                camera ? "/rodaq_test_0" : "/rodaq_test_1",
                camera ? "sensor_msgs/msg/Image" : "sensor_msgs/msg/JointState",
                rclcpp::QoS(4).reliable());
            sensor_msgs::msg::Image image;
            image.height = 480;
            image.width = 640;
            image.encoding = "rgb8";
            image.step = 640 * 3;
            image.header.frame_id = "camera";
            if (camera) {
                image.data.resize(480 * 640 * 3);
            }
            sensor_msgs::msg::JointState joint;
            joint.header.frame_id = "joints";
            for (unsigned i = 0; i < 12; ++i) {
                joint.name.push_back("joint_" + std::to_string(i));
                joint.position.push_back(0);
                joint.velocity.push_back(i * 0.01);
                joint.effort.push_back(-static_cast<double>(i) * 0.125);
            }
            auto next = std::chrono::steady_clock::now();
            for (std::int32_t sequence = 1; sequence <= 512 && !stopping_.load(); ++sequence) {
                if (camera) {
                    image.header.stamp.sec = sequence;
                    for (std::size_t i = 0; i < image.data.size(); ++i) {
                        image.data[i] = static_cast<std::uint8_t>(sequence + i % 251);
                    }
                    publish(publisher, image, true);
                } else {
                    joint.header.stamp.sec = sequence;
                    for (std::size_t i = 0; i < joint.position.size(); ++i) {
                        joint.position[i] = sequence + static_cast<double>(i) * 0.25;
                    }
                    publish(publisher, joint, false);
                }
                next += period;
                std::this_thread::sleep_until(next);
            }
            if (!stopping_.load()) {
                throw std::runtime_error("Robot publisher receipt budget exhausted");
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = std::current_exception();
            stopping_.store(true);
        }
    }

    rclcpp::Node::SharedPtr node_;
    std::atomic<bool> stopping_{false};
    std::thread camera_;
    std::thread joints_;
    std::mutex mutex_;
    std::exception_ptr error_;
    std::map<std::pair<bool, std::int32_t>, Digest> receipts_;
};

}  // namespace

TEST_F(Ros2Runtime, RepresentativeCameraAndLateJointsSurviveRotationPauseResumeAndCommit) {
    open_core(true);
    ASSERT_TRUE(core);
    auto source = config(2);
    source["streams"][0]["type"] = "sensor_msgs/msg/Image";
    source["streams"][1]["type"] = "sensor_msgs/msg/JointState";
    for (auto& stream : source["streams"]) {
        stream["qos"]["depth"] = "4";
    }
    TaskConfig task;
    task.name = "640x480 RGB 15Hz and 12 joint 100Hz";
    task.segment_policy.max_duration = 250ms;
    task.segment_policy.max_physical_bytes = 4 * 1024 * 1024;
    PipelineConfig pipeline;
    pipeline.resource_profile = "basic";
    pipeline.output_root = "local";
    pipeline.acquisition = {"com.rodaq.acquisition.ros2", "1.0.0", source.dump()};
    pipeline.storage = {"com.rodaq.storage.mcap", "1.0.0", R"({"format_version":"1.0"})"};
    pipeline.streams = {
        {0, "s0", {}, ReadinessPolicy::EntryReady}, {0, "s1", {}, ReadinessPolicy::EntryReady}};
    task.pipelines.push_back(std::move(pipeline));
    auto created = core->tasks().create_task(task, {"create"});
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    auto start = core->tasks().start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(completed(id, start.value()));
    RobotPublisher producer(node);
    producer.start();
    const auto run_for = [&](std::chrono::milliseconds duration) {
        const auto end = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < end) {
            EXPECT_TRUE(core->observe().ok());
            std::this_thread::sleep_for(10ms);
        }
    };
    run_for(600ms);
    auto pause = core->tasks().pause(id, {"pause"}, {true, 2});
    ASSERT_TRUE(pause.ok());
    ASSERT_TRUE(completed(id, pause.value()));
    run_for(200ms);
    auto resume = core->tasks().resume(id, {"resume"}, {true, 3});
    ASSERT_TRUE(resume.ok());
    ASSERT_TRUE(completed(id, resume.value()));
    run_for(600ms);
    auto stop = core->tasks().stop(id, {"stop"}, {true, 4});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(completed(id, stop.value()));
    producer.stop();
    producer.rethrow_error();
    ASSERT_TRUE(core->observe().ok());
    auto artifacts = core->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    ASSERT_GE(artifacts.value().items.size(), 3);
    std::set<std::pair<bool, std::int32_t>> received;
    std::uint64_t camera_count = 0;
    std::uint64_t joint_count = 0;
    for (const auto& summary : artifacts.value().items) {
        auto artifact = core->artifacts().get_artifact(summary.artifact_id);
        ASSERT_TRUE(artifact.ok());
        std::uint64_t accepted = 0;
        for (const auto& coverage : artifact.value().coverage) {
            EXPECT_EQ(coverage.accepted, coverage.written);
            EXPECT_EQ(coverage.known_failed, 0);
            EXPECT_EQ(coverage.unknown, 0);
            accepted += coverage.accepted;
        }
        std::uint64_t messages = 0;
        for (const auto& file : artifact.value().files) {
            auto bytes = read_file(summary.artifact_id, file.file_id);
            ASSERT_TRUE(bytes.ok());
            if (const auto* destination = std::getenv("RODAQ_ROS2_EVIDENCE_DIR")) {
                const fs::path directory(destination);
                ASSERT_TRUE(directory.is_absolute() && fs::is_directory(directory));
                ASSERT_EQ(fs::path(file.display_name).filename().string(), file.display_name);
                std::ofstream evidence(directory / file.display_name, std::ios::binary);
                evidence.write(
                    reinterpret_cast<const char*>(bytes.value().data()),
                    static_cast<std::streamsize>(bytes.value().size()));
                ASSERT_TRUE(evidence.good());
            }
            if (file.display_name.find(".mcap") == std::string::npos) {
                continue;
            }
            const auto path = root / "robot-read.mcap";
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
                const bool camera = message.channel->topic == "/rodaq_test_0";
                rclcpp::SerializedMessage serialized(message.message.dataSize);
                auto& raw = serialized.get_rcl_serialized_message();
                std::memcpy(raw.buffer, message.message.data, message.message.dataSize);
                raw.buffer_length = message.message.dataSize;
                std::int32_t sequence;
                if (camera) {
                    sensor_msgs::msg::Image image;
                    rclcpp::Serialization<sensor_msgs::msg::Image>().deserialize_message(
                        &serialized, &image);
                    sequence = image.header.stamp.sec;
                    EXPECT_EQ(message.schema->name, "sensor_msgs/msg/Image");
                    EXPECT_EQ(image.width, 640);
                    EXPECT_EQ(image.height, 480);
                    EXPECT_EQ(image.data.size(), 640 * 480 * 3);
                    ++camera_count;
                } else {
                    EXPECT_EQ(message.channel->topic, "/rodaq_test_1");
                    sensor_msgs::msg::JointState joint;
                    rclcpp::Serialization<sensor_msgs::msg::JointState>().deserialize_message(
                        &serialized, &joint);
                    sequence = joint.header.stamp.sec;
                    EXPECT_EQ(message.schema->name, "sensor_msgs/msg/JointState");
                    ASSERT_EQ(joint.position.size(), 12);
                    for (std::size_t i = 0; i < joint.position.size(); ++i) {
                        EXPECT_EQ(joint.position[i], sequence + static_cast<double>(i) * 0.25);
                    }
                    ++joint_count;
                }
                const auto key = std::make_pair(camera, sequence);
                const auto expected = producer.receipts().find(key);
                ASSERT_NE(expected, producer.receipts().end());
                EXPECT_EQ(digest(message.message.data, message.message.dataSize), expected->second);
                EXPECT_TRUE(received.insert(key).second);
                ++messages;
            }
        }
        EXPECT_EQ(messages, accepted);
    }
    EXPECT_GE(camera_count, 8);
    EXPECT_GE(joint_count, 50);
    if (const auto* destination = std::getenv("RODAQ_ROS2_EVIDENCE_DIR")) {
        nlohmann::json receipts = nlohmann::json::array();
        constexpr char hex[] = "0123456789abcdef";
        for (const auto& receipt : producer.receipts()) {
            std::string hash;
            for (const auto byte : receipt.second) {
                hash.push_back(hex[byte >> 4]);
                hash.push_back(hex[byte & 15]);
            }
            receipts.push_back(
                {{"camera", receipt.first.first},
                 {"sequence", receipt.first.second},
                 {"sha256", hash}});
        }
        nlohmann::json report{
            {"format_version", "1.0"},
            {"camera_recorded", camera_count},
            {"joints_recorded", joint_count},
            {"artifact_count", artifacts.value().items.size()},
            {"rclcpp_major", RCLCPP_VERSION_MAJOR},
            {"rmw", rmw_get_implementation_identifier()},
            {"source_receipts", std::move(receipts)}};
        std::ofstream output(fs::path(destination) / "robot-source.json");
        output << report.dump(2) << '\n';
        ASSERT_TRUE(output.good());
    }
}

}  // namespace rodaq::test::ros2_backend
#endif
