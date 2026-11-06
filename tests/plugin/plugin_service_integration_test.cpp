#include "plugin/acquisition_plugin_instance.hpp"
#include "config/abi_bridge.hpp"
#include "plugin/config_view.hpp"
#include "plugin/plugin_manager.hpp"
#include "plugin/service_registry.hpp"
#include "plugin/storage_plugin_instance.hpp"
#include "clock/clock_correlation.hpp"
#include "projection/projection_registry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

using rodaq::ClockCorrelation;
using rodaq::ProjectionLimits;
using rodaq::ProjectionRegistry;
using rodaq::UnixEpochClockAnchor;
using rodaq::plugin::AcquisitionPluginInstance;
using rodaq::plugin::ConfigTree;
using rodaq::plugin::PluginManager;
using rodaq::plugin::ServiceRegistry;
using rodaq::plugin::StoragePluginInstance;

int common_context = 0;
int canonical_context = 0;
int acquisition_context = 0;
int storage_context = 0;

void log_message(void*, rodaq_log_level, rodaq_string_view, rodaq_string_view) noexcept {}

std::uint64_t monotonic_time(void*) noexcept { return 0; }

rodaq_status_code get_lifecycle_snapshot(
    void*, rodaq_runtime_lifecycle_snapshot* out_snapshot, rodaq_status*) noexcept {
    if (out_snapshot != nullptr) {
        *out_snapshot = {};
        out_snapshot->state = RODAQ_RUNTIME_RUNNING;
        out_snapshot->normal_acquisition_allowed = RODAQ_TRUE;
        out_snapshot->generation = 1;
    }
    return RODAQ_STATUS_OK;
}

void report_diagnostic(
    void*, rodaq_diagnostic_severity, rodaq_string_view, rodaq_string_view) noexcept {}

rodaq_status_code get_source(
    void*, rodaq_source_handle, rodaq_source_info* out_info, rodaq_status*) noexcept {
    if (out_info != nullptr) {
        *out_info = {};
    }
    return RODAQ_STATUS_NOT_FOUND;
}

rodaq_status_code get_clock(
    void*, rodaq_clock_handle, rodaq_clock_info* out_info, rodaq_status*) noexcept {
    if (out_info != nullptr) {
        *out_info = {};
    }
    return RODAQ_STATUS_NOT_FOUND;
}

rodaq_status_code get_schema(
    void*, rodaq_schema_handle, rodaq_schema_info* out_info, rodaq_status*) noexcept {
    if (out_info != nullptr) {
        *out_info = {};
    }
    return RODAQ_STATUS_NOT_FOUND;
}

rodaq_status_code get_timestamp_key_set(
    void*, std::uint64_t, rodaq_timestamp_key_set_info* out_info, rodaq_status*) noexcept {
    if (out_info != nullptr) {
        *out_info = {};
    }
    return RODAQ_STATUS_NOT_FOUND;
}

rodaq_status_code get_stream(
    void*, rodaq_stream_handle, rodaq_stream_info* out_info, rodaq_status*) noexcept {
    if (out_info != nullptr) {
        *out_info = {};
    }
    return RODAQ_STATUS_NOT_FOUND;
}

rodaq_status_code get_stream_descriptor(
    void*, rodaq_stream_descriptor_handle, rodaq_stream_descriptor_info* out_info,
    rodaq_status*) noexcept {
    if (out_info != nullptr) {
        *out_info = {};
    }
    return RODAQ_STATUS_NOT_FOUND;
}

rodaq_status_code register_source(
    void*, const rodaq_source_registration*, rodaq_source_handle* out_handle,
    rodaq_status*) noexcept {
    if (out_handle != nullptr) {
        *out_handle = {};
    }
    return RODAQ_STATUS_INVALID_STATE;
}

