#include "plugin/plugin_manager.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace {

using rodaq::plugin::PluginManager;

struct AcquisitionInstance {
    const rodaq_acquisition_plugin* protocol = nullptr;
    void* instance = nullptr;
    rodaq::plugin::PluginLibraryLease lease;

    AcquisitionInstance() = default;
    AcquisitionInstance(const AcquisitionInstance&) = delete;
    AcquisitionInstance& operator=(const AcquisitionInstance&) = delete;
    AcquisitionInstance(AcquisitionInstance&& other) noexcept
    : protocol(other.protocol),
      instance(std::exchange(other.instance, nullptr)),
      lease(std::move(other.lease)) {}
    AcquisitionInstance& operator=(AcquisitionInstance&&) = delete;

    ~AcquisitionInstance() {
        if (instance != nullptr) {
            protocol->destroy(instance);
        }
    }
};

std::filesystem::path plugin_path() {
    const char* value = std::getenv("RODAQ_TEST_SDK_ACQUISITION");
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error("Missing test environment: RODAQ_TEST_SDK_ACQUISITION");
    }
    return std::filesystem::path(value);
}

AcquisitionInstance create_instance(PluginManager& manager) {
    auto lease = manager.load_library(plugin_path());
    if (!lease.ok()) {
        throw std::runtime_error("Failed to load SDK Acquisition fixture");
    }
    AcquisitionInstance owner;
    owner.lease = lease.value();
    owner.protocol = lease.value().acquisition_protocol();
    rodaq_status error{};
    if (owner.protocol == nullptr ||
        owner.protocol->create(&owner.instance, &error) != RODAQ_STATUS_OK) {
        owner.instance = nullptr;
    }
    return owner;
}

}  // namespace

TEST(AcquisitionInputValidationTest, ControlMethodsRejectMalformedArguments) {
    PluginManager manager;
    AcquisitionInstance owner = create_instance(manager);
    ASSERT_NE(nullptr, owner.instance);
    rodaq_status error{};
    rodaq_interface interface {};
    ASSERT_EQ(
        RODAQ_STATUS_OK, owner.protocol->query_interface(
                             owner.instance,
                             rodaq_string_view{
                                 RODAQ_ACQUISITION_CONTROL_INTERFACE_ID,
                                 sizeof(RODAQ_ACQUISITION_CONTROL_INTERFACE_ID) - 1},
                             &interface, &error));
    const auto* control = static_cast<const rodaq_acquisition_control*>(interface.vtable);
    ASSERT_NE(nullptr, control);

    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        control->prepare_binding_switch(
            nullptr, rodaq_binding_transition{1}, rodaq_stream_handle{1}, {},
            rodaq_stream_descriptor_handle{2}, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT, control->prepare_binding_switch(
                                           interface.context, {}, rodaq_stream_handle{1}, {},
                                           rodaq_stream_descriptor_handle{2}, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT, control->prepare_binding_switch(
                                           interface.context, rodaq_binding_transition{1}, {}, {},
                                           rodaq_stream_descriptor_handle{2}, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT, control->prepare_binding_switch(
                                           interface.context, rodaq_binding_transition{1},
                                           rodaq_stream_handle{1}, {}, {}, &error));

    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        control->commit_binding_switch(nullptr, rodaq_binding_transition{1}, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        control->commit_binding_switch(interface.context, {}, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        control->abort_binding_switch(
            nullptr, rodaq_binding_transition{1}, RODAQ_BINDING_ABORT_KEEP_PAUSED, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        control->abort_binding_switch(
            interface.context, {}, RODAQ_BINDING_ABORT_KEEP_PAUSED, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        control->abort_binding_switch(interface.context, rodaq_binding_transition{1}, 99, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        control->quiesce_stream(nullptr, rodaq_stream_handle{1}, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT, control->quiesce_stream(interface.context, {}, &error));
    EXPECT_EQ(RODAQ_STATUS_INVALID_ARGUMENT, control->quiesce_plugin(nullptr, &error));

    EXPECT_EQ(
        RODAQ_STATUS_OK, control->prepare_binding_switch(
                             interface.context, rodaq_binding_transition{1}, rodaq_stream_handle{1},
                             {}, rodaq_stream_descriptor_handle{2}, &error));
    EXPECT_EQ(
        RODAQ_STATUS_OK,
        control->commit_binding_switch(interface.context, rodaq_binding_transition{1}, &error));
    EXPECT_EQ(
        RODAQ_STATUS_OK, control->abort_binding_switch(
                             interface.context, rodaq_binding_transition{2},
                             RODAQ_BINDING_ABORT_KEEP_PAUSED, &error));
    EXPECT_EQ(
        RODAQ_STATUS_OK,
        control->quiesce_stream(interface.context, rodaq_stream_handle{1}, &error));
    EXPECT_EQ(RODAQ_STATUS_OK, control->quiesce_plugin(interface.context, &error));
}

TEST(AcquisitionInputValidationTest, QueryInterfaceRejectsMalformedViews) {
    PluginManager manager;
    AcquisitionInstance owner = create_instance(manager);
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
}
