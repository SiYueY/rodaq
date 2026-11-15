#include <counter.hpp>

#include <atomic>
#include <cstdint>
#include <limits>

class AtomicCounter {
public:
    rodaq::Result<std::int64_t> increment(std::int64_t delta) {
        auto previous = value_.load(std::memory_order_relaxed);
        for (;;) {
            if ((delta > 0 && previous > std::numeric_limits<std::int64_t>::max() - delta) ||
                (delta < 0 && previous < std::numeric_limits<std::int64_t>::min() - delta)) {
                return rodaq::Status(rodaq::StatusCode::InvalidArgument, "Counter overflow");
            }
            if (value_.compare_exchange_weak(
                    previous, previous + delta, std::memory_order_relaxed)) {
                return previous + delta;
            }
        }
    }

    rodaq::Result<std::int64_t> current() const { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::int64_t> value_{0};
};

class AtomicCounterPlugin final : public rodaq::plugin::PluginBase {
public:
    rodaq::Result<void> configure(rodaq::plugin::PluginSetup& setup) override {
        return setup.provide<CounterInterface>(counter_);
    }

    rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override { return {}; }

private:
    AtomicCounter counter_;
};

RODAQ_DEFINE_PLUGIN(AtomicCounterPlugin)
