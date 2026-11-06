#include <rodaq/config/config.hpp>
#include <rodaq/daq.hpp>

#include <string>
#include <utility>
#include <vector>

int main() {
    auto tree = rodaq::ConfigTree::map(
        {{"enabled", rodaq::ConfigTree::boolean(true)},
         {"target", rodaq::ConfigTree::string("memory://combined")}});
    if (!tree.ok()) {
        return 1;
    }
    auto enabled = tree.value().root().find("enabled");
    if (!enabled.ok() || !enabled.value().as_bool().ok() || !enabled.value().as_bool().value()) {
        return 2;
    }

    rodaq::DAQConfig config;
    rodaq::DAQ daq(std::move(config));
    return daq.state() == rodaq::DAQState::Constructed ? 0 : 3;
}
