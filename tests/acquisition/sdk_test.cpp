#include <rodaq/acquisition/interface.hpp>
#include <rodaq/acquisition/service.hpp>
#include <rodaq/plugin/authoring.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

namespace acq = rodaq::acquisition;
namespace plugin = rodaq::plugin;

acq::RecordAttempt record() {
    acq::RecordAttempt result{};
    result.header.struct_size = sizeof(result);
    result.stream_id = 1;
    result.descriptor_token = {1, 1};
    result.payload.header.struct_size = sizeof(result.payload);
    return result;
}

class Host {
public:
    rodaq::Result<acq::SubmitResult> submit_record(const acq::RecordAttempt& value) {
        ++calls;
        if (throw_error) {
            throw std::runtime_error("callback failure");
        }
        if (failure) {
            return rodaq::Status(rodaq::StatusCode::Unavailable, "callback unavailable");
        }
        if (value.payload.logical_size == 0) {
            copied.clear();
        } else {
            copied.assign(value.payload.data, value.payload.data + value.payload.logical_size);
        }
        return acq::SubmitResult{{sizeof(acq::SubmitResult)}, disposition, 0};
    }

    rodaq::Result<acq::DescriptorToken> propose_descriptor(
        const acq::DescriptorProposal&, plugin::Deadline) {
        return acq::DescriptorToken{1, 1};
    }

    rodaq::Result<void> report_source_event(const acq::SourceEvent&) { return {}; }

    rodaq::Result<void> report_acquisition_event(const acq::AcquisitionEvent&) { return {}; }

    bool failure{false};
    bool throw_error{false};
    std::uint32_t calls{0};
    rodaq_submit_disposition_t disposition{RODAQ_SUBMIT_ACCEPTED};
    std::vector<std::uint8_t> copied;
};

class Reader final : public plugin::PluginBase {
public:
    rodaq::Result<void> configure(plugin::PluginSetup& setup) override {
        auto result = setup.service<acq::Service>();
        if (!result.ok()) {
            return result.status();
        }
        reference = std::move(result).value();
        return {};
    }

    rodaq::Result<void> shutdown(plugin::Deadline) override { return {}; }

    std::optional<acq::ServiceReference> reference;
};

class AcquisitionSdk : public testing::Test {
protected:
    static rodaq_status_code_t query(
        rodaq_service_registry_context_t* context, const rodaq_interface_query_t* request,
        rodaq_interface_reference_t* result, rodaq_error_t* error) noexcept {
        return plugin::authoring::query_interface(
            reinterpret_cast<AcquisitionSdk*>(context)->entries_, request, result, error);
    }

    void SetUp() override {
        auto binding = plugin::authoring::bind_interface<acq::Service>(host_);
        ASSERT_TRUE(binding.ok());
        entries_.push_back(std::move(binding).value());
        ASSERT_EQ(
            plugin::authoring::Instance<Reader>::create(&instance_, nullptr), RODAQ_STATUS_OK);
        const rodaq_config_t config{{sizeof(config)}, {nullptr, 0}};
        ASSERT_EQ(
            instance_.vtable->configure(instance_.context, &registry_, &config, nullptr),
            RODAQ_STATUS_OK);
    }

    void TearDown() override {
        if (instance_.context != nullptr) {
            EXPECT_EQ(
                instance_.vtable->shutdown(instance_.context, {RODAQ_DEADLINE_INFINITE}, nullptr),
                RODAQ_STATUS_OK);
            instance_.vtable->destroy(instance_.context);
        }
    }

    acq::ServiceReference& reference() {
        return *reinterpret_cast<plugin::authoring::Instance<Reader>*>(instance_.context)
                    ->plugin.reference;
    }

    Host host_;
    std::vector<plugin::authoring::InterfaceEntry> entries_;
    rodaq_plugin_instance_t instance_{};
    const rodaq_service_registry_vtable_t table_{
        {offsetof(rodaq_service_registry_vtable_t, query) + sizeof(table_.query)},
        query,
        nullptr,
        nullptr};
    const rodaq_service_registry_reference_t registry_{
        reinterpret_cast<rodaq_service_registry_context_t*>(this), &table_};
};

