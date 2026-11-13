#include "counter_interface.hpp"

#include <rodaq/plugin/export.hpp>
#include <rodaq/plugin/plugin.hpp>

#include <cstdint>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace {

std::atomic<int> destroy_count{0};

class CounterPlugin final : public rodaq::plugin::PluginBase {
public:
    ~CounterPlugin() override { destroy_count.fetch_add(1, std::memory_order_relaxed); }

    [[nodiscard]] rodaq::Result<void> configure(rodaq::plugin::PluginSetup& setup) override {
        auto sink = setup.service<rodaq::plugin::test::CounterSinkInterface>();
        if (!sink.ok()) {
            return sink.status();
        }
        sink_ = std::move(sink).value();
        counter_.set_sink(&sink_);
        return setup.provide<rodaq::plugin::test::CounterInterface>(counter_);
    }

    [[nodiscard]] rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override { return {}; }

private:
    class Counter final {
    public:
        void set_sink(rodaq::plugin::test::CounterSinkInterface::Reference* sink) noexcept {
            sink_ = sink;
        }

        std::int64_t increment(std::int64_t delta) {
            const auto value = value_.fetch_add(delta) + delta;
            if (sink_ != nullptr) {
                auto result = rodaq::plugin::test::record(*sink_, value);
                if (!result.ok()) {
                    throw std::runtime_error{"counter sink failed"};
                }
            }
            return value;
        }

        std::int64_t current() const noexcept { return value_.load(); }

    private:
        std::atomic<std::int64_t> value_{0};
        rodaq::plugin::test::CounterSinkInterface::Reference* sink_ = nullptr;
    };

    Counter counter_;
    rodaq::plugin::test::CounterSinkInterface::Reference sink_;
};

}  // namespace

RODAQ_DEFINE_PLUGIN(CounterPlugin)

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL int rodaq_test_counter_destroy_count(void) {
    return destroy_count.load(std::memory_order_relaxed);
}
