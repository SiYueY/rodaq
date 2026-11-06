#include "plugin/plugin_manager.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using rodaq::plugin::PluginManager;

constexpr std::uint8_t kCanary = 0x5A;

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

std::filesystem::path plugin_path(const char* variable) {
    const char* value = std::getenv(variable);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string("Missing test environment: ") + variable);
    }
    return std::filesystem::path(value);
}

StorageInstance create_instance(rodaq::plugin::PluginLibraryLease& lease) {
    StorageInstance owner;
    owner.lease = lease;
    owner.protocol = lease.storage_protocol();
    if (owner.protocol == nullptr) {
        return owner;
    }
    rodaq_status error{};
    if (owner.protocol->create(&owner.instance, &error) != RODAQ_STATUS_OK) {
        owner.instance = nullptr;
    }
    return owner;
}

void expect_canary(const std::uint8_t* bytes, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        EXPECT_EQ(kCanary, bytes[index]);
    }
}

std::string_view diagnostic(const rodaq_status& error) {
    return std::string_view(error.message, error.message_size);
}

void expect_exception_error(
    rodaq_status_code status, const rodaq_status& error, rodaq_status_code expected_status,
    std::string_view expected_message) {
    EXPECT_EQ(expected_status, status);
    EXPECT_EQ(expected_status, error.status_code);
    EXPECT_EQ("sdk", std::string_view(error.domain, error.domain_size));
    EXPECT_NE(std::string_view::npos, diagnostic(error).find(expected_message));
    EXPECT_EQ(0U, error.reserved[0]);
    EXPECT_EQ(0U, error.reserved[1]);
}

void run_capabilities_case(
    const char* environment, rodaq_status_code expected_status, std::string_view expected_message) {
    PluginManager manager;
    auto lease = manager.load_library(plugin_path(environment));
    ASSERT_TRUE(lease.ok()) << environment;
    StorageInstance owner = create_instance(lease.value());
    ASSERT_NE(nullptr, owner.protocol) << environment;
    ASSERT_NE(nullptr, owner.instance) << environment;

    rodaq_interface interface {};
    rodaq_status query_error{};
    ASSERT_EQ(
        RODAQ_STATUS_OK,
        owner.protocol->query_interface(
            owner.instance,
            rodaq_string_view{
                RODAQ_STORAGE_CONTROL_INTERFACE_ID, sizeof(RODAQ_STORAGE_CONTROL_INTERFACE_ID) - 1},
            &interface, &query_error))
        << environment;
    const auto* control = static_cast<const rodaq_storage_control_v1*>(interface.vtable);
    ASSERT_NE(nullptr, control) << environment;

    struct {
        rodaq_storage_capabilities value;
        std::array<std::uint8_t, 32> canary;
    } output{};
    output.canary.fill(kCanary);
    output.value.struct_size = sizeof(output.value) + output.canary.size();
    rodaq_status error{};
    const rodaq_status_code status =
        control->get_capabilities(interface.context, &output.value, &error);
    expect_exception_error(status, error, expected_status, expected_message);
    expect_canary(output.canary.data(), output.canary.size());
}

void run_ready_case(
    const char* environment, rodaq_status_code expected_status, std::string_view expected_message) {
    PluginManager manager;
    auto lease = manager.load_library(plugin_path(environment));
    ASSERT_TRUE(lease.ok()) << environment;
    StorageInstance owner = create_instance(lease.value());
    ASSERT_NE(nullptr, owner.protocol) << environment;
    ASSERT_NE(nullptr, owner.instance) << environment;

    rodaq_interface interface {};
    rodaq_status query_error{};
    ASSERT_EQ(
        RODAQ_STATUS_OK,
        owner.protocol->query_interface(
            owner.instance,
            rodaq_string_view{
                RODAQ_STORAGE_CONTROL_INTERFACE_ID, sizeof(RODAQ_STORAGE_CONTROL_INTERFACE_ID) - 1},
            &interface, &query_error))
        << environment;
    const auto* control = static_cast<const rodaq_storage_control_v1*>(interface.vtable);
    ASSERT_NE(nullptr, control) << environment;

    struct {
        rodaq_descriptor_readiness_result value;
        std::array<std::uint8_t, 32> canary;
    } output{};
    output.canary.fill(kCanary);
    output.value.struct_size = sizeof(output.value) + output.canary.size();
    rodaq_status error{};
    const rodaq_status_code status = control->ensure_descriptor_ready(
        interface.context, rodaq_stream_descriptor_handle{7}, &output.value, &error);
    expect_exception_error(status, error, expected_status, expected_message);
    expect_canary(output.canary.data(), output.canary.size());
}