TEST_F(AcquisitionSdk, CopyCompletesBeforeCallbackReturnsAndRejectIsSuccessfulDisposition) {
    std::array<std::uint8_t, 4> source{1, 2, 3, 4};
    auto value = record();
    value.payload.data = source.data();
    value.payload.logical_size = source.size();
    auto result = reference().submit_record(value);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().disposition, RODAQ_SUBMIT_ACCEPTED);
    source.fill(9);
    EXPECT_EQ(host_.copied, (std::vector<std::uint8_t>{1, 2, 3, 4}));
    host_.disposition = RODAQ_SUBMIT_GATE_CLOSED;
    result = reference().submit_record(value);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().disposition, RODAQ_SUBMIT_GATE_CLOSED);
}

TEST_F(AcquisitionSdk, NonOkKeepsDefaultInvalidAndExceptionsDoNotEscape) {
    auto value = record();
    acq::SubmitResult output{{sizeof(output)}, RODAQ_SUBMIT_ACCEPTED, 99};
    auto* context = reinterpret_cast<rodaq_interface_context_t*>(&host_);
    const auto* table = acq::ServiceProvider<Host>::vtable();
    host_.failure = true;
    EXPECT_EQ(table->submit_record(context, &value, &output), RODAQ_STATUS_UNAVAILABLE);
    EXPECT_EQ(output.disposition, RODAQ_SUBMIT_INVALID);
    EXPECT_EQ(output.reserved0, 0);
    host_.throw_error = true;
    EXPECT_EQ(reference().submit_record(value).status().code(), rodaq::StatusCode::Internal);
    host_.throw_error = false;
    host_.failure = false;
    host_.disposition = 999;
    EXPECT_EQ(reference().submit_record(value).status().code(), rodaq::StatusCode::Internal);
}

