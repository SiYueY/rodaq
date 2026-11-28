#include "lifecycle.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

namespace {

namespace plugin = rodaq::plugin;

class InitializationOwnership : public testing::Test {
protected:
    void SetUp() override {
        auto token = plugin::reserve_authority();
        ASSERT_TRUE(token.ok());
        plugin::PluginManagerOptions options;
        options.max_loaded_plugins = 1;
        options.max_instances = 1;
        options.max_config_size = 64;
        options.shutdown_timeout = std::chrono::seconds(1);
        manager_ = std::make_shared<plugin::ManagerState>(
            std::move(options), std::vector<plugin::CatalogEntry>{}, token.value());
        library_ = std::make_shared<plugin::LibraryState>();
        instance_ = std::make_shared<plugin::InstanceState>(manager_, library_, "{}", 0, 1);
        manager_->instances[0] = instance_;
        manager_->reserved_instances[0] = true;
        manager_->instance_count = 1;
        manager_->active_creates = 1;
    }

    std::shared_ptr<plugin::ManagerState> manager_;
    std::shared_ptr<plugin::LibraryState> library_;
    std::shared_ptr<plugin::InstanceState> instance_;
};

TEST_F(InitializationOwnership, UnvalidatedOpaqueOutputRemainsInReservedUnresolvedState) {
    instance_->native.context = reinterpret_cast<rodaq_plugin_context_t*>(1);
    instance_->native.vtable = reinterpret_cast<const rodaq_plugin_instance_vtable_t*>(2);
    plugin::fail_initialization(
        manager_, instance_, rodaq::Status(rodaq::StatusCode::Internal, {}), true);
    EXPECT_EQ(manager_->instances[0], instance_);
    EXPECT_TRUE(manager_->reserved_instances[0]);
    EXPECT_EQ(manager_->instance_count, 1U);
    EXPECT_EQ(manager_->active_creates, 0U);
    EXPECT_EQ(instance_->phase, plugin::PluginInstancePhase::Unresolved);
    EXPECT_EQ(instance_->native.context, reinterpret_cast<rodaq_plugin_context_t*>(1));
    EXPECT_EQ(instance_->functions.destroy, nullptr);
    EXPECT_TRUE(instance_->initialization.done);
    EXPECT_TRUE(instance_->terminal);
    EXPECT_TRUE(manager_->restart_required);
    EXPECT_TRUE(library_->faulted);
    EXPECT_TRUE(instance_->config.empty());
}

TEST_F(InitializationOwnership, OrdinaryDefaultInvalidFailureReleasesCapacityWithoutFault) {
    plugin::fail_initialization(
        manager_, instance_, rodaq::Status(rodaq::StatusCode::Unavailable, {}), false);
    EXPECT_FALSE(manager_->instances[0]);
    EXPECT_FALSE(manager_->reserved_instances[0]);
    EXPECT_EQ(manager_->instance_count, 0U);
    EXPECT_EQ(manager_->active_creates, 0U);
    EXPECT_FALSE(manager_->restart_required);
    EXPECT_FALSE(library_->faulted);
    EXPECT_EQ(instance_->initialization.result.code(), rodaq::StatusCode::Unavailable);
}

}  // namespace
