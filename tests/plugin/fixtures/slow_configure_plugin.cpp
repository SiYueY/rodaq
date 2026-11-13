#include <rodaq/plugin/export.hpp>
#include <rodaq/plugin/plugin.hpp>

#include <chrono>
#include <thread>

namespace {

class SlowConfigurePlugin final : public rodaq::plugin::PluginBase {
public:
    [[nodiscard]] rodaq::Result<void> configure(rodaq::plugin::PluginSetup&) override {
        std::this_thread::sleep_for(std::chrono::milliseconds{400});
        return {};
    }

    [[nodiscard]] rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override { return {}; }
};

}  // namespace

RODAQ_DEFINE_PLUGIN(SlowConfigurePlugin)