TEST_F(AcquisitionSdk, CallerCapacityIsCheckedBeforeAnyOutputWrite) {
    auto value = record();
    acq::SubmitResult output{{0}, 123, 456};
    const auto* table = acq::ServiceProvider<Host>::vtable();
    EXPECT_EQ(
        table->submit_record(reinterpret_cast<rodaq_interface_context_t*>(&host_), &value, &output),
        RODAQ_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(output.disposition, 123);
    EXPECT_EQ(output.reserved0, 456);
    EXPECT_EQ(host_.calls, 0);
}

TEST_F(AcquisitionSdk, HealthyReadinessReportReturnsSuccess) {
    acq::AcquisitionEvent event{};
    event.header.struct_size = sizeof(event);
    event.kind = RODAQ_ACQUISITION_EVENT_TRANSPORT_READY;
    EXPECT_TRUE(reference().report_acquisition_event(event).ok());
}

TEST(AcquisitionValidation, RecordFlagsUnusedTimestampsAndAbsentFactsFailClosed) {
    const auto original = record();
    ASSERT_TRUE(acq::abi::valid_record(&original));
    auto value = original;
    value.delivery_flags = 2;
    EXPECT_FALSE(acq::abi::valid_record(&value));
    value = original;
    value.timestamps[3].value = 1;
    EXPECT_FALSE(acq::abi::valid_record(&value));
    value = original;
    value.source_sequence = 1;
    EXPECT_FALSE(acq::abi::valid_record(&value));
    value = original;
    value.has_source_id = 2;
    EXPECT_FALSE(acq::abi::valid_record(&value));
    value = original;
    value.header.struct_size = sizeof(value) - 1;
    EXPECT_FALSE(acq::abi::valid_record(&value));
}

TEST(AcquisitionValidation, EventScopeAndStatusVocabularyAreExplicit) {
    acq::AcquisitionEvent event{};
    event.header.struct_size = sizeof(event);
    event.kind = RODAQ_ACQUISITION_EVENT_TRANSPORT_READY;
    EXPECT_TRUE(acq::abi::valid_acquisition_event(&event));
    event.kind = RODAQ_ACQUISITION_EVENT_OVERFLOW;
    EXPECT_FALSE(acq::abi::valid_acquisition_event(&event));
    event.has_stream_id = 1;
    event.stream_id = 1;
    event.count = 3;
    EXPECT_TRUE(acq::abi::valid_acquisition_event(&event));
    event.status = RODAQ_STATUS_UNAVAILABLE;
    EXPECT_FALSE(acq::abi::valid_acquisition_event(&event));
    event.kind = RODAQ_ACQUISITION_EVENT_RECOVERABLE_FAILURE;
    event.count = 0;
    EXPECT_TRUE(acq::abi::valid_acquisition_event(&event));
    event.status = 99;
    EXPECT_FALSE(acq::abi::valid_acquisition_event(&event));
}

TEST(AcquisitionValidation, ScopeRequiresItsDeclaredIdentityAndOpaqueCannotProveGaps) {
    auto value = record();
    value.has_source_sequence = 1;
    value.sequence_scope = RODAQ_SEQUENCE_SCOPE_OPAQUE;
    EXPECT_TRUE(acq::abi::valid_record(&value));
    value.sequence_scope = RODAQ_SEQUENCE_SCOPE_SOURCE;
    EXPECT_FALSE(acq::abi::valid_record(&value));
    value.has_source_id = 1;
    value.source_id.kind = RODAQ_SOURCE_ID_RTPS_GUID;
    value.source_id.size = 16;
    EXPECT_TRUE(acq::abi::valid_record(&value));
    value.sequence_scope = RODAQ_SEQUENCE_SCOPE_SOURCE_SESSION;
    EXPECT_FALSE(acq::abi::valid_record(&value));
    value.has_source_session_id = 1;
    value.source_session_id = 1;
    EXPECT_TRUE(acq::abi::valid_record(&value));
    acq::SourceEvent event{};
    event.header.struct_size = sizeof(event);
    event.stream_id = 1;
    event.kind = RODAQ_SOURCE_EVENT_SEQUENCE_GAP;
    event.has_source_sequence = 1;
    event.sequence_scope = RODAQ_SEQUENCE_SCOPE_OPAQUE;
    EXPECT_FALSE(acq::abi::valid_source_event(&event));
}

TEST(AcquisitionValidation, GenericSchemaSupportStaysWithBackendWhileRos2RequiresSchema) {
    acq::DescriptorProposal proposal{};
    proposal.header.struct_size = sizeof(proposal);
    proposal.stream_id = 1;
    proposal.channel_name = {"sample", 6};
    proposal.message_encoding = {"raw", 3};
    proposal.data_profile = RODAQ_DATA_PROFILE_GENERIC;
    EXPECT_TRUE(acq::abi::valid_descriptor(&proposal));
    proposal.data_profile = RODAQ_DATA_PROFILE_ROS2;
    proposal.message_encoding = {"cdr", 3};
    EXPECT_FALSE(acq::abi::valid_descriptor(&proposal));
    proposal.schema_name = {"std_msgs/msg/Int32", 18};
    proposal.schema_encoding = {"ros2msg", 7};
    const std::uint8_t schema[] = {'i', 'n', 't'};
    proposal.schema = {schema, sizeof(schema)};
    EXPECT_TRUE(acq::abi::valid_descriptor(&proposal));
    rodaq_ros2_qos_profile_t qos{};
    proposal.ros2_offered_qos_profiles = &qos;
    proposal.ros2_offered_qos_profile_count = 1;
    EXPECT_TRUE(acq::abi::valid_descriptor(&proposal));
    qos.reliability = 99;
    EXPECT_FALSE(acq::abi::valid_descriptor(&proposal));
    qos.reliability = RODAQ_ROS2_QOS_RELIABILITY_RELIABLE;
    qos.deadline.nsec = 1000000000;
    EXPECT_FALSE(acq::abi::valid_descriptor(&proposal));
}

struct ControlBackend {
    rodaq::Result<void> prepare(const acq::PrepareConfig&, plugin::Deadline budget) {
        deadline = budget.native().monotonic_deadline;
        return {};
    }

    rodaq::Result<void> start(plugin::Deadline budget) {
        deadline = budget.native().monotonic_deadline;
        return {};
    }

    rodaq::Result<void> quiesce(plugin::Deadline budget) {
        deadline = budget.native().monotonic_deadline;
        return {};
    }

    std::int64_t deadline{0};
};

TEST(AcquisitionSdkControl, NativeAbsoluteBudgetIsPreservedAndNegativeBudgetRejected) {
    ControlBackend backend;
    auto* context = reinterpret_cast<rodaq_interface_context_t*>(&backend);
    const auto* table = acq::Provider<ControlBackend>::vtable();
    EXPECT_EQ(table->start(context, {12345}, nullptr), RODAQ_STATUS_OK);
    EXPECT_EQ(backend.deadline, 12345);
    EXPECT_EQ(table->quiesce(context, {-1}, nullptr), RODAQ_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(backend.deadline, 12345);
}

}  // namespace
