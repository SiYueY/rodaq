#include <rodaq/plugin/export.hpp>
#include <rodaq/plugin/plugin.hpp>

namespace {

class ShutdownFailurePlugin final : public rodaq::plugin::PluginBase {
public:
    [[nodiscard]] rodaq::Result<void> configure(rodaq::plugin::PluginSetup&) override { return {}; }

    [[nodiscard]] rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override {
        return rodaq::Status{rodaq::StatusCode::Unavailable, "fixture shutdown failure"};
    }
};

}  // namespace

RODAQ_DEFINE_PLUGIN(ShutdownFailurePlugin)
