#include "runtime_fixture.hpp"

namespace rodaq::test::ros2_backend {

TEST_F(Ros2Runtime, ActualContextReleaseBreaksNativeSubcontextOwnershipAfterShutdown) {
    auto owned = std::make_shared<ros2_acquisition::Context>();
    rclcpp::InitOptions options;
    options.set_domain_id(domain_id);
    options.auto_initialize_logging(false);
    owned->init(0, nullptr, options);
    auto participant = std::make_shared<rclcpp::Node>(
        "rodaq_context_owner", rclcpp::NodeOptions().context(owned).enable_rosout(false));
    EXPECT_EQ(owned->release_after_join().status().code(), StatusCode::InvalidState);
    EXPECT_TRUE(owned->is_valid());
    participant.reset();
    EXPECT_TRUE(owned->shutdown("owner regression"));
    EXPECT_TRUE(owned->release_after_join().ok());
    std::weak_ptr<rclcpp::Context> weak = owned;
    owned.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_TRUE(context->is_valid());
}

TEST_F(Ros2Runtime, ConcurrentResidentTypeLoadsShareActualOwnerAndFailuresDoNotPoisonFutureLoads) {
    std::array<std::future<Result<std::shared_ptr<rcpputils::SharedLibrary>>>, 8> loads;
    for (auto& loading : loads) {
        loading = std::async(std::launch::async, [] {
            return ros2_acquisition::resident_typesupport("std_msgs/msg/String", soon());
        });
    }
    std::shared_ptr<rcpputils::SharedLibrary> first;
    for (auto& loading : loads) {
        auto loaded = loading.get();
        ASSERT_TRUE(loaded.ok()) << loaded.status().message();
        if (!first) {
            first = loaded.value();
        }
        EXPECT_EQ(loaded.value(), first);
    }
    EXPECT_EQ(
        ros2_acquisition::resident_typesupport("rodaq_missing_package/msg/Absent", soon())
            .status()
            .code(),
        StatusCode::Unavailable);
    auto again = ros2_acquisition::resident_typesupport("std_msgs/msg/String", soon());
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value(), first);
}

TEST_F(Ros2Runtime, RepeatedActualPluginLifecyclesReleasePrivateContextsAndReuseTypeSupport) {
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
        create(config());
        ASSERT_TRUE(handle);
        ASSERT_TRUE(prepare().ok());
        ASSERT_TRUE(handle->start(soon()).ok());
        ASSERT_TRUE(handle->quiesce(soon()).ok());
        handle.reset();
        ASSERT_TRUE(instance->close(soon()).ok());
        instance.reset();
    }
    EXPECT_TRUE(context->is_valid());
    std::lock_guard<std::mutex> lock(receiver->mutex);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 4U);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE], 0U);
}

TEST_F(Ros2Runtime, PrepareBindsActualInstalledQosAndRuntimeIncompatibleChangeFailsClosed) {
    // IC-D01 + B1 bind-once: Prepare binds actual installed QoS once; an
    // incompatible runtime QoS change (reliable -> best_effort publisher) must
    // NOT perform a runtime Descriptor Cut and instead fails closed with a
    // TERMINAL_FAILURE event.
    auto reliable = node->create_generic_publisher(
        "/rodaq_test_0", "std_msgs/msg/String", rclcpp::QoS(10).reliable());
    create(config(1, false));
    ASSERT_TRUE(handle);
    ASSERT_TRUE(prepare().ok());
    {
        std::lock_guard<std::mutex> lock(receiver->mutex);
        ASSERT_EQ(receiver->subscriptions.size(), 1U);
        EXPECT_EQ(
            receiver->subscriptions[0].requested.reliability, RODAQ_ROS2_QOS_RELIABILITY_RELIABLE);
    }
    ASSERT_TRUE(handle->start(soon()).ok());
    ASSERT_TRUE(connected(reliable));
    ASSERT_TRUE(publish_until(reliable, [&] { return !receiver->payloads.empty(); }));

    // Introduce incompatible best-effort publisher at runtime -> bind-once fails closed.
    auto best_effort = node->create_generic_publisher(
        "/rodaq_test_0", "std_msgs/msg/String", rclcpp::QoS(10).best_effort());
    const bool terminal = receiver->wait(
        [&] { return receiver->events[RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE] > 0; }, 3s);
    EXPECT_TRUE(terminal);
    ASSERT_TRUE(handle->quiesce(soon()).ok());
    std::lock_guard<std::mutex> lock(receiver->mutex);
    // Must remain bind-once (no second descriptor proposed at runtime!).
    EXPECT_EQ(receiver->subscriptions.size(), 1U);
}

TEST_F(Ros2Runtime, IdleExplicitTypeIsLocallyReadyAndPrivateShutdownPreservesOtherContext) {
    create(config());
    ASSERT_TRUE(handle);
    auto prepared = prepare();
    ASSERT_TRUE(prepared.ok()) << prepared.status().message();
    ASSERT_TRUE(handle->start(soon()).ok());
    ASSERT_TRUE(handle->quiesce(soon()).ok());
    EXPECT_TRUE(context->is_valid());
    std::lock_guard<std::mutex> lock(receiver->mutex);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 1U);
    EXPECT_TRUE(receiver->payloads.empty());
    ASSERT_EQ(receiver->schemas.size(), 1U);
    EXPECT_NE(receiver->schemas[0].find("string data\n"), std::string::npos);
}

TEST_F(Ros2Runtime, MissingDiscoveredTypeFailsWithoutClaimingReady) {
    create(config(1, false));
    ASSERT_TRUE(handle);
    EXPECT_EQ(
        prepare(1, 1, plugin::Deadline::after(50ms).value()).status().code(),
        StatusCode::Unavailable);
    std::lock_guard<std::mutex> lock(receiver->mutex);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 0U);
}

TEST_F(Ros2Runtime, DuplicateBindingIdentityIsRejectedBeforeAnyRosResourceReadiness) {
    create(config(2));
    ASSERT_TRUE(handle);
    const std::array<acquisition::StreamBinding, 2> dup{
        {{{sizeof(acquisition::StreamBinding)}, 1, {"s0", 2}, 1048576},
         {{sizeof(acquisition::StreamBinding)}, 1, {"s1", 2}, 1048576}}};
    const acquisition::PrepareConfig env{{sizeof(env)}, dup.data(), 2, 1};
    EXPECT_EQ(
        handle->prepare(env, receiver->table(), soon()).status().code(),
        StatusCode::InvalidArgument);
    std::lock_guard<std::mutex> lock(receiver->mutex);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 0U);
}

}  // namespace rodaq::test::ros2_backend
