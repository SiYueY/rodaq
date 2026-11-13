#include <rodaq/plugin/export.hpp>
#include <rodaq/plugin/plugin.hpp>

#include <chrono>
#include <thread>

namespace {

class SlowShutdownPlugin final : public rodaq::plugin::PluginBase {
public:
    [[nodiscard]] rodaq::Result<void> configure(rodaq::plugin::PluginSetup&) override { return {}; }

    [[nodiscard]] rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        return {};
    }
};

}  // namespace

RODAQ_DEFINE_PLUGIN(SlowShutdownPlugin)
