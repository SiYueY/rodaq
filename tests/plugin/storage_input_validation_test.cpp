#include "plugin/plugin_manager.hpp"

#include <rodaq/plugin/services.hpp>
#include <rodaq/plugin/storage_plugin.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using rodaq::plugin::PluginManager;

constexpr std::uint8_t kCanary = 0xC3;

struct StorageInstance {
    const rodaq_storage_plugin* protocol = nullptr;
    void* instance = nullptr;
    rodaq::plugin::PluginLibraryLease lease;

    StorageInstance() = default;
    StorageInstance(const StorageInstance&) = delete;
    StorageInstance& operator=(const StorageInstance&) = delete;
    StorageInstance(StorageInstance&& other) noexcept
    : protocol(other.protocol),
      instance(std::exchange(other.instance, nullptr)),
      lease(std::move(other.lease)) {}
    StorageInstance& operator=(StorageInstance&&) = delete;

    ~StorageInstance() {
        if (instance != nullptr) {
            protocol->destroy(instance);
        }
    }
};

std::filesystem::path plugin_path() {
    const char* value = std::getenv("RODAQ_TEST_SDK_STORAGE");
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error("Missing test environment: RODAQ_TEST_SDK_STORAGE");
    }
    return std::filesystem::path(value);
}

StorageInstance create_instance(PluginManager& manager) {
    auto lease = manager.load_library(plugin_path());
    if (!lease.ok()) {
        throw std::runtime_error("Failed to load SDK Storage fixture");
    }
    StorageInstance owner;
    owner.lease = lease.value();
    owner.protocol = lease.value().storage_protocol();
    rodaq_status error{};
    if (owner.protocol == nullptr ||
        owner.protocol->create(&owner.instance, &error) != RODAQ_STATUS_OK) {
        owner.instance = nullptr;
    }
    return owner;
}

rodaq_storage_open_request valid_open() {
    rodaq_storage_open_request request{};
    request.struct_size = sizeof(request);
    request.session_id = rodaq_string_view{"session", sizeof("session") - 1};
    request.target_uri = rodaq_string_view{"memory://fixture", sizeof("memory://fixture") - 1};
    return request;
}

rodaq_storage_submission valid_submission() {
    rodaq_storage_submission submission{};
    submission.struct_size = sizeof(submission);
    submission.operation.value = 1;
    submission.stream.value = 2;
    submission.descriptor.value = 3;
    submission.source_present = RODAQ_FALSE;
    submission.source_sequence_present = RODAQ_FALSE;
    return submission;
}

struct SubmitOutput {
    rodaq_storage_submit_result value;
    std::array<std::uint8_t, 24> canary;
};

SubmitOutput make_output() {
    SubmitOutput output{};
    output.value.struct_size = sizeof(output.value) + output.canary.size();
    output.canary.fill(kCanary);
    return output;
}

void expect_canary(const SubmitOutput& output) {
    for (const std::uint8_t byte : output.canary) {
        EXPECT_EQ(kCanary, byte);
    }
}

void expect_open_rejected(StorageInstance& owner, const rodaq_storage_open_request& request) {
    rodaq_status error{};
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT, owner.protocol->open(owner.instance, &request, &error));
    EXPECT_EQ(RODAQ_STATUS_INVALID_ARGUMENT, error.status_code);
    EXPECT_NE(0U, error.message_size);

    const rodaq_storage_open_request valid = valid_open();
    EXPECT_EQ(RODAQ_STATUS_OK, owner.protocol->open(owner.instance, &valid, &error));
}

}  // namespace

