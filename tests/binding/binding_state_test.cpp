#include "binding/binding_state.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace rodaq {
namespace {

TEST(BindingStateTest, OwnsLifecycleAndInflightAccountingIndependently) {
    BindingEntry binding{
        StreamHandle::from_raw(7),
        StreamDescriptorHandle::from_raw(11),
        2,
        std::make_unique<std::atomic<std::uint32_t>>(
            static_cast<std::uint32_t>(BindingLifecycle::Prepared)),
        std::make_unique<std::atomic<std::uint64_t>>(0),
    };

    EXPECT_EQ(binding.stream, StreamHandle::from_raw(7));
    EXPECT_EQ(binding.descriptor, StreamDescriptorHandle::from_raw(11));
    EXPECT_EQ(binding.plugin_index, 2U);
    EXPECT_EQ(binding.lifecycle->load(), static_cast<std::uint32_t>(BindingLifecycle::Prepared));
    EXPECT_EQ(binding.admission_inflight->load(), 0U);

    binding.lifecycle->store(static_cast<std::uint32_t>(BindingLifecycle::Active));
    binding.admission_inflight->fetch_add(3);
    EXPECT_EQ(binding.lifecycle->load(), static_cast<std::uint32_t>(BindingLifecycle::Active));
    EXPECT_EQ(binding.admission_inflight->load(), 3U);
}

}  // namespace
}  // namespace rodaq
