#include "scalar_checks.h"

#include <rodaq/status.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace {

void persistence_completed(
    void*, rodaq_storage_operation, rodaq_storage_persistence_state, const rodaq_status*) noexcept {
}

void report_failure(void*, const rodaq_status*) noexcept {}

}  // namespace

TEST(ScalarContractTest, ServiceAndInterfaceIdsMatchFrozenStrings) {
    EXPECT_STREQ(RODAQ_RUNTIME_COMMON_SERVICE_ID, "rodaq.runtime.common");
    EXPECT_STREQ(RODAQ_RUNTIME_CANONICAL_SERVICE_ID, "rodaq.runtime.canonical");
    EXPECT_STREQ(RODAQ_RUNTIME_PROJECTION_SERVICE_ID, "rodaq.runtime.projection");
    EXPECT_STREQ(RODAQ_RUNTIME_CLOCK_CORRELATION_SERVICE_ID, "rodaq.runtime.clock_correlation");
    EXPECT_STREQ(RODAQ_RUNTIME_ACQUISITION_SERVICE_ID, "rodaq.runtime.acquisition");
    EXPECT_STREQ(RODAQ_RUNTIME_STORAGE_SERVICE_ID, "rodaq.runtime.storage");
    EXPECT_STREQ(RODAQ_ACQUISITION_CONTROL_INTERFACE_ID, "rodaq.acquisition.control");
    EXPECT_STREQ(RODAQ_STORAGE_CONTROL_INTERFACE_ID, "rodaq.storage.control");
}

TEST(ScalarContractTest, StorageServiceCallbacksUseFullDiagnostics) {
    rodaq_runtime_storage storage_service = {};
    storage_service.persistence_completed = persistence_completed;
    storage_service.report_failure = report_failure;

    EXPECT_NE(storage_service.persistence_completed, nullptr);
    EXPECT_NE(storage_service.report_failure, nullptr);
}

TEST(ScalarContractTest, CppStatusCodesMatchAbiValues) {
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::InvalidArgument), 1U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::InvalidState), 2U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::NotFound), 3U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::Conflict), 4U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::ResourceExhausted), 5U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::Unavailable), 6U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::StorageError), 7U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::Timeout), 8U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::InternalError), 9U);
}
