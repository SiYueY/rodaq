#include "runtime.hpp"

#include <rodaq/acquisition/interface.hpp>
#include <rodaq/plugin/authoring.hpp>

namespace rodaq::ros2_acquisition {

class Ros2Plugin final : public plugin::PluginBase {
public:
    Result<void> configure(plugin::PluginSetup& setup) override {
        auto configured = backend_.configure(setup.config());
        return configured.ok() ? setup.provide<acquisition::Interface>(backend_) : configured;
    }

    Result<void> shutdown(plugin::Deadline deadline) override { return backend_.quiesce(deadline); }

private:
    Backend backend_;
};

}  // namespace rodaq::ros2_acquisition

RODAQ_DEFINE_PLUGIN(rodaq::ros2_acquisition::Ros2Plugin)
