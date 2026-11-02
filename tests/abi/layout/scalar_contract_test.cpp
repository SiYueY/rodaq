#include "scalar_checks.h"

#include <rodaq/error.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace {

void persistence_completed(
    void*, rodaq_storage_operation, rodaq_storage_persistence_state, const rodaq_error*) noexcept {}

void report_failure(void*, const rodaq_error*) noexcept {}

}  // namespace

TEST(ScalarContractTest, ServiceAndInterfaceIdsMatchFrozenStrings) {
    EXPECT_STREQ(RODAQ_RUNTIME_COMMON_SERVICE_ID, "rodaq.runtime.common.v1");
    EXPECT_STREQ(RODAQ_RUNTIME_CANONICAL_SERVICE_ID, "rodaq.runtime.canonical.v1");
    EXPECT_STREQ(RODAQ_RUNTIME_ACQUISITION_SERVICE_ID, "rodaq.runtime.acquisition.v1");
    EXPECT_STREQ(RODAQ_RUNTIME_STORAGE_SERVICE_ID, "rodaq.runtime.storage.v1");
    EXPECT_STREQ(RODAQ_ACQUISITION_CONTROL_INTERFACE_ID, "rodaq.acquisition.control.v1");
    EXPECT_STREQ(RODAQ_STORAGE_CONTROL_INTERFACE_ID, "rodaq.storage.control.v1");
}

TEST(ScalarContractTest, StorageRuntimeCallbacksUseFullDiagnostics) {
    rodaq_runtime_storage_v1 storage_service = {};
    storage_service.persistence_completed = persistence_completed;
    storage_service.report_failure = report_failure;

    EXPECT_NE(storage_service.persistence_completed, nullptr);
    EXPECT_NE(storage_service.report_failure, nullptr);
}

TEST(ScalarContractTest, CppErrorTaxonomyMatchesStableStatusValues) {
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::InvalidArgument), 1U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::InvalidState), 2U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::NotFound), 3U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::Conflict), 4U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::ResourceExhausted), 5U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::Unavailable), 6U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::StorageError), 7U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::Timeout), 8U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::InternalError), 9U);
}
