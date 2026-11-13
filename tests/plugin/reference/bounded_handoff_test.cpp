#include "bounded_handoff.hpp"
#include "test_support.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>

int main() {
    using namespace std::chrono_literals;
    std::mutex mutex;
    std::condition_variable condition;
    bool release_consumer = false;
    bool consumer_started = false;
    rodaq::plugin::reference_test::BoundedHandoff<int> handoff{
        4u, [&](int&&) {
            std::unique_lock<std::mutex> lock{mutex};
            consumer_started = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release_consumer; });
        }};

    RODAQ_TEST_CHECK(handoff.push(1));
    {
        std::unique_lock<std::mutex> lock{mutex};
        condition.wait(lock, [&] { return consumer_started; });
    }
    RODAQ_TEST_CHECK(handoff.push(2));
    RODAQ_TEST_CHECK(handoff.push(3));
    RODAQ_TEST_CHECK(handoff.push(4));
    RODAQ_TEST_CHECK(handoff.push(5));
    RODAQ_TEST_CHECK(!handoff.push(6));
    RODAQ_TEST_CHECK(handoff.dropped() == 1u);
    {
        std::lock_guard<std::mutex> lock{mutex};
        release_consumer = true;
    }
    condition.notify_all();
    handoff.stop();
    return 0;
}