rodaq_status_code register_clock(
    void*, const rodaq_clock_registration*, rodaq_clock_handle* out_handle,
    rodaq_status*) noexcept {
    if (out_handle != nullptr) {
        *out_handle = {};
    }
    return RODAQ_STATUS_INVALID_STATE;
}

rodaq_status_code register_schema(
    void*, const rodaq_schema_registration*, rodaq_schema_handle* out_handle,
    rodaq_status*) noexcept {
    if (out_handle != nullptr) {
        *out_handle = {};
    }
    return RODAQ_STATUS_INVALID_STATE;
}

rodaq_status_code register_stream(
    void*, const rodaq_stream_registration*, rodaq_stream_handle* out_handle,
    rodaq_status*) noexcept {
    if (out_handle != nullptr) {
        *out_handle = {};
    }
    return RODAQ_STATUS_INVALID_STATE;
}

rodaq_status_code publish_stream_descriptor(
    void*, const rodaq_stream_descriptor_registration*, rodaq_stream_descriptor_handle* out_handle,
    rodaq_status*) noexcept {
    if (out_handle != nullptr) {
        *out_handle = {};
    }
    return RODAQ_STATUS_INVALID_STATE;
}

rodaq_status_code report_source_availability(
    void*, rodaq_source_handle, rodaq_availability_state, const rodaq_status*,
    rodaq_status*) noexcept {
    return RODAQ_STATUS_INVALID_STATE;
}

rodaq_status_code report_stream_availability(
    void*, rodaq_stream_handle, rodaq_availability_state, const rodaq_status*,
    rodaq_status*) noexcept {
    return RODAQ_STATUS_INVALID_STATE;
}

rodaq_status_code report_plugin_state(
    void*, rodaq_acquisition_plugin_state, const rodaq_status*, rodaq_status*) noexcept {
    return RODAQ_STATUS_INVALID_STATE;
}

rodaq_status_code submit_record(
    void*, const rodaq_record_submission*, rodaq_acquisition_submit_result* out_result,
    rodaq_status*) noexcept {
    if (out_result != nullptr) {
        *out_result = {};
    }
    return RODAQ_STATUS_INVALID_STATE;
}

void payload_consumed(void*, rodaq_storage_operation) noexcept {}

void persistence_completed(
    void*, rodaq_storage_operation, rodaq_storage_persistence_state, const rodaq_status*) noexcept {
}

void report_storage_failure(void*, const rodaq_status*) noexcept {}

const rodaq_runtime_common common_vtable = {
    RODAQ_RUNTIME_COMMON_PROTOCOL_VERSION,
    RODAQ_RUNTIME_COMMON_MIN_STRUCT_SIZE,
    &log_message,
    &monotonic_time,
    &get_lifecycle_snapshot,
    &report_diagnostic,
};

const rodaq_runtime_canonical canonical_vtable = {
    RODAQ_RUNTIME_CANONICAL_PROTOCOL_VERSION,
    RODAQ_RUNTIME_CANONICAL_MIN_STRUCT_SIZE,
    &get_source,
    &get_clock,
    &get_schema,
    &get_timestamp_key_set,
    &get_stream,
    &get_stream_descriptor,
};

const rodaq_runtime_acquisition acquisition_vtable = {
    RODAQ_RUNTIME_ACQUISITION_PROTOCOL_VERSION,
    RODAQ_RUNTIME_ACQUISITION_MIN_STRUCT_SIZE,
    &register_source,
    &register_clock,
    &register_schema,
    &register_stream,
    &publish_stream_descriptor,
    &report_source_availability,
    &report_stream_availability,
    &report_plugin_state,
    &submit_record,
};

const rodaq_runtime_storage storage_vtable = {
    RODAQ_RUNTIME_STORAGE_PROTOCOL_VERSION,
    RODAQ_RUNTIME_STORAGE_MIN_STRUCT_SIZE,
    &payload_consumed,
    &persistence_completed,
    &report_storage_failure,
};

