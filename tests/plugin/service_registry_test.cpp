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

    ASSERT_TRUE(registry.Register("rodaq.test.first.v1", first).has_value());
    ASSERT_TRUE(registry.Register("rodaq.test.second.v1", second).has_value());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, registry.Query("rodaq.test.first.v1").error().code());
    ASSERT_TRUE(registry.Freeze().has_value());

    const auto queried_first = registry.Query("rodaq.test.first.v1");
    const auto queried_second = registry.Query("rodaq.test.second.v1");
    ASSERT_TRUE(queried_first.has_value());
    ASSERT_TRUE(queried_second.has_value());
    EXPECT_EQ(first.context, queried_first.value().context);
    EXPECT_EQ(first.vtable, queried_first.value().vtable);
    EXPECT_EQ(second.context, queried_second.value().context);
    EXPECT_EQ(second.vtable, queried_second.value().vtable);
}

TEST(ServiceRegistryTest, FreezeIsIdempotentAndPreventsRegistration) {
    auto registry = make_registry();
    ASSERT_TRUE(registry.Freeze().has_value());
    ASSERT_TRUE(registry.Freeze().has_value());

    int context = 0;
    int vtable = 0;
    const auto result =
        registry.Register("rodaq.test.after_freeze.v1", make_service(&context, &vtable));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, result.error().code());
}

TEST(ServiceRegistryTest, RejectsInvalidIdsDuplicatesAndCapacityExhaustion) {
    auto registry = make_registry(2, 8);
    int context = 0;
    int vtable = 0;
    const rodaq_interface service = make_service(&context, &vtable);

    EXPECT_EQ(rodaq::ErrorCode::InvalidArgument, registry.Register("", service).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument,
        registry.Register(std::string_view("\xFF", 1), service).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument,
        registry.Register("far.too.long", service).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument,
        registry.Register("valid.id", rodaq_interface{}).error().code());

    ASSERT_TRUE(registry.Register("one", service).has_value());
    EXPECT_EQ(rodaq::ErrorCode::Conflict, registry.Register("one", service).error().code());
    ASSERT_TRUE(registry.Register("two", service).has_value());
    EXPECT_EQ(
        rodaq::ErrorCode::ResourceExhausted, registry.Register("tri", service).error().code());
}

TEST(ServiceRegistryTest, QueryReportsNotFoundAndRejectsInvalidIds) {
    auto registry = make_registry();
    ASSERT_TRUE(registry.Freeze().has_value());

    EXPECT_EQ(rodaq::ErrorCode::NotFound, registry.Query("missing").error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidArgument, registry.Query("").error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument,
        registry.Query(std::string_view("\xFF", 1)).error().code());
}

TEST(ServiceRegistryTest, AbiViewReturnsProtocolAndQueryBehavior) {
    auto registry = make_registry();
    int context = 0;
    int vtable = 0;
    const rodaq_interface service = make_service(&context, &vtable);
    ASSERT_TRUE(registry.Register("rodaq.test.abi.v1", service).has_value());
    ASSERT_TRUE(registry.Freeze().has_value());

    const rodaq_service_registry abi = registry.AbiView();
    ASSERT_NE(nullptr, abi.context);
    ASSERT_NE(nullptr, abi.vtable);
    EXPECT_EQ(RODAQ_SERVICE_REGISTRY_PROTOCOL_VERSION, abi.vtable->protocol_version);
    EXPECT_EQ(RODAQ_SERVICE_REGISTRY_VTABLE_MIN_STRUCT_SIZE, abi.vtable->struct_size);

    rodaq_interface out{};
    rodaq_error error{};
    EXPECT_EQ(
        RODAQ_STATUS_OK, abi.vtable->query(abi.context, view("rodaq.test.abi.v1"), &out, &error));
    EXPECT_EQ(service.context, out.context);
    EXPECT_EQ(service.vtable, out.vtable);

    out = make_service(&context, &vtable);
    EXPECT_EQ(
        RODAQ_STATUS_NOT_FOUND, abi.vtable->query(abi.context, view("missing"), &out, &error));
    EXPECT_EQ(nullptr, out.context);
    EXPECT_EQ(nullptr, out.vtable);
    EXPECT_EQ(RODAQ_STATUS_NOT_FOUND, error.code);
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
    const rodaq::Error error(
        rodaq::ErrorCode::Unavailable, std::string(RODAQ_ERROR_DOMAIN_CAPACITY + 4, 'd'), -7,
        std::string(RODAQ_ERROR_MESSAGE_CAPACITY + 8, 'm'));

    rodaq_error output{};
    rodaq::assign_rodaq_error(error, &output);

    EXPECT_EQ(RODAQ_STATUS_UNAVAILABLE, output.code);
    EXPECT_EQ(-7, output.native_code);
    EXPECT_EQ(RODAQ_ERROR_DOMAIN_CAPACITY, output.domain_size);
    EXPECT_EQ(RODAQ_ERROR_MESSAGE_CAPACITY, output.message_size);
    EXPECT_NE(0U, output.flags & RODAQ_ERROR_FLAG_MESSAGE_TRUNCATED);
    EXPECT_EQ('d', output.domain[0]);
    EXPECT_EQ('m', output.message[0]);
}

TEST(AbiSupportTest, CallerOwnedErrorSurvivesCalleeLifetime) {
    rodaq_error output{};
    {
        const rodaq::Error temporary(
            rodaq::ErrorCode::Unavailable, std::string("temporary-domain"), 1234,
            std::string("temporary-message"));
        rodaq::assign_rodaq_error(temporary, &output);
    }
    for (int index = 0; index < 32; ++index) {
        const std::string churn(4096, static_cast<char>('a' + index));
        ASSERT_EQ(4096U, churn.size());
    }
    EXPECT_EQ(RODAQ_STATUS_UNAVAILABLE, output.code);
    EXPECT_EQ(1234, output.native_code);
    EXPECT_EQ("temporary-domain", std::string_view(output.domain, output.domain_size));
    EXPECT_EQ("temporary-message", std::string_view(output.message, output.message_size));
}
