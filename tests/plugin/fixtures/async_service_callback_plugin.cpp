#include "counter_interface.hpp"

#include <rodaq/plugin/export.hpp>
#include <rodaq/plugin/plugin.hpp>

#include <atomic>
#include <thread>

namespace {

class AsyncServiceCallbackPlugin final : public rodaq::plugin::PluginBase {
public:
    [[nodiscard]] rodaq::Result<void> configure(rodaq::plugin::PluginSetup& setup) override {
        auto service = setup.service<rodaq::plugin::test::CounterSinkInterface>();
        if (!service.ok()) {
            return service.status();
        }
        service_ = std::move(service).value();
        worker_ = std::thread([this] { (void)rodaq::plugin::test::record(service_, 1); });
        return {};
    }

    [[nodiscard]] rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override {
        if (worker_.joinable()) {
            worker_.join();
        }
        return {};
    }

private:
    rodaq::plugin::test::CounterSinkInterface::Reference service_;
    std::thread worker_;
};

}  // namespace

RODAQ_DEFINE_PLUGIN(AsyncServiceCallbackPlugin)