template <typename T>
T take(rodaq::Result<T> result) {
    if (!result.ok()) {
        throw std::runtime_error("Expected successful operation");
    }
    return std::move(result.value());
}

ConfigTree make_config() {
    ConfigTree nested = take(ConfigTree::map({
        {"name", ConfigTree::string("node")},
        {"level", ConfigTree::int64(2)},
    }));
    ConfigTree items = take(ConfigTree::list({
        ConfigTree::int64(1),
        ConfigTree::int64(2),
        ConfigTree::string("three"),
    }));
    return take(ConfigTree::map({
        {"enabled", ConfigTree::boolean(true)},
        {"limit", ConfigTree::int64(7)},
        {"gain", ConfigTree::float64(0.25)},
        {"mode", ConfigTree::string("fixture")},
        {"nested", std::move(nested)},
        {"items", std::move(items)},
    }));
}

fs::path plugin_path(const char* environment_name) {
    const char* value = std::getenv(environment_name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string("Missing test environment: ") + environment_name);
    }
    return fs::path(value);
}

fs::path configure_event_log() {
    const fs::path path =
        fs::temp_directory_path() / ("rodaq-service-integration-" + std::to_string(::getpid()));
    std::error_code error;
    fs::remove(path, error);
    setenv("RODAQ_TEST_EVENT_LOG", path.c_str(), 1);
    return path;
}

std::vector<std::string> read_events(const fs::path& path) {
    std::ifstream input(path);
    std::vector<std::string> events;
    std::string event;
    while (std::getline(input, event)) {
        events.push_back(std::move(event));
    }
    return events;
}

void expect_event_order(const std::vector<std::string>& events) {
    const auto create = std::find(events.begin(), events.end(), "create");
    const auto bind = std::find(events.begin(), events.end(), "bind");
    const auto configure = std::find(events.begin(), events.end(), "configure");
    const auto destroy = std::find(events.begin(), events.end(), "destroy");
    ASSERT_NE(events.end(), create);
    ASSERT_NE(events.end(), bind);
    ASSERT_NE(events.end(), configure);
    ASSERT_NE(events.end(), destroy);
    EXPECT_LT(create, bind);
    EXPECT_LT(bind, configure);
    EXPECT_LT(configure, destroy);
}

