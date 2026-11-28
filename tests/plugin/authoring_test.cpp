#include <rodaq/plugin/authoring.hpp>

#include "abi/counter.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string_view>

namespace {

class Counter {
public:
    rodaq::Result<std::int64_t> increment(std::int64_t delta) {
        if (delta == -1) {
            throw std::bad_alloc();
        }
        if (delta == -2) {
            throw std::runtime_error("provider failure");
        }
        return delta;
    }

    rodaq::Result<std::int64_t> current() const { return 0; }
};

class TestPlugin final : public rodaq::plugin::PluginBase {
public:
    rodaq::Result<void> configure(rodaq::plugin::PluginSetup& setup) override {
        if (setup.config() == "throw") {
            throw std::runtime_error("configure failure");
        }
        auto result = setup.provide<CounterInterface>(counter_);
        if (!result.ok()) {
            return result;
        }
        if (setup.config() == "duplicate") {
            return setup.provide<CounterInterface>(counter_);
        }
        if (setup.config() == "fail") {
            return rodaq::Status(rodaq::StatusCode::Unavailable, "configure unavailable");
        }
        return {};
    }

    rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override { return {}; }

private:
    Counter counter_;
};

class AllocationFailure final : public rodaq::plugin::PluginBase {
public:
    AllocationFailure() { throw std::bad_alloc(); }

    rodaq::Result<void> configure(rodaq::plugin::PluginSetup&) override { return {}; }

    rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override { return {}; }
};

class AuthoringTest : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(
            rodaq::plugin::authoring::Instance<TestPlugin>::create(&instance_, &error_),
            RODAQ_STATUS_OK);
    }

    void TearDown() override {
        ASSERT_EQ(
            instance_.vtable->shutdown(instance_.context, {RODAQ_DEADLINE_INFINITE}, &error_),
            RODAQ_STATUS_OK);
        instance_.vtable->destroy(instance_.context);
    }

    rodaq_status_code_t configure(std::string_view text) {
        const rodaq_config_t config{
            {sizeof(rodaq_config_t)}, {text.data(), static_cast<std::uint32_t>(text.size())}};
        return instance_.vtable->configure(instance_.context, &app_callbacks_, &config, &error_);
    }

    rodaq_plugin_instance_t instance_{};
    std::array<std::uint8_t, 64> error_buffer_{};
    rodaq_error_t error_{
        {error_buffer_.data(), static_cast<std::uint32_t>(error_buffer_.size()), 0}};
    const rodaq_application_callbacks_t app_callbacks_{
        {sizeof(rodaq_application_callbacks_t)}, nullptr, nullptr};
};

TEST_F(AuthoringTest, ConfigureQueryAndSingleConfigureContract) {
    ASSERT_EQ(configure({}), RODAQ_STATUS_OK);
    const rodaq_interface_query_t query{
        {RODAQ_COUNTER_INTERFACE_ID, sizeof(RODAQ_COUNTER_INTERFACE_ID) - 1}, 1, 0, 99};
    rodaq_interface_reference_t reference{};
    ASSERT_EQ(
        instance_.vtable->query(instance_.context, &query, &reference, &error_), RODAQ_STATUS_OK);
    EXPECT_EQ(reference.negotiated_version.minor, 0);
    CounterInterface::Vtable table{};
    ASSERT_TRUE(rodaq::plugin::abi::copy_vtable<CounterInterface>(reference, table));
    std::int64_t value = 0;
    EXPECT_EQ(table.increment(reference.context, 9, &value, &error_), RODAQ_STATUS_OK);
    EXPECT_EQ(value, 9);
    EXPECT_EQ(configure({}), RODAQ_STATUS_INVALID_STATE);
}

TEST_F(AuthoringTest, FailedOrThrowingConfigurationPublishesNoRegistry) {
    EXPECT_EQ(configure("duplicate"), RODAQ_STATUS_CONFLICT);
    const rodaq_interface_query_t query{
        {RODAQ_COUNTER_INTERFACE_ID, sizeof(RODAQ_COUNTER_INTERFACE_ID) - 1}, 1, 0, 0};
    rodaq_interface_reference_t reference{};
    EXPECT_EQ(
        instance_.vtable->query(instance_.context, &query, &reference, &error_),
        RODAQ_STATUS_INVALID_STATE);
    EXPECT_TRUE(rodaq::plugin::abi::default_invalid(reference));
}

TEST_F(AuthoringTest, ConfigureExceptionsStayInsideCAbi) {
    EXPECT_EQ(configure("throw"), RODAQ_STATUS_INTERNAL);
    EXPECT_EQ(error_.message.data, error_buffer_.data());
    EXPECT_EQ(error_.message.capacity, error_buffer_.size());
}

TEST_F(AuthoringTest, ConfigureFailureAllowsRepeatedShutdown) {
    EXPECT_EQ(configure("fail"), RODAQ_STATUS_UNAVAILABLE);
    EXPECT_EQ(
        instance_.vtable->shutdown(instance_.context, {RODAQ_DEADLINE_INFINITE}, &error_),
        RODAQ_STATUS_OK);
}

TEST_F(AuthoringTest, ProviderExceptionsInvalidateSuccessOnlyOutput) {
    ASSERT_EQ(configure({}), RODAQ_STATUS_OK);
    const rodaq_interface_query_t query{
        {RODAQ_COUNTER_INTERFACE_ID, sizeof(RODAQ_COUNTER_INTERFACE_ID) - 1}, 1, 0, 0};
    rodaq_interface_reference_t reference{};
    ASSERT_EQ(
        instance_.vtable->query(instance_.context, &query, &reference, &error_), RODAQ_STATUS_OK);
    CounterInterface::Vtable table{};
    ASSERT_TRUE(rodaq::plugin::abi::copy_vtable<CounterInterface>(reference, table));
    std::int64_t value = 99;
    EXPECT_EQ(
        table.increment(reference.context, -1, &value, &error_), RODAQ_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_EQ(value, 0);
    value = 99;
    EXPECT_EQ(table.increment(reference.context, -2, &value, &error_), RODAQ_STATUS_INTERNAL);
    EXPECT_EQ(value, 0);
}

TEST(PluginAuthoring, ConstructorAllocationFailureKeepsInvalidOutput) {
    rodaq_plugin_instance_t instance{};
    EXPECT_EQ(
        rodaq::plugin::authoring::Instance<AllocationFailure>::create(&instance, nullptr),
        RODAQ_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_EQ(instance.context, nullptr);
    EXPECT_EQ(instance.vtable, nullptr);
}

TEST(PluginAuthoring, MalformedCallerErrorBufferRejectsBeforeBackend) {
    rodaq_plugin_instance_t instance{};
    rodaq_error_t error{{nullptr, 10, 0}};
    EXPECT_EQ(
        rodaq::plugin::authoring::Instance<TestPlugin>::create(&instance, &error),
        RODAQ_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(instance.context, nullptr);
    EXPECT_EQ(instance.vtable, nullptr);
}

}  // namespace