void run_submit_case(
    const char* environment, rodaq_status_code expected_status, std::string_view expected_message) {
    PluginManager manager;
    auto lease = manager.load_library(plugin_path(environment));
    ASSERT_TRUE(lease.ok()) << environment;
    StorageInstance owner = create_instance(lease.value());
    ASSERT_NE(nullptr, owner.protocol) << environment;
    ASSERT_NE(nullptr, owner.instance) << environment;

    const char session[] = "session";
    const char target[] = "memory://exception";
    rodaq_storage_open_request open{};
    open.struct_size = sizeof(open);
    open.session_id = rodaq_string_view{session, sizeof(session) - 1};
    open.target_uri = rodaq_string_view{target, sizeof(target) - 1};
    rodaq_status open_error{};
    ASSERT_EQ(RODAQ_STATUS_OK, owner.protocol->open(owner.instance, &open, &open_error))
        << environment;

    rodaq_storage_submission submission{};
    submission.struct_size = sizeof(submission);
    submission.operation.value = 1;
    submission.stream.value = 2;
    submission.descriptor.value = 3;
    submission.source_present = RODAQ_FALSE;
    submission.source_sequence_present = RODAQ_FALSE;

    struct {
        rodaq_storage_submit_result value;
        std::array<std::uint8_t, 32> canary;
    } output{};
    output.canary.fill(kCanary);
    output.value.struct_size = sizeof(output.value) + output.canary.size();
    rodaq_status error{};
    const rodaq_status_code status =
        owner.protocol->submit(owner.instance, &submission, &output.value, &error);
    expect_exception_error(status, error, expected_status, expected_message);
    expect_canary(output.canary.data(), output.canary.size());
}

}  // namespace

TEST(StorageExceptionMatrix, SubmitContainsAllExceptions) {
    run_submit_case(
        "RODAQ_TEST_STORAGE_EXCEPTION_BAD_ALLOC_SUBMIT", RODAQ_STATUS_RESOURCE_EXHAUSTED, "memory");
    run_submit_case(
        "RODAQ_TEST_STORAGE_EXCEPTION_RUNTIME_ERROR_SUBMIT", RODAQ_STATUS_INTERNAL_ERROR,
        "fixture storage runtime exception");
    run_submit_case(
        "RODAQ_TEST_STORAGE_EXCEPTION_UNKNOWN_SUBMIT", RODAQ_STATUS_INTERNAL_ERROR, "unknown");
}

TEST(StorageExceptionMatrix, CapabilitiesContainsAllExceptions) {
    run_capabilities_case(
        "RODAQ_TEST_STORAGE_EXCEPTION_BAD_ALLOC_CAPABILITIES", RODAQ_STATUS_RESOURCE_EXHAUSTED,
        "memory");
    run_capabilities_case(
        "RODAQ_TEST_STORAGE_EXCEPTION_RUNTIME_ERROR_CAPABILITIES", RODAQ_STATUS_INTERNAL_ERROR,
        "fixture storage runtime exception");
    run_capabilities_case(
        "RODAQ_TEST_STORAGE_EXCEPTION_UNKNOWN_CAPABILITIES", RODAQ_STATUS_INTERNAL_ERROR,
        "unknown");
}

TEST(StorageExceptionMatrix, DescriptorReadinessContainsAllExceptions) {
    run_ready_case(
        "RODAQ_TEST_STORAGE_EXCEPTION_BAD_ALLOC_READY", RODAQ_STATUS_RESOURCE_EXHAUSTED, "memory");
    run_ready_case(
        "RODAQ_TEST_STORAGE_EXCEPTION_RUNTIME_ERROR_READY", RODAQ_STATUS_INTERNAL_ERROR,
        "fixture storage runtime exception");
    run_ready_case(
        "RODAQ_TEST_STORAGE_EXCEPTION_UNKNOWN_READY", RODAQ_STATUS_INTERNAL_ERROR, "unknown");
}