struct RuntimeServices {
    RuntimeServices()
    : projection(ProjectionLimits{64, 8, 256, 1024}),
      clock(take(ClockCorrelation::from_anchor(
          UnixEpochClockAnchor{rodaq_clock_handle{11}, 100, 1'000}))) {}

    rodaq_interface common() noexcept { return rodaq_interface{&common_context, &common_vtable}; }

    rodaq_interface canonical() noexcept {
        return rodaq_interface{&canonical_context, &canonical_vtable};
    }

    rodaq_interface acquisition() noexcept {
        return rodaq_interface{&acquisition_context, &acquisition_vtable};
    }

    rodaq_interface storage() noexcept {
        return rodaq_interface{&storage_context, &storage_vtable};
    }

    ProjectionRegistry projection;
    ClockCorrelation clock;
};

enum class RequiredService {
    Common,
    Canonical,
    Projection,
    Clock,
    Acquisition,
    Storage,
};

void require_success(rodaq::Result<void> result) {
    if (!result.ok()) {
        throw std::runtime_error(std::string(result.status().message()));
    }
}

std::unique_ptr<ServiceRegistry> make_registry(
    RuntimeServices& services, std::optional<RequiredService> omitted = std::nullopt,
    const rodaq_interface* projection_override = nullptr,
    const rodaq_interface* clock_override = nullptr) {
    auto registry = std::make_unique<ServiceRegistry>(6, 128);
    if (omitted != RequiredService::Common) {
        require_success(
            registry->register_entry(RODAQ_RUNTIME_COMMON_SERVICE_ID, services.common()));
    }
    if (omitted != RequiredService::Canonical) {
        require_success(
            registry->register_entry(RODAQ_RUNTIME_CANONICAL_SERVICE_ID, services.canonical()));
    }
    if (omitted != RequiredService::Projection) {
        require_success(registry->register_entry(
            RODAQ_RUNTIME_PROJECTION_SERVICE_ID, projection_override != nullptr
                                                     ? *projection_override
                                                     : services.projection.abi_interface()));
    }
    if (omitted != RequiredService::Clock) {
        require_success(registry->register_entry(
            RODAQ_RUNTIME_CLOCK_CORRELATION_SERVICE_ID,
            clock_override != nullptr ? *clock_override : services.clock.abi_interface()));
    }
    if (omitted != RequiredService::Acquisition) {
        require_success(
            registry->register_entry(RODAQ_RUNTIME_ACQUISITION_SERVICE_ID, services.acquisition()));
    }
    if (omitted != RequiredService::Storage) {
        require_success(
            registry->register_entry(RODAQ_RUNTIME_STORAGE_SERVICE_ID, services.storage()));
    }
    require_success(registry->freeze());
    return registry;
}

void register_fixture_projection(RuntimeServices& services) {
    require_success(services.projection.register_entry(
        rodaq_stream_descriptor_handle{42}, "rodaq.test.projection",
        reinterpret_cast<const std::uint8_t*>("fixture"), 7));
}

void expect_acquisition_control_failure(const char* path_environment) {
    PluginManager manager;
    RuntimeServices services;
    const std::unique_ptr<ServiceRegistry> registry = make_registry(services);
    AcquisitionPluginInstance instance = take(manager.create_acquisition_plugin_instance(
        take(manager.load_library(plugin_path(path_environment)))));
    const auto result = instance.bind(registry->abi_view());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(rodaq::StatusCode::InvalidArgument, result.status().status_code());
    instance.destroy();
}

void expect_storage_control_failure(const char* path_environment) {
    PluginManager manager;
    RuntimeServices services;
    register_fixture_projection(services);
    const std::unique_ptr<ServiceRegistry> registry = make_registry(services);
    StoragePluginInstance instance = take(manager.create_storage_plugin_instance(
        take(manager.load_library(plugin_path(path_environment)))));
    const auto result = instance.bind(registry->abi_view());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(rodaq::StatusCode::InvalidArgument, result.status().status_code());
    instance.destroy();
}

}  // namespace

TEST(PluginServiceIntegrationTest, FakePluginsBindConfigureAndExchangeDAQData) {
    const fs::path events_path = configure_event_log();
    PluginManager manager;
    ConfigTree config = make_config();
    RuntimeServices services;
    const std::unique_ptr<ServiceRegistry> registry = make_registry(services);

    {
        AcquisitionPluginInstance acquisition = take(manager.create_acquisition_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_ACQUISITION_PLUGIN")))));
        StoragePluginInstance storage = take(manager.create_storage_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_STORAGE_PLUGIN")))));

        ASSERT_TRUE(acquisition.bind(registry->abi_view()).ok());
        ASSERT_TRUE(acquisition.configure(rodaq::config_abi_view(config.root())).ok());
        ASSERT_TRUE(storage.bind(registry->abi_view()).ok());
        ASSERT_TRUE(storage.configure(rodaq::config_abi_view(config.root())).ok());
        acquisition.destroy();
        storage.destroy();
    }

    expect_event_order(read_events(events_path));
}

TEST(PluginServiceIntegrationTest, MissingRequiredServicesFailBind) {
    PluginManager manager;

    {
        RuntimeServices services;
        const std::unique_ptr<ServiceRegistry> registry =
            make_registry(services, RequiredService::Acquisition);
        AcquisitionPluginInstance instance = take(manager.create_acquisition_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_ACQUISITION_PLUGIN")))));
        const auto result = instance.bind(registry->abi_view());
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(rodaq::StatusCode::NotFound, result.status().status_code());
        instance.destroy();
    }
    {
        RuntimeServices services;
        const std::unique_ptr<ServiceRegistry> registry =
            make_registry(services, RequiredService::Projection);
        StoragePluginInstance instance = take(manager.create_storage_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_STORAGE_PLUGIN")))));
        const auto result = instance.bind(registry->abi_view());
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(rodaq::StatusCode::NotFound, result.status().status_code());
        instance.destroy();
    }
    {
        RuntimeServices services;
        ASSERT_TRUE(services.projection
                        .register_entry(
                            rodaq_stream_descriptor_handle{42}, "rodaq.test.projection",
                            reinterpret_cast<const std::uint8_t*>("fixture"), 7)
                        .ok());
        const std::unique_ptr<ServiceRegistry> registry =
            make_registry(services, RequiredService::Clock);
        StoragePluginInstance instance = take(manager.create_storage_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_STORAGE_PLUGIN")))));
        const auto result = instance.bind(registry->abi_view());
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(rodaq::StatusCode::NotFound, result.status().status_code());
        instance.destroy();
    }
}

TEST(PluginServiceIntegrationTest, IncompatibleServiceAbiFailsBind) {
    PluginManager manager;

    {
        RuntimeServices services;
        const auto projection_interface = services.projection.abi_interface();
        const auto* valid_projection =
            static_cast<const rodaq_runtime_projection*>(projection_interface.vtable);
        rodaq_runtime_projection wrong_protocol = *valid_projection;
        wrong_protocol.protocol_version = RODAQ_RUNTIME_PROJECTION_PROTOCOL_VERSION + 1;
        const rodaq_interface override{projection_interface.context, &wrong_protocol};
        const std::unique_ptr<ServiceRegistry> registry =
            make_registry(services, std::nullopt, &override);
        AcquisitionPluginInstance instance = take(manager.create_acquisition_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_ACQUISITION_PLUGIN")))));
        const auto result = instance.bind(registry->abi_view());
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(rodaq::StatusCode::InvalidArgument, result.status().status_code());
        instance.destroy();
    }

    {
        RuntimeServices services;
        const rodaq_interface projection_interface = services.projection.abi_interface();
        const auto* valid_projection =
            static_cast<const rodaq_runtime_projection*>(projection_interface.vtable);
        rodaq_runtime_projection null_slot = *valid_projection;
        null_slot.register_projection = nullptr;
        const rodaq_interface override{projection_interface.context, &null_slot};
        const std::unique_ptr<ServiceRegistry> registry =
            make_registry(services, std::nullopt, &override);
        AcquisitionPluginInstance instance = take(manager.create_acquisition_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_ACQUISITION_PLUGIN")))));
        const auto result = instance.bind(registry->abi_view());
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(rodaq::StatusCode::InvalidArgument, result.status().status_code());
        instance.destroy();
    }

    {
        RuntimeServices services;
        const rodaq_interface projection_interface = services.projection.abi_interface();
        const auto* valid_projection =
            static_cast<const rodaq_runtime_projection*>(projection_interface.vtable);
        rodaq_runtime_projection too_small = *valid_projection;
        too_small.struct_size = RODAQ_RUNTIME_PROJECTION_MIN_STRUCT_SIZE - 1;
        const rodaq_interface override{projection_interface.context, &too_small};
        const std::unique_ptr<ServiceRegistry> registry =
            make_registry(services, std::nullopt, &override);
        AcquisitionPluginInstance instance = take(manager.create_acquisition_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_ACQUISITION_PLUGIN")))));
        const auto result = instance.bind(registry->abi_view());
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(rodaq::StatusCode::InvalidArgument, result.status().status_code());
        instance.destroy();
    }
}

TEST(PluginServiceIntegrationTest, RequiredControlInterfacesAreQueriedAndValidated) {
    PluginManager manager;
    ConfigTree config = make_config();

    {
        RuntimeServices services;
        const std::unique_ptr<ServiceRegistry> registry = make_registry(services);
        AcquisitionPluginInstance instance = take(manager.create_acquisition_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_ACQUISITION_PLUGIN")))));
        ASSERT_TRUE(instance.bind(registry->abi_view()).ok());
        const auto control = instance.required_control_interface();
        ASSERT_TRUE(control.ok());
        ASSERT_NE(nullptr, control.value().context);
        const auto* vtable = static_cast<const rodaq_acquisition_control*>(control.value().vtable);
        ASSERT_NE(nullptr, vtable);
        EXPECT_EQ(RODAQ_ACQUISITION_CONTROL_PROTOCOL_VERSION, vtable->protocol_version);
        EXPECT_GE(vtable->struct_size, RODAQ_ACQUISITION_CONTROL_MIN_STRUCT_SIZE);
        EXPECT_NE(nullptr, vtable->prepare_binding_switch);
        EXPECT_NE(nullptr, vtable->commit_binding_switch);
        EXPECT_NE(nullptr, vtable->abort_binding_switch);
        EXPECT_NE(nullptr, vtable->quiesce_stream);
        EXPECT_NE(nullptr, vtable->quiesce_plugin);
        EXPECT_TRUE(instance.configure(rodaq::config_abi_view(config.root())).ok());

        const auto unknown = instance.query_interface("rodaq.test.unknown");
        ASSERT_FALSE(unknown.ok());
        EXPECT_EQ(rodaq::StatusCode::NotFound, unknown.status().status_code());
        instance.destroy();
    }

    {
        RuntimeServices services;
        register_fixture_projection(services);
        const std::unique_ptr<ServiceRegistry> registry = make_registry(services);
        StoragePluginInstance instance = take(manager.create_storage_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_STORAGE_PLUGIN")))));
        ASSERT_TRUE(instance.bind(registry->abi_view()).ok());
        const auto control = instance.required_control_interface();
        ASSERT_TRUE(control.ok());
        const auto* vtable = static_cast<const rodaq_storage_control*>(control.value().vtable);
        ASSERT_NE(nullptr, vtable);
        EXPECT_EQ(RODAQ_STORAGE_CONTROL_PROTOCOL_VERSION, vtable->protocol_version);
        EXPECT_GE(vtable->struct_size, RODAQ_STORAGE_CONTROL_MIN_STRUCT_SIZE);
        EXPECT_NE(nullptr, vtable->get_capabilities);
        EXPECT_NE(nullptr, vtable->ensure_descriptor_ready);
        EXPECT_TRUE(instance.configure(rodaq::config_abi_view(config.root())).ok());
        instance.destroy();
    }
}

TEST(PluginServiceIntegrationTest, LargerControlInterfaceIsAccepted) {
    PluginManager manager;
    RuntimeServices services;
    const std::unique_ptr<ServiceRegistry> registry = make_registry(services);
    AcquisitionPluginInstance instance = take(manager.create_acquisition_plugin_instance(
        take(manager.load_library(plugin_path("RODAQ_TEST_CONTROL_APPENDABLE")))));
    ASSERT_TRUE(instance.bind(registry->abi_view()).ok());
    const auto control = instance.required_control_interface();
    ASSERT_TRUE(control.ok());
    const auto* vtable = static_cast<const rodaq_acquisition_control*>(control.value().vtable);
    EXPECT_GT(vtable->struct_size, RODAQ_ACQUISITION_CONTROL_MIN_STRUCT_SIZE);
    instance.destroy();
}

TEST(PluginServiceIntegrationTest, IncompatibleControlInterfacesFailBind) {
    expect_acquisition_control_failure("RODAQ_TEST_CONTROL_WRONG_PROTOCOL");
    expect_acquisition_control_failure("RODAQ_TEST_CONTROL_TOO_SMALL");
    expect_acquisition_control_failure("RODAQ_TEST_CONTROL_MISSING_SLOT");
    expect_acquisition_control_failure("RODAQ_TEST_CONTROL_NULL_CONTEXT");
    expect_storage_control_failure("RODAQ_TEST_STORAGE_CONTROL_WRONG_PROTOCOL");
    expect_storage_control_failure("RODAQ_TEST_STORAGE_CONTROL_MISSING_SLOT");
}

TEST(PluginServiceIntegrationTest, SdkFixturesCompleteFullLifecycle) {
    PluginManager manager;
    ConfigTree config = make_config();

    {
        RuntimeServices services;
        const std::unique_ptr<ServiceRegistry> registry = make_registry(services);
        AcquisitionPluginInstance instance = take(manager.create_acquisition_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_SDK_ACQUISITION")))));
        EXPECT_EQ(rodaq::StatusCode::InvalidState, instance.start().status().status_code());
        ASSERT_TRUE(instance.bind(registry->abi_view()).ok());
        ASSERT_TRUE(instance.configure(rodaq::config_abi_view(config.root())).ok());
        ASSERT_TRUE(instance.start().ok());
        EXPECT_EQ(rodaq::StatusCode::InvalidState, instance.start().status().status_code());

        const auto control = instance.required_control_interface();
        ASSERT_TRUE(control.ok());
        const auto* table = static_cast<const rodaq_acquisition_control*>(control.value().vtable);
        rodaq_status error{};
        EXPECT_EQ(
            RODAQ_STATUS_OK,
            table->quiesce_stream(control.value().context, rodaq_stream_handle{1}, &error));
        EXPECT_EQ(RODAQ_STATUS_OK, table->quiesce_plugin(control.value().context, &error));
        ASSERT_TRUE(instance.stop().ok());
        EXPECT_EQ(rodaq::StatusCode::InvalidState, instance.stop().status().status_code());
        instance.destroy();
    }

    {
        RuntimeServices services;
        register_fixture_projection(services);
        const std::unique_ptr<ServiceRegistry> registry = make_registry(services);
        StoragePluginInstance instance = take(manager.create_storage_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_SDK_STORAGE")))));
        ASSERT_TRUE(instance.bind(registry->abi_view()).ok());
        ASSERT_TRUE(instance.configure(rodaq::config_abi_view(config.root())).ok());

        const auto control = instance.required_control_interface();
        ASSERT_TRUE(control.ok());
        const auto* table = static_cast<const rodaq_storage_control*>(control.value().vtable);
        rodaq_storage_capabilities capabilities{};
        capabilities.struct_size = sizeof(capabilities);
        rodaq_status error{};
        ASSERT_EQ(
            RODAQ_STATUS_OK,
            table->get_capabilities(control.value().context, &capabilities, &error));
        EXPECT_EQ(RODAQ_TRUE, capabilities.full_fidelity);
        rodaq_descriptor_readiness_result readiness{};
        readiness.struct_size = sizeof(readiness);
        ASSERT_EQ(
            RODAQ_STATUS_OK,
            table->ensure_descriptor_ready(
                control.value().context, rodaq_stream_descriptor_handle{42}, &readiness, &error));
        EXPECT_EQ(RODAQ_DESCRIPTOR_READY, readiness.state);

        rodaq_storage_open_request open{};
        open.struct_size = RODAQ_STORAGE_OPEN_REQUEST_MIN_STRUCT_SIZE;
        const char session[] = "session";
        const char target[] = "memory://fixture";
        open.session_id = rodaq_string_view{session, sizeof(session) - 1};
        open.target_uri = rodaq_string_view{target, sizeof(target) - 1};
        ASSERT_TRUE(instance.open(open).ok());
        EXPECT_EQ(rodaq::StatusCode::InvalidState, instance.open(open).status().status_code());

        rodaq_storage_submission submission{};
        submission.struct_size = sizeof(submission);
        submission.operation.value = 1;
        submission.stream.value = 1;
        submission.descriptor.value = 42;
        submission.source_present = RODAQ_FALSE;
        submission.source_sequence_present = RODAQ_FALSE;
        const auto submitted = instance.submit(submission);
        ASSERT_TRUE(submitted.ok());
        EXPECT_EQ(RODAQ_STORAGE_PAYLOAD_CONSUMED, submitted.value().payload_state);
        EXPECT_EQ(RODAQ_STORAGE_PERSISTENCE_SUCCEEDED, submitted.value().persistence_state);
        ASSERT_TRUE(instance.flush().ok());

        rodaq_storage_close_request close{};
        close.struct_size = RODAQ_STORAGE_CLOSE_REQUEST_MIN_STRUCT_SIZE;
        close.disposition = RODAQ_STORAGE_CLOSE_NORMAL;
        ASSERT_TRUE(instance.close(close).ok());
        EXPECT_EQ(rodaq::StatusCode::InvalidState, instance.close(close).status().status_code());
        instance.destroy();
    }
}

TEST(PluginServiceIntegrationTest, SdkLifecycleFailuresAndExceptionsAreContained) {
    PluginManager manager;
    ConfigTree config = make_config();

    {
        RuntimeServices services;
        const std::unique_ptr<ServiceRegistry> registry = make_registry(services);
        AcquisitionPluginInstance instance = take(manager.create_acquisition_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_SDK_EXCEPTION")))));
        ASSERT_TRUE(instance.bind(registry->abi_view()).ok());
        ASSERT_TRUE(instance.configure(rodaq::config_abi_view(config.root())).ok());
        const auto result = instance.start();
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(rodaq::StatusCode::InternalError, result.status().status_code());
        EXPECT_NE(std::string_view::npos, result.status().message().find("exception"));
        instance.destroy();
    }

    {
        RuntimeServices services;
        register_fixture_projection(services);
        const std::unique_ptr<ServiceRegistry> registry = make_registry(services);
        StoragePluginInstance instance = take(manager.create_storage_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_SDK_OPEN_FAILURE")))));
        ASSERT_TRUE(instance.bind(registry->abi_view()).ok());
        ASSERT_TRUE(instance.configure(rodaq::config_abi_view(config.root())).ok());
        rodaq_storage_open_request open{};
        open.struct_size = RODAQ_STORAGE_OPEN_REQUEST_MIN_STRUCT_SIZE;
        const auto result = instance.open(open);
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(rodaq::StatusCode::Unavailable, result.status().status_code());
        instance.destroy();
    }
}

TEST(PluginServiceIntegrationTest, IndependentCurrentAbiPluginsLoadInCurrentHost) {
    PluginManager manager;
    ConfigTree config = make_config();

    {
        RuntimeServices services;
        const std::unique_ptr<ServiceRegistry> registry = make_registry(services);
        AcquisitionPluginInstance instance = take(manager.create_acquisition_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_CURRENT_ABI_ACQUISITION")))));
        ASSERT_TRUE(instance.bind(registry->abi_view()).ok());
        ASSERT_TRUE(instance.configure(rodaq::config_abi_view(config.root())).ok());
        EXPECT_TRUE(instance.required_control_interface().ok());
        instance.destroy();
    }
    {
        RuntimeServices services;
        register_fixture_projection(services);
        const std::unique_ptr<ServiceRegistry> registry = make_registry(services);
        StoragePluginInstance instance = take(manager.create_storage_plugin_instance(
            take(manager.load_library(plugin_path("RODAQ_TEST_CURRENT_ABI_STORAGE")))));
        ASSERT_TRUE(instance.bind(registry->abi_view()).ok());
        ASSERT_TRUE(instance.configure(rodaq::config_abi_view(config.root())).ok());
        EXPECT_TRUE(instance.required_control_interface().ok());
        instance.destroy();
    }
}
