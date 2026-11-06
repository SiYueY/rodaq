#include "plugin/service_registry.hpp"

#include <rodaq/plugin/abi_support.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace {

rodaq::plugin::ServiceRegistry make_registry(std::size_t count = 4, std::size_t id_bytes = 64) {
    return rodaq::plugin::ServiceRegistry(count, id_bytes);
}

rodaq_interface make_service(int* context, int* vtable) noexcept {
    return rodaq_interface{context, vtable};
}

rodaq_string_view view(std::string_view text) noexcept {
    return rodaq_string_view{text.data(), static_cast<std::uint64_t>(text.size())};
}

}  // namespace

TEST(ServiceRegistryTest, RegistersAndQueriesBorrowedInterfaces) {
    auto registry = make_registry();
    int first_context = 0;
    int first_vtable = 0;
    int second_context = 0;
    int second_vtable = 0;
    const rodaq_interface first = make_service(&first_context, &first_vtable);
    const rodaq_interface second = make_service(&second_context, &second_vtable);

    ASSERT_TRUE(registry.register_entry("rodaq.test.first.v1", first).ok());
    ASSERT_TRUE(registry.register_entry("rodaq.test.second.v1", second).ok());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState,
        registry.query("rodaq.test.first.v1").status().status_code());
    ASSERT_TRUE(registry.freeze().ok());

    const auto queried_first = registry.query("rodaq.test.first.v1");
    const auto queried_second = registry.query("rodaq.test.second.v1");
    ASSERT_TRUE(queried_first.ok());
    ASSERT_TRUE(queried_second.ok());
    EXPECT_EQ(first.context, queried_first.value().context);
    EXPECT_EQ(first.vtable, queried_first.value().vtable);
    EXPECT_EQ(second.context, queried_second.value().context);
    EXPECT_EQ(second.vtable, queried_second.value().vtable);
}

TEST(ServiceRegistryTest, FreezeIsIdempotentAndPreventsRegistration) {
    auto registry = make_registry();
    ASSERT_TRUE(registry.freeze().ok());
    ASSERT_TRUE(registry.freeze().ok());

    int context = 0;
    int vtable = 0;
    const auto result =
        registry.register_entry("rodaq.test.after_freeze.v1", make_service(&context, &vtable));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, result.status().status_code());
}

TEST(ServiceRegistryTest, RejectsInvalidIdsDuplicatesAndCapacityExhaustion) {
    auto registry = make_registry(2, 8);
    int context = 0;
    int vtable = 0;
    const rodaq_interface service = make_service(&context, &vtable);

    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument,
        registry.register_entry("", service).status().status_code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument,
        registry.register_entry(std::string_view("\xFF", 1), service).status().status_code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument,
        registry.register_entry("far.too.long", service).status().status_code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument,
        registry.register_entry("valid.id", rodaq_interface{}).status().status_code());

    ASSERT_TRUE(registry.register_entry("one", service).ok());
    EXPECT_EQ(
        rodaq::StatusCode::Conflict,
        registry.register_entry("one", service).status().status_code());
    ASSERT_TRUE(registry.register_entry("two", service).ok());
    EXPECT_EQ(
        rodaq::StatusCode::ResourceExhausted,
        registry.register_entry("tri", service).status().status_code());
}

TEST(ServiceRegistryTest, QueryReportsNotFoundAndRejectsInvalidIds) {
    auto registry = make_registry();
    ASSERT_TRUE(registry.freeze().ok());

    EXPECT_EQ(rodaq::StatusCode::NotFound, registry.query("missing").status().status_code());
    EXPECT_EQ(rodaq::StatusCode::InvalidArgument, registry.query("").status().status_code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument,
        registry.query(std::string_view("\xFF", 1)).status().status_code());
}

