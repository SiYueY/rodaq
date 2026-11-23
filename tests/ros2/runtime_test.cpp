#include "runtime_fixture.hpp"

namespace rodaq::test::ros2_backend {

namespace {
struct PrefixService : acquisition::Service {
    static constexpr std::uint16_t kMinor = 0;
};
}  // namespace

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
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
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
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_STREAM_READY], 8);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE], 0);
}

TEST_F(Ros2Runtime, FailedLoadContentionCannotEraseOrMisidentifyAnotherTypeOwner) {
    std::array<std::future<Result<void>>, 8> loads;
    for (unsigned worker = 0; worker < loads.size(); ++worker) {
        loads[worker] = std::async(std::launch::async, [worker]() -> Result<void> {
            const auto* type =
                worker % 2 == 0 ? "std_msgs/msg/String" : "builtin_interfaces/msg/Time";
            const auto* package = worker % 2 == 0 ? "libstd_msgs__" : "libbuiltin_interfaces__";
            for (unsigned attempt = 0; attempt < 12; ++attempt) {
                const auto absent = "rodaq_missing_" + std::to_string(worker) + "_" +
                                    std::to_string(attempt) + "/msg/Absent";
                auto failed = ros2_acquisition::resident_typesupport(absent, soon());
                if (failed.status().code() != StatusCode::Unavailable) {
                    return Status(StatusCode::Internal, "Failed load lost reservation ownership");
                }
                auto valid = ros2_acquisition::resident_typesupport(type, soon());
                if (!valid.ok()) {
                    return valid.status();
                }
                if (valid.value()->get_library_path().find(package) == std::string::npos) {
                    return Status(StatusCode::DataLoss, "Resident type references another package");
                }
            }
            return {};
        });
    }
    for (auto& loading : loads) {
        auto result = loading.get();
        EXPECT_TRUE(result.ok()) << result.status().message();
    }
}

TEST_F(Ros2Runtime, LocalDescriptorCutReportsDiscardedNativeCallbacksAsCoverageEvidence) {
    create(config());
    ASSERT_TRUE(handle);
    ASSERT_TRUE(prepare().ok());
    ASSERT_TRUE(handle->start(soon()).ok());
    {
        std::lock_guard<std::mutex> lock(receiver->mutex);
        receiver->block_descriptor = true;
    }
    auto publisher = node->create_generic_publisher(
        "/rodaq_test_0", "std_msgs/msg/String", rclcpp::QoS(100).reliable());
    const bool cutting = receiver->wait([&] { return receiver->descriptor_active; });
    if (!cutting) {
        receiver->release();
    }
    ASSERT_TRUE(cutting);
    ASSERT_TRUE(connected(publisher));
    for (unsigned i = 0; i < 20; ++i) {
        publish(publisher);
        std::this_thread::sleep_for(2ms);
    }
    receiver->release();
    ASSERT_TRUE(receiver->wait([&] { return receiver->dropped != 0; }));
    ASSERT_TRUE(handle->quiesce(soon()).ok());
    std::lock_guard<std::mutex> lock(receiver->mutex);
    EXPECT_EQ(receiver->events[RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE], 0);
}

TEST_F(Ros2Runtime, AuditBackendRejectsServiceZeroBeforeCreatingNativeSubscriptions) {
    plugin::ServiceRegistry prefix;
    ASSERT_TRUE(prefix.add_shared<PrefixService>(receiver).ok());
    ASSERT_TRUE(prefix.freeze().ok());
    const auto rejected = library->create_instance(prefix, config().dump());
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.status().code(), StatusCode::Unavailable);
    const auto settled_by = std::chrono::steady_clock::now() + 2s;
    while (!manager->initialization_cleanup_settled() &&
           std::chrono::steady_clock::now() < settled_by) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(manager->initialization_cleanup_settled());
    EXPECT_TRUE(node->get_subscriptions_info_by_topic("/rodaq_test_0").empty());
    create(config());
    ASSERT_TRUE(handle);
    ASSERT_TRUE(prepare().ok());
    std::lock_guard<std::mutex> lock(receiver->mutex);
    ASSERT_EQ(receiver->subscriptions.size(), 1);
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
    ASSERT_TRUE(receiver->wait([&] { return receiver->subscriptions.size() >= 2; }));
    const auto endpoints = node->get_subscriptions_info_by_topic("/rodaq_test_0");
    ASSERT_EQ(endpoints.size(), 1);
    EXPECT_EQ(
        endpoints[0].qos_profile().get_rmw_qos_profile().reliability,
        RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT);
    ASSERT_TRUE(handle->quiesce(soon()).ok());
    std::lock_guard<std::mutex> lock(receiver->mutex);
    ASSERT_GE(receiver->subscriptions.size(), 2);
    for (const auto& observation : receiver->subscriptions) {
        EXPECT_EQ(observation.rmw, rmw_get_implementation_identifier());
        EXPECT_EQ(observation.requested.reliability, RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT);
        EXPECT_EQ(observation.generation, 1);
        EXPECT_EQ(observation.threads, config().at("executor_threads").get<unsigned>());
#if RCLCPP_VERSION_MAJOR == 16
        EXPECT_EQ(observation.distribution, "humble");
#else
        EXPECT_EQ(observation.distribution, "jazzy");
#endif
    }
    ASSERT_FALSE(receiver->payloads.empty());
    for (const auto& payload : receiver->payloads) {
        EXPECT_EQ(payload, received_cdr(bytes()));
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
    ASSERT_GE(receiver->subscriptions.size(), 2);
    EXPECT_EQ(receiver->subscriptions.front().generation, 1);
    EXPECT_EQ(
        receiver->subscriptions.front().requested.reliability, RODAQ_ROS2_QOS_RELIABILITY_RELIABLE);
    EXPECT_EQ(receiver->subscriptions.back().generation, 2);
    EXPECT_EQ(
        receiver->subscriptions.back().requested.reliability,
        RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT);
}

}  // namespace rodaq::test::ros2_backend
