#include <rodaq/plugin/registry.hpp>

#include "abi/counter.hpp"
#include "registry_state.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

class RegistryCounter {
public:
    rodaq::Result<std::int64_t> increment(std::int64_t value) { return value; }

    rodaq::Result<std::int64_t> current() const { return 0; }
};

TEST(ServiceRegistry, SharedOwnershipMoveAndFreeze) {
    std::weak_ptr<RegistryCounter> weak;
    {
        rodaq::plugin::ServiceRegistry registry;
        auto provider = std::make_shared<RegistryCounter>();
        weak = provider;
        EXPECT_TRUE(registry.add_shared<CounterInterface>(std::move(provider)).ok());
        EXPECT_FALSE(weak.expired());
        auto moved = std::move(registry);
        // Intentional verification of defined moved-from state.
        EXPECT_FALSE(registry.frozen());  // NOLINT(bugprone-use-after-move)
        EXPECT_TRUE(moved.freeze().ok());
        EXPECT_TRUE(moved.frozen());
        EXPECT_TRUE(moved.freeze().ok());
        EXPECT_EQ(
            moved.add_shared<CounterInterface>(std::make_shared<RegistryCounter>()).status().code(),
            rodaq::StatusCode::InvalidState);
    }
    EXPECT_TRUE(weak.expired());
}

TEST(ServiceRegistry, RejectedProviderDeleterRunsOutsideStructuralLock) {
    rodaq::plugin::ServiceRegistry registry;
    EXPECT_TRUE(registry.add_shared<CounterInterface>(std::make_shared<RegistryCounter>()).ok());
    bool reentered = false;
    auto duplicate =
        std::shared_ptr<RegistryCounter>(new RegistryCounter, [&](RegistryCounter* value) {
            reentered = registry.freeze().ok();
            delete value;
        });
    const auto result = registry.add_shared<CounterInterface>(std::move(duplicate));
    EXPECT_EQ(result.status().code(), rodaq::StatusCode::Conflict);
    EXPECT_TRUE(reentered);
}

TEST(ServiceRegistry, FrozenNativeLookupIsConcurrentAndBounded) {
    rodaq::plugin::RegistryState registry;
    RegistryCounter counter;
    ASSERT_TRUE(
        rodaq::plugin::authoring::add_interface<CounterInterface>(registry.entries, counter).ok());
    registry.frozen.store(true, std::memory_order_release);
    std::atomic<bool> valid{true};
    std::vector<std::thread> readers;
    for (int worker = 0; worker < 8; ++worker) {
        readers.emplace_back([&] {
            const rodaq_interface_query_t query{
                {RODAQ_COUNTER_INTERFACE_ID, sizeof(RODAQ_COUNTER_INTERFACE_ID) - 1}, 1, 0, 0};
            for (int i = 0; i < 1000; ++i) {
                rodaq_interface_reference_t reference{};
                const auto code =
                    rodaq::plugin::registry_query(registry, &query, &reference, nullptr);
                if (code != RODAQ_STATUS_OK || reference.context == nullptr ||
                    reference.negotiated_version.major != 1 ||
                    reference.negotiated_version.minor != 0) {
                    valid.store(false);
                }
            }
        });
    }
    for (auto& reader : readers) {
        reader.join();
    }
    EXPECT_TRUE(valid.load());
}

}  // namespace