TEST(ServiceRegistryTest, AbiViewReturnsProtocolAndQueryBehavior) {
    auto registry = make_registry();
    int context = 0;
    int vtable = 0;
    const rodaq_interface service = make_service(&context, &vtable);
    ASSERT_TRUE(registry.register_entry("rodaq.test.abi.v1", service).ok());
    ASSERT_TRUE(registry.freeze().ok());

    const rodaq_service_registry abi = registry.abi_view();
    ASSERT_NE(nullptr, abi.context);
    ASSERT_NE(nullptr, abi.vtable);
    EXPECT_EQ(RODAQ_SERVICE_REGISTRY_PROTOCOL_VERSION, abi.vtable->protocol_version);
    EXPECT_EQ(RODAQ_SERVICE_REGISTRY_VTABLE_MIN_STRUCT_SIZE, abi.vtable->struct_size);

    rodaq_interface out{};
    rodaq_status error{};
    EXPECT_EQ(
        RODAQ_STATUS_OK, abi.vtable->query(abi.context, view("rodaq.test.abi.v1"), &out, &error));
    EXPECT_EQ(service.context, out.context);
    EXPECT_EQ(service.vtable, out.vtable);

    out = make_service(&context, &vtable);
    EXPECT_EQ(
        RODAQ_STATUS_NOT_FOUND, abi.vtable->query(abi.context, view("missing"), &out, &error));
    EXPECT_EQ(nullptr, out.context);
    EXPECT_EQ(nullptr, out.vtable);
    EXPECT_EQ(RODAQ_STATUS_NOT_FOUND, error.status_code);
    EXPECT_NE(0U, error.domain_size);
    EXPECT_NE(0U, error.message_size);

    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        abi.vtable->query(abi.context, view("rodaq.test.abi.v1"), nullptr, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        abi.vtable->query(nullptr, view("rodaq.test.abi.v1"), &out, &error));
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        abi.vtable->query(abi.context, view(std::string_view("\xFF", 1)), &out, &error));
}

TEST(AbiSupportTest, ConvertsAndTruncatesCallerOwnedDiagnostics) {
    const rodaq::Status error(
        rodaq::StatusCode::Unavailable, std::string(RODAQ_STATUS_DOMAIN_CAPACITY + 4, 'd'), -7,
        std::string(RODAQ_STATUS_MESSAGE_CAPACITY + 8, 'm'));

    rodaq_status output{};
    rodaq::assign_abi_status(error, &output);

    EXPECT_EQ(RODAQ_STATUS_UNAVAILABLE, output.status_code);
    EXPECT_EQ(-7, output.error_code);
    EXPECT_EQ(RODAQ_STATUS_DOMAIN_CAPACITY, output.domain_size);
    EXPECT_EQ(RODAQ_STATUS_MESSAGE_CAPACITY, output.message_size);
    EXPECT_NE(0U, output.flags & RODAQ_STATUS_FLAG_MESSAGE_TRUNCATED);
    EXPECT_EQ('d', output.domain[0]);
    EXPECT_EQ('m', output.message[0]);
}

TEST(AbiSupportTest, CallerOwnedErrorSurvivesCalleeLifetime) {
    rodaq_status output{};
    {
        const rodaq::Status temporary(
            rodaq::StatusCode::Unavailable, std::string("temporary-domain"), 1234,
            std::string("temporary-message"));
        rodaq::assign_abi_status(temporary, &output);
    }
    for (int index = 0; index < 32; ++index) {
        const std::string churn(4096, static_cast<char>('a' + index));
        ASSERT_EQ(4096U, churn.size());
    }
    EXPECT_EQ(RODAQ_STATUS_UNAVAILABLE, output.status_code);
    EXPECT_EQ(1234, output.error_code);
    EXPECT_EQ("temporary-domain", std::string_view(output.domain, output.domain_size));
    EXPECT_EQ("temporary-message", std::string_view(output.message, output.message_size));
}

