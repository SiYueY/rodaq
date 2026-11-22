#include "storage.hpp"

#include <rodaq/plugin/authoring.hpp>
#include <rodaq/storage/interface.hpp>

namespace rodaq::mcap_storage {

class McapPlugin final : public plugin::PluginBase {
public:
    Result<void> configure(plugin::PluginSetup& setup) override {
        auto configured = backend_.configure(setup.config());
        return configured.ok() ? setup.provide<storage::Interface>(backend_) : configured;
    }

    Result<void> shutdown(plugin::Deadline deadline) override { return backend_.quiesce(deadline); }

private:
    StorageBackend backend_;
};

}  // namespace rodaq::mcap_storage

RODAQ_DEFINE_PLUGIN(rodaq::mcap_storage::McapPlugin)