TEST(StorageInputValidationTest, OpenRejectsMalformedViewsBeforePluginEntry) {
    PluginManager manager;

    {
        StorageInstance owner = create_instance(manager);
        ASSERT_NE(nullptr, owner.instance);
        rodaq_status error{};
        const auto request = valid_open();
        EXPECT_EQ(RODAQ_STATUS_INVALID_ARGUMENT, owner.protocol->open(nullptr, &request, &error));
        EXPECT_EQ(
            RODAQ_STATUS_INVALID_ARGUMENT, owner.protocol->open(owner.instance, nullptr, &error));
    }

    {
        StorageInstance owner = create_instance(manager);
        ASSERT_NE(nullptr, owner.instance);
        auto request = valid_open();
        request.session_id = rodaq_string_view{nullptr, 1};
        expect_open_rejected(owner, request);
    }
    {
        StorageInstance owner = create_instance(manager);
        ASSERT_NE(nullptr, owner.instance);
        auto request = valid_open();
        request.target_uri = rodaq_string_view{nullptr, 1};
        expect_open_rejected(owner, request);
    }
    {
        StorageInstance owner = create_instance(manager);
        ASSERT_NE(nullptr, owner.instance);
        auto request = valid_open();
        request.session_id = rodaq_string_view{"\xFF", 1};
        expect_open_rejected(owner, request);
    }
    {
        StorageInstance owner = create_instance(manager);
        ASSERT_NE(nullptr, owner.instance);
        auto request = valid_open();
        request.target_uri = rodaq_string_view{"a\0b", 3};
        expect_open_rejected(owner, request);
    }
    {
        StorageInstance owner = create_instance(manager);
        ASSERT_NE(nullptr, owner.instance);
        auto request = valid_open();
        request.struct_size = RODAQ_STORAGE_OPEN_REQUEST_MIN_STRUCT_SIZE - 1;
        expect_open_rejected(owner, request);
    }
    {
        StorageInstance owner = create_instance(manager);
        ASSERT_NE(nullptr, owner.instance);
        auto request = valid_open();
        request.flags = 1;
        expect_open_rejected(owner, request);
    }
    {
        StorageInstance owner = create_instance(manager);
        ASSERT_NE(nullptr, owner.instance);
        auto request = valid_open();
        request.session_id = {};
        request.target_uri = {};
        rodaq_status error{};
        EXPECT_EQ(RODAQ_STATUS_OK, owner.protocol->open(owner.instance, &request, &error));
    }
}

TEST(StorageInputValidationTest, SubmitRejectsMalformedViewsBeforePluginEntry) {
    PluginManager manager;

    {
        StorageInstance owner = create_instance(manager);
        const auto request = valid_open();
        rodaq_status error{};
        ASSERT_EQ(RODAQ_STATUS_OK, owner.protocol->open(owner.instance, &request, &error));
        auto output = make_output();
        const auto submission = valid_submission();
        EXPECT_EQ(
            RODAQ_STATUS_INVALID_ARGUMENT,
            owner.protocol->submit(nullptr, &submission, &output.value, &error));
        EXPECT_EQ(
            RODAQ_STATUS_INVALID_ARGUMENT,
            owner.protocol->submit(owner.instance, nullptr, &output.value, &error));
        EXPECT_EQ(
            RODAQ_STATUS_INVALID_ARGUMENT,
            owner.protocol->submit(owner.instance, &submission, nullptr, &error));
    }

    const auto expect_rejected = [&manager](auto mutate) {
        StorageInstance owner = create_instance(manager);
        ASSERT_NE(nullptr, owner.instance);
        const auto open = valid_open();
        rodaq_status error{};
        ASSERT_EQ(RODAQ_STATUS_OK, owner.protocol->open(owner.instance, &open, &error));
        auto submission = valid_submission();
        mutate(submission);
        auto output = make_output();
        EXPECT_EQ(
            RODAQ_STATUS_INVALID_ARGUMENT,
            owner.protocol->submit(owner.instance, &submission, &output.value, &error));
        EXPECT_EQ(RODAQ_STATUS_INVALID_ARGUMENT, error.status_code);
        EXPECT_NE(0U, error.message_size);
        expect_canary(output);

        const auto valid = valid_submission();
        output = make_output();
        EXPECT_EQ(
            RODAQ_STATUS_OK, owner.protocol->submit(owner.instance, &valid, &output.value, &error));
    };

    expect_rejected([](auto& value) { value.struct_size -= 1; });
    expect_rejected([](auto& value) { value.flags = 1; });
    expect_rejected([](auto& value) { value.reserved = 1; });
    expect_rejected([](auto& value) { value.operation.value = 0; });
    expect_rejected([](auto& value) { value.stream.value = 0; });
    expect_rejected([](auto& value) { value.descriptor.value = 0; });
    expect_rejected([](auto& value) { value.source_present = 2; });
    expect_rejected([](auto& value) {
        value.source_present = RODAQ_TRUE;
        value.source_sequence_present = RODAQ_TRUE;
        value.source.value = 0;
    });
    expect_rejected([](auto& value) {
        value.source_present = RODAQ_FALSE;
        value.source_sequence_present = RODAQ_TRUE;
    });
    expect_rejected([](auto& value) {
        value.timestamp_values = nullptr;
        value.timestamp_value_count = 1;
    });
    expect_rejected([](auto& value) {
        static const rodaq_timestamp_value timestamp[] = {{RODAQ_FALSE, 1, 0}};
        value.timestamp_values = timestamp;
        value.timestamp_value_count = 1;
    });
    expect_rejected([](auto& value) {
        static const rodaq_timestamp_value timestamp[] = {{2, 0, 0}};
        value.timestamp_values = timestamp;
        value.timestamp_value_count = 1;
    });
    expect_rejected([](auto& value) {
        value.payload.data = nullptr;
        value.payload.size = 1;
    });
    expect_rejected([](auto& value) {
        static const rodaq_timestamp_value timestamp[] = {{RODAQ_FALSE, 0, 0}};
        value.timestamp_values = timestamp;
        value.timestamp_value_count = UINT32_MAX;
    });
}

