#include "runtime.hpp"

#include <rodaq/acquisition/interface.hpp>
#include <rodaq/plugin/authoring.hpp>

namespace rodaq::ros2_acquisition {

class Ros2Plugin final : public plugin::PluginBase {
public:
    Result<void> configure(plugin::PluginSetup& setup) override {
        auto service = setup.service<acquisition::Service>(1);
        if (!service.ok()) {
            return service.status();
        }
        auto configured = backend_.configure(setup.config(), std::move(service.value()));
        return configured.ok() ? setup.provide<acquisition::Interface>(backend_) : configured;
    }

    Result<void> shutdown(plugin::Deadline deadline) override { return backend_.quiesce(deadline); }

private:
    Backend backend_;
};

}  // namespace rodaq::ros2_acquisition

RODAQ_DEFINE_PLUGIN(rodaq::ros2_acquisition::Ros2Plugin)
