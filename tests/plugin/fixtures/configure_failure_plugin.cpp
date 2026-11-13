#include <rodaq/plugin/export.hpp>
#include <rodaq/plugin/plugin.hpp>

namespace {

class ConfigureFailurePlugin final : public rodaq::plugin::PluginBase {
public:
    [[nodiscard]] rodaq::Result<void> configure(rodaq::plugin::PluginSetup&) override {
        return rodaq::Status{
            rodaq::StatusCode::InvalidArgument, "configuration rejected by fixture"};
    }

    [[nodiscard]] rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override { return {}; }
};

}  // namespace

RODAQ_DEFINE_PLUGIN(ConfigureFailurePlugin)