TEST(AbiSupportTest, ConvertsAllStatusCodesAndHandlesInvalidDiagnostics) {
    struct Mapping {
        rodaq::StatusCode cpp_status;
        rodaq_status_code abi_status;
    };
    const Mapping mappings[] = {
        {rodaq::StatusCode::Ok, RODAQ_STATUS_OK},
        {rodaq::StatusCode::InvalidArgument, RODAQ_STATUS_INVALID_ARGUMENT},
        {rodaq::StatusCode::InvalidState, RODAQ_STATUS_INVALID_STATE},
        {rodaq::StatusCode::NotFound, RODAQ_STATUS_NOT_FOUND},
        {rodaq::StatusCode::Conflict, RODAQ_STATUS_CONFLICT},
        {rodaq::StatusCode::ResourceExhausted, RODAQ_STATUS_RESOURCE_EXHAUSTED},
        {rodaq::StatusCode::Unavailable, RODAQ_STATUS_UNAVAILABLE},
        {rodaq::StatusCode::StorageError, RODAQ_STATUS_STORAGE_ERROR},
        {rodaq::StatusCode::Timeout, RODAQ_STATUS_TIMEOUT},
        {rodaq::StatusCode::InternalError, RODAQ_STATUS_INTERNAL_ERROR},
        {rodaq::StatusCode::UnknownError, RODAQ_STATUS_UNKNOWN_ERROR},
    };
    for (const auto& mapping : mappings) {
        EXPECT_EQ(static_cast<std::uint32_t>(mapping.cpp_status), mapping.abi_status);
        EXPECT_EQ(rodaq::abi_status_from_status_code(mapping.cpp_status), mapping.abi_status);
        auto result = rodaq::status_from_abi(mapping.abi_status, nullptr, "test");
        if (mapping.cpp_status == rodaq::StatusCode::Ok) {
            EXPECT_TRUE(result.ok());
        } else {
            ASSERT_FALSE(result.ok());
            EXPECT_EQ(result.status_code(), mapping.cpp_status);
        }
    }
    EXPECT_EQ(
        rodaq::abi_status_from_status_code(static_cast<rodaq::StatusCode>(42U)),
        RODAQ_STATUS_UNKNOWN_ERROR);
    auto unknown = rodaq::status_from_abi(42U, nullptr, "test");
    ASSERT_FALSE(unknown.ok());
    EXPECT_EQ(unknown.status_code(), rodaq::StatusCode::UnknownError);

    rodaq_status diagnostic{};
    diagnostic.status_code = RODAQ_STATUS_NOT_FOUND;
    auto mismatch = rodaq::status_from_abi(RODAQ_STATUS_TIMEOUT, &diagnostic, "test");
    ASSERT_FALSE(mismatch.ok());
    EXPECT_EQ(mismatch.status_code(), rodaq::StatusCode::InternalError);
    EXPECT_EQ(mismatch.message(), "Plugin returned a malformed ABI diagnostic");
    diagnostic.status_code = RODAQ_STATUS_TIMEOUT;
    diagnostic.reserved[0] = 1;
    auto malformed = rodaq::status_from_abi(RODAQ_STATUS_TIMEOUT, &diagnostic, "test");
    ASSERT_FALSE(malformed.ok());
    EXPECT_EQ(malformed.status_code(), rodaq::StatusCode::InternalError);
    EXPECT_TRUE(rodaq::status_from_abi(RODAQ_STATUS_OK, &diagnostic, "test").ok());
}

TEST(AbiSupportTest, StatusRoundTripsDiagnosticsAndNormalizesUnknownCode) {
    const rodaq::Status original(rodaq::StatusCode::Unavailable, "dds", -37, "failed");
    rodaq_status diagnostic{};
    rodaq::assign_abi_status(original, &diagnostic);
    EXPECT_EQ(diagnostic.status_code, RODAQ_STATUS_UNAVAILABLE);
    EXPECT_EQ(diagnostic.error_code, -37);
    const auto restored = rodaq::status_from_abi(diagnostic.status_code, &diagnostic);
    EXPECT_EQ(restored.status_code(), rodaq::StatusCode::Unavailable);
    EXPECT_EQ(restored.domain(), "dds");
    EXPECT_EQ(restored.error_code(), -37);
    EXPECT_EQ(restored.message(), "failed");
    EXPECT_TRUE(rodaq::status_from_abi(RODAQ_STATUS_OK, nullptr).ok());
    EXPECT_EQ(rodaq::status_from_abi(42U, nullptr).status_code(), rodaq::StatusCode::UnknownError);
}

TEST(AbiSupportTest, CanonicalizesSuccessAndUnknownDiagnosticStatus) {
    rodaq_status output{};
    rodaq::assign_abi_status(RODAQ_STATUS_OK, "ignored", -12, "ignored", &output);
    EXPECT_EQ(output.status_code, RODAQ_STATUS_OK);
    EXPECT_EQ(output.error_code, 0);
    EXPECT_EQ(output.domain_size, 0U);
    EXPECT_EQ(output.message_size, 0U);
    rodaq::assign_abi_status(42U, "backend", -9, "unknown", &output);
    auto status = rodaq::status_from_abi(42U, &output);
    EXPECT_EQ(status.status_code(), rodaq::StatusCode::UnknownError);
    EXPECT_EQ(status.error_code(), -9);
    EXPECT_EQ(status.domain(), "backend");
    EXPECT_EQ(status.message(), "unknown");
    output.reserved[0] = 1;
    EXPECT_EQ(rodaq::status_from_abi(42U, &output).status_code(), rodaq::StatusCode::InternalError);
}
