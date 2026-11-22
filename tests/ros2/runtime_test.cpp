#include "runtime_fixture.hpp"

namespace rodaq::test::ros2_backend {

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

}  // namespace rodaq::test::ros2_backend
