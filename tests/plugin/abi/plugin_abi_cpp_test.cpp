#include "test_support.hpp"

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/plugin/interface.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/plugin.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <chrono>

int main() {
    using rodaq::plugin::Deadline;
    using rodaq::plugin::DeadlineDuration;

    RODAQ_TEST_CHECK(Deadline::infinite().is_infinite());
    RODAQ_TEST_CHECK(!Deadline::infinite().expired());
    RODAQ_TEST_CHECK(Deadline::after(DeadlineDuration{0}).expired());
    RODAQ_TEST_CHECK(Deadline::after(DeadlineDuration{-1}).expired());
    RODAQ_TEST_CHECK(Deadline::after(std::chrono::seconds{10}).nanoseconds() > 0);
    const auto saturated = Deadline::after(DeadlineDuration::max());
    RODAQ_TEST_CHECK(!saturated.is_infinite());
    RODAQ_TEST_CHECK(saturated.nanoseconds() == RODAQ_DEADLINE_INFINITE - 1);

    rodaq_interface_reference_t empty{};
    RODAQ_TEST_CHECK(empty.context == nullptr);
    RODAQ_TEST_CHECK(empty.vtable_size == 0u);
    return 0;
}