TEST(StorageInputValidationTest, SubmitAcceptsExplicitEmptyViews) {
    PluginManager manager;
    StorageInstance owner = create_instance(manager);
    ASSERT_NE(nullptr, owner.instance);
    const auto open = valid_open();
    rodaq_status error{};
    ASSERT_EQ(RODAQ_STATUS_OK, owner.protocol->open(owner.instance, &open, &error));

    auto submission = valid_submission();
    std::uint8_t unused_payload = 0;
    submission.timestamp_values = nullptr;
    submission.timestamp_value_count = 0;
    submission.payload = rodaq_bytes_view{&unused_payload, 0};
    auto output = make_output();
    ASSERT_EQ(
        RODAQ_STATUS_OK,
        owner.protocol->submit(owner.instance, &submission, &output.value, &error));
    EXPECT_EQ(RODAQ_STORAGE_PAYLOAD_CONSUMED, output.value.payload_state);
    EXPECT_EQ(RODAQ_STORAGE_PERSISTENCE_SUCCEEDED, output.value.persistence_state);
}

TEST(StorageInputValidationTest, QueryAndReadinessValidateInputs) {
    PluginManager manager;
    StorageInstance owner = create_instance(manager);
    ASSERT_NE(nullptr, owner.instance);
    rodaq_status error{};
    rodaq_interface interface {};

    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        owner.protocol->query_interface(nullptr, rodaq_string_view{"id", 2}, &interface, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        owner.protocol->query_interface(
            owner.instance, rodaq_string_view{"id", 2}, nullptr, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        owner.protocol->query_interface(
            owner.instance, rodaq_string_view{nullptr, 1}, &interface, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        owner.protocol->query_interface(
            owner.instance, rodaq_string_view{"\xFF", 1}, &interface, &error));

    ASSERT_EQ(
        RODAQ_STATUS_OK,
        owner.protocol->query_interface(
            owner.instance,
            rodaq_string_view{
                RODAQ_STORAGE_CONTROL_INTERFACE_ID, sizeof(RODAQ_STORAGE_CONTROL_INTERFACE_ID) - 1},
            &interface, &error));
    const auto* control = static_cast<const rodaq_storage_control*>(interface.vtable);
    ASSERT_NE(nullptr, control);
    rodaq_descriptor_readiness_result result{};
    result.struct_size = sizeof(result);
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        control->ensure_descriptor_ready(
            nullptr, rodaq_stream_descriptor_handle{1}, &result, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        control->ensure_descriptor_ready(interface.context, {}, &result, &error));
}

TEST(StorageInputValidationTest, RejectsMalformedSynchronousSubmitResults) {
    auto valid = [](rodaq_storage_submit_result* result) {
        *result = {};
        result->struct_size = sizeof(*result);
        result->payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
        result->persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
    };

    rodaq_storage_submit_result result{};
    valid(&result);
    EXPECT_TRUE(rodaq::plugin::is_valid_storage_submit_result(result));
    result.struct_size = RODAQ_STORAGE_SUBMIT_RESULT_MIN_STRUCT_SIZE - 1;
    EXPECT_FALSE(rodaq::plugin::is_valid_storage_submit_result(result));

    valid(&result);
    result.reserved = 1;
    EXPECT_FALSE(rodaq::plugin::is_valid_storage_submit_result(result));

    valid(&result);
    result.persistence_error.status_code = RODAQ_STATUS_OK;
    result.persistence_error.error_code = 1;
    EXPECT_FALSE(rodaq::plugin::is_valid_storage_submit_result(result));

    valid(&result);
    result.persistence_error.flags = 2;
    EXPECT_FALSE(rodaq::plugin::is_valid_storage_submit_result(result));

    valid(&result);
    result.persistence_state = RODAQ_STORAGE_PERSISTENCE_FAILED;
    EXPECT_FALSE(rodaq::plugin::is_valid_storage_submit_result(result));

    valid(&result);
    result.persistence_state = RODAQ_STORAGE_PERSISTENCE_UNKNOWN;
    result.persistence_error.status_code = RODAQ_STATUS_STORAGE_ERROR;
    result.persistence_error.message_size = 1;
    result.persistence_error.message[0] = 'x';
    EXPECT_TRUE(rodaq::plugin::is_valid_storage_submit_result(result));

    result.persistence_error.message[0] = static_cast<char>(0xff);
    EXPECT_FALSE(rodaq::plugin::is_valid_storage_submit_result(result));

    result.persistence_error.message[0] = 'x';
    result.persistence_error.reserved[0] = 1;
    EXPECT_FALSE(rodaq::plugin::is_valid_storage_submit_result(result));
}
